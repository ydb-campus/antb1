#include "part_operators.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <numeric>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/array/concatenate.h>
#include <arrow/compute/api_vector.h>
#include <arrow/util/future.h>
#include <arrow/util/thread_pool.h>

#include "antb1/common/check.h"
#include "antb1/exec/operator.h"
#include "antb1/exec/profile.h"
#include "antb1/exec/sort.h"
#include "antb1/exec/table_scan.h"
#include "antb1/plan/logical_plan.h"

#include "aggregate_set.h"
#include "heavy_hitters.h"
#include "outer_groups.h"
#include "part_scheduler.h"
#include "partition_lanes.h"

namespace antb1::exec {
namespace {

// A part runs single-threaded: its operators never see the executor.
ExecContext PartContext(const ExecContext& ctx) {
  return ExecContext{.pool = ctx.pool,
                     .batch_size = ctx.batch_size,
                     .executor = nullptr,
                     .threads = 1,
                     .budget = ctx.budget};
}

// The number of parts that may run ahead of the consumer.
int64_t Window(const ExecContext& ctx) {
  return ctx.executor == nullptr ? 1 : int64_t{2} * (ctx.threads < 1 ? 1 : ctx.threads);
}

// Opens the part's pipeline, hands every batch with selected rows to `consume` until it returns
// false or the pipeline ends, and closes the pipeline. Once `stop` is set the part is abandoned
// (its status is dropped by the scheduler).
arrow::Status RunPart(const PartPipeline& pipeline, int64_t part, ExecContext ctx,
                      const std::atomic<bool>& stop,
                      const std::function<arrow::Result<bool>(Batch)>& consume) {
  ARROW_ASSIGN_OR_RAISE(std::unique_ptr<Operator> op, pipeline(part));
  arrow::Status status = op->Open(ctx);
  while (status.ok()) {
    if (stop) {
      status = arrow::Status::Cancelled("the query stopped reading parts");
      break;
    }
    arrow::Result<Batch> batch = op->Next();
    if (!batch.ok()) {
      status = batch.status();
      break;
    }
    if (batch->end()) {
      break;
    }
    if (batch->selected_rows() == 0) {
      continue;
    }
    arrow::Result<bool> more = consume(*std::move(batch));
    if (!more.ok()) {
      status = more.status();
    } else if (!*more) {
      break;
    }
  }
  arrow::Status closed = op->Close();
  ARROW_RETURN_NOT_OK(status);
  return closed;
}

// Runs fn(0) .. fn(n - 1) on the executor (here, one after another, without one) and waits for all
// of them: the first failure in index order decides the status.
arrow::Status ForEach(arrow::internal::Executor* executor, std::size_t n,
                      const std::function<arrow::Status(std::size_t)>& fn) {
  const auto guarded = [&fn](std::size_t i) -> arrow::Status {
    try {
      return fn(i);
    } catch (const std::bad_alloc&) {
      return arrow::Status::OutOfMemory("out of memory while merging groups");
    }
  };
  if (executor == nullptr || n <= 1) {
    for (std::size_t i = 0; i < n; ++i) {
      ARROW_RETURN_NOT_OK(guarded(i));
    }
    return arrow::Status::OK();
  }
  std::vector<arrow::Future<>> tasks;
  tasks.reserve(n);
  arrow::Status submitted;
  for (std::size_t i = 0; i < n && submitted.ok(); ++i) {
    auto task = executor->Submit([&guarded, i] { return guarded(i); });
    if (task.ok()) {
      tasks.push_back(std::move(*task));
    } else {
      submitted = task.status();
    }
  }
  arrow::Status status = submitted;
  for (const arrow::Future<>& task : tasks) {  // every task ends before its inputs can go
    const arrow::Status done = task.status();
    if (status.ok()) {
      status = done;
    }
  }
  return status;
}

}  // namespace

// ---- PartUnionOperator ----

PartUnionOperator::PartUnionOperator(PartPipeline pipeline, int64_t num_parts,
                                     std::shared_ptr<arrow::Schema> schema,
                                     std::optional<int64_t> row_cap)
    : pipeline_(std::make_shared<const PartPipeline>(std::move(pipeline))),
      num_parts_(num_parts),
      schema_(std::move(schema)),
      row_cap_(row_cap) {}

PartUnionOperator::~PartUnionOperator() = default;

arrow::Status PartUnionOperator::Open(ExecContext& ctx) {
  ARROW_RETURN_NOT_OK(Close());
  auto task = [pipeline = pipeline_, part_ctx = PartContext(ctx), cap = row_cap_,
               profile = profile()](int64_t part,
                                    const std::atomic<bool>& stop) -> arrow::Result<PartBatches> {
    const ProfileTimer part_time(profile, "part_time");
    auto batches = std::make_shared<std::vector<Batch>>();
    int64_t rows = 0;
    if (cap.has_value() && *cap <= 0) {
      return batches;
    }
    ARROW_RETURN_NOT_OK(
        RunPart(*pipeline, part, part_ctx, stop, [&](Batch batch) -> arrow::Result<bool> {
          rows += batch.selected_rows();
          batches->push_back(std::move(batch));
          return !cap.has_value() || rows < *cap;
        }));
    return batches;
  };
  scheduler_ = std::make_unique<PartScheduler<PartBatches>>(num_parts_, std::move(task),
                                                            ctx.executor, Window(ctx), ctx.budget);
  return arrow::Status::OK();
}

arrow::Result<Batch> PartUnionOperator::Next() {
  if (scheduler_ == nullptr) {
    return arrow::Status::Invalid("part union: Next() before Open()");
  }
  while (true) {
    if (current_ != nullptr && next_ < current_->size()) {
      return std::move((*current_)[next_++]);  // released as it is handed on
    }
    current_.reset();
    next_ = 0;
    if (scheduler_->done()) {
      return Batch{};
    }
    {
      const ProfileTimer wait(profile(), "wait");
      ARROW_ASSIGN_OR_RAISE(current_, scheduler_->Next());
    }
  }
}

arrow::Status PartUnionOperator::Close() {
  scheduler_.reset();  // stops the parts still running and waits for them
  current_.reset();
  next_ = 0;
  return arrow::Status::OK();
}

// ---- PartAggregateOperator ----

PartAggregateOperator::PartAggregateOperator(PartPipeline pipeline, int64_t num_parts,
                                             int input_width,
                                             std::vector<plan::AggregateCall> aggregates)
    : pipeline_(std::make_shared<const PartPipeline>(std::move(pipeline))),
      num_parts_(num_parts),
      input_width_(input_width),
      aggregates_(std::make_shared<const std::vector<plan::AggregateCall>>(std::move(aggregates))),
      schema_(AggregateSet::ResultSchema(*aggregates_)) {}

PartAggregateOperator::~PartAggregateOperator() = default;

arrow::Status PartAggregateOperator::Open(ExecContext& ctx) {
  ARROW_RETURN_NOT_OK(Close());
  pool_ = ctx.pool;
  done_ = false;
  auto task = [pipeline = pipeline_, part_ctx = PartContext(ctx), aggregates = aggregates_,
               width = input_width_, profile = profile()](
                  int64_t part, const std::atomic<bool>& stop) -> arrow::Result<PartStates> {
    const ProfileTimer part_time(profile, "part_time");
    ARROW_ASSIGN_OR_RAISE(AggregateSet states,
                          AggregateSet::Make(*aggregates, width, part_ctx.pool));
    auto shared = std::make_shared<AggregateSet>(std::move(states));
    ARROW_RETURN_NOT_OK(
        RunPart(*pipeline, part, part_ctx, stop, [&](const Batch& batch) -> arrow::Result<bool> {
          ARROW_RETURN_NOT_OK(shared->Consume(batch));
          return true;
        }));
    return shared;
  };
  scheduler_ = std::make_unique<PartScheduler<PartStates>>(num_parts_, std::move(task),
                                                           ctx.executor, Window(ctx), ctx.budget);
  return arrow::Status::OK();
}

arrow::Result<Batch> PartAggregateOperator::Next() {
  if (done_) {
    return Batch{};
  }
  if (scheduler_ == nullptr) {
    return arrow::Status::Invalid("part aggregate: Next() before Open()");
  }
  ARROW_ASSIGN_OR_RAISE(AggregateSet total, AggregateSet::Make(*aggregates_, input_width_, pool_));
  while (!scheduler_->done()) {
    PartStates part;
    {
      const ProfileTimer wait(profile(), "wait");
      ARROW_ASSIGN_OR_RAISE(part, scheduler_->Next());
    }
    const ProfileTimer merge(profile(), "merge");
    ARROW_RETURN_NOT_OK(total.Merge(*part));
  }
  done_ = true;
  ARROW_ASSIGN_OR_RAISE(auto row, total.Finalize(schema_, pool_));
  return Batch{.data = std::move(row), .selection = {}};
}

arrow::Status PartAggregateOperator::Close() {
  scheduler_.reset();
  done_ = true;
  return arrow::Status::OK();
}

namespace {

// Whether a part's rows may go straight to the partitions (unaggregated) when its own table does
// not reduce them: with keys, and without a DOUBLE SUM or AVG, whose rounding follows the parts
// (docs/adr/0013-parallel-execution.md), or a HUGEINT one or one of a DECIMAL beyond 18 digits.
bool MayRouteRows(const std::vector<plan::BoundColumn>& keys,
                  const std::vector<plan::AggregateCall>& calls) {
  return !keys.empty() && std::ranges::none_of(calls, [](const plan::AggregateCall& call) {
    // These sums check for a 128-bit overflow at every addition, so the grouping of the additions
    // decides whether a query fails (values of at most 18 digits cannot reach it).
    if ((call.kind != plan::AggKind::kSum && call.kind != plan::AggKind::kAvg) ||
        !call.arg.has_value()) {
      return false;
    }
    const plan::LogicalType type = call.arg->type;
    return type == plan::LogicalType::kDouble || type == plan::LogicalType::kHugeInt ||
           (type == plan::LogicalType::kDecimal && type.width() > 18);
  });
}

// The rows a part aggregates on its own before it decides whether to route the rest: one batch of
// the default size, so that the decision sees the part's repetition (its first few thousand rows
// look more distinct than the part).
constexpr int64_t kRouteDecisionRows = int64_t{64} * 1024;

}  // namespace

// ---- PartGroupAggregateOperator ----

PartGroupAggregateOperator::PartGroupAggregateOperator(PartPipeline pipeline, int64_t num_parts,
                                                       int input_width,
                                                       std::vector<plan::BoundColumn> keys,
                                                       std::vector<plan::AggregateCall> aggregates,
                                                       std::shared_ptr<arrow::Schema> schema,
                                                       std::optional<PartitionTopN> top_n)
    : pipeline_(std::make_shared<const PartPipeline>(std::move(pipeline))),
      num_parts_(num_parts),
      input_width_(input_width),
      keys_(std::move(keys)),
      aggregates_(std::move(aggregates)),
      schema_(std::move(schema)),
      top_n_(std::move(top_n)) {}

PartGroupAggregateOperator::~PartGroupAggregateOperator() = default;

arrow::Status PartGroupAggregateOperator::Open(ExecContext& ctx) {
  ARROW_RETURN_NOT_OK(Close());
  if (top_n_.has_value() && !comparator_.has_value()) {
    ARROW_ASSIGN_OR_RAISE(comparator_, RowComparator::Make(schema_, top_n_->keys));
  }
  pool_ = ctx.pool;
  budget_ = ctx.budget;
  executor_ = ctx.executor;
  window_ = Window(ctx);
  threads_ = ctx.executor == nullptr ? 1 : static_cast<std::size_t>(std::max(ctx.threads, 1));
  merged_ = false;
  opened_ = true;
  auto task = [pipeline = pipeline_, part_ctx = PartContext(ctx), keys = keys_,
               aggregates = aggregates_, width = input_width_, profile = profile(),
               may_route = MayRouteRows(keys_, aggregates_)](
                  int64_t part, const std::atomic<bool>& stop) -> arrow::Result<PartTable> {
    const ProfileTimer part_time(profile, "part_time");
    auto groups = std::make_shared<PartGroups>();
    ARROW_ASSIGN_OR_RAISE(
        groups->table, GroupTable::Make(keys, aggregates, width, part_ctx.pool, part_ctx.budget));
    GroupTable& table = *groups->table;
    // After its first kRouteDecisionRows rows (or batch), a part whose groups are more than 1/4 of
    // its rows stops aggregating on its own: its other rows go straight to the partitions. A part
    // that reduces less than 4 to 1 would insert most of its groups twice (its table, then the
    // partition's), which costs more than inserting its rows once in the partitions. The decision
    // waits for enough rows (a selective filter can leave a batch a few rows, which never reduce),
    // and is made once, from the part's data alone.
    const int64_t decide_after = std::min<int64_t>(kRouteDecisionRows, part_ctx.batch_size);
    bool decided = !may_route;
    int64_t consumed = 0;
    bool route = false;
    int64_t routed = 0;
    ARROW_RETURN_NOT_OK(
        RunPart(*pipeline, part, part_ctx, stop, [&](const Batch& batch) -> arrow::Result<bool> {
          ARROW_ASSIGN_OR_RAISE(const auto rows, Materialize(batch, part_ctx.pool));
          if (route) {
            // Routed rows are Arrow buffers of the query's pool: the budget counts them.
            ARROW_ASSIGN_OR_RAISE(auto split, table.RouteRows(*rows));
            for (std::size_t p = 0; p < split.size(); ++p) {
              if (split[p]->num_rows() > 0) {
                groups->rows[p].push_back(std::move(split[p]));
              }
            }
            routed += rows->num_rows();
            return true;
          }
          ARROW_RETURN_NOT_OK(table.Consume(*rows));
          consumed += rows->num_rows();
          if (!decided && consumed >= decide_after) {
            decided = true;
            if (int64_t{table.num_groups()} * 4 > consumed) {
              route = true;
              groups->rows.resize(GroupTable::kPartitions);
            }
          }
          return true;
        }));
    ARROW_RETURN_NOT_OK(table.Partition());  // on the worker, not on the merging thread
    if (profile != nullptr && routed > 0) {
      profile->Add("raw_parts", MetricUnit::kCount, 1);
      profile->Add("raw_rows", MetricUnit::kCount, routed);
    }
    return groups;
  };
  scheduler_ = std::make_unique<PartScheduler<PartTable>>(num_parts_, std::move(task), ctx.executor,
                                                          Window(ctx), ctx.budget);
  return arrow::Status::OK();
}

arrow::Status PartGroupAggregateOperator::Merge() {
  // Partitions are disjoint: each merges the parts in part order in its own lane.
  std::unique_ptr<PartitionLanes> lanes;
  // A part that runs out of memory runs again alone once the earlier parts are merged and freed.
  scheduler_->set_before_retry([&lanes] {
    if (lanes != nullptr) {
      lanes->Wait();
    }
  });
  arrow::Status status;
  for (int64_t index = 0; !scheduler_->done(); ++index) {
    arrow::Result<PartTable> part = [&] {
      const ProfileTimer wait(profile(), "wait");
      return scheduler_->Next();
    }();
    if (!part.ok()) {
      status = part.status();
      break;
    }
    const std::size_t partitions = (*part)->table->num_partitions();
    if (lanes == nullptr) {
      tables_.reserve(partitions);
      for (std::size_t p = 0; p < partitions; ++p) {
        ARROW_ASSIGN_OR_RAISE(auto table,
                              GroupTable::Make(keys_, aggregates_, input_width_, pool_, budget_));
        tables_.push_back(std::move(table));
      }
      lanes = std::make_unique<PartitionLanes>(partitions, executor_, window_, budget_);
    }
    ANTB1_CHECK(tables_.size() == partitions);
    status = lanes->Add(index, [this, groups = *std::move(part)](std::size_t p) -> arrow::Status {
      ARROW_RETURN_NOT_OK(tables_[p]->MergePartition(*groups->table, p));
      if (p < groups->rows.size()) {  // the part's other rows, after its first batch's groups
        for (const auto& rows : groups->rows[p]) {
          ARROW_RETURN_NOT_OK(tables_[p]->Consume(*rows));
        }
      }
      return arrow::Status::OK();
    });
    if (!status.ok()) {
      break;
    }
  }
  scheduler_->set_before_retry(nullptr);
  // A merge failure is of an earlier part than any failure of the scheduler seen after it.
  if (lanes != nullptr) {
    const ProfileTimer tail(profile(), "lanes_tail");  // merging after the last part came
    ARROW_RETURN_NOT_OK(lanes->Finish());
  }
  return status;
}

arrow::Result<Batch> PartGroupAggregateOperator::Next() {
  if (!opened_) {
    return arrow::Status::Invalid("part group aggregate: Next() before Open() or after Close()");
  }
  if (!merged_) {
    ARROW_RETURN_NOT_OK(Merge());
    merged_ = true;
    scheduler_.reset();
    rows_.assign(tables_.size(), {});
    if (profile() != nullptr) {
      int64_t groups = 0;
      for (const auto& table : tables_) {
        groups += table->num_groups();
      }
      profile()->Add("groups", MetricUnit::kCount, groups);
    }
  }
  for (; next_table_ < rows_.size(); ++next_table_, next_chunk_ = 0) {
    if (next_table_ == built_) {
      const ProfileTimer build(profile(), "build");
      ARROW_RETURN_NOT_OK(BuildRows());
    }
    std::vector<std::shared_ptr<arrow::RecordBatch>>& rows = rows_[next_table_];
    if (next_chunk_ < rows.size()) {
      return Batch{.data = std::move(rows[next_chunk_++]), .selection = {}};
    }
    rows = {};  // gives the memory back
  }
  return Batch{};
}

arrow::Status PartGroupAggregateOperator::BuildRows() {
  // The next partitions' rows, in parallel: as many partitions as threads (one under memory
  // pressure), so that the rows built ahead of the consumer stay bounded. Each table goes once
  // its rows are built.
  const bool pressure = budget_ != nullptr && budget_->under_pressure();
  const std::size_t wave = pressure ? 1 : std::max<std::size_t>(1, threads_);
  const std::size_t first = built_;
  const std::size_t count = std::min(wave, tables_.size() - first);
  built_ += count;
  return ForEach(executor_, count, [this, first](std::size_t i) -> arrow::Status {
    const std::size_t p = first + i;
    while (true) {
      ARROW_ASSIGN_OR_RAISE(auto chunk, tables_[p]->NextChunk(schema_));
      if (chunk == nullptr) {
        break;
      }
      rows_[p].push_back(std::move(chunk));
    }
    tables_[p].reset();
    if (top_n_.has_value() && comparator_.has_value()) {
      // Only the partition's first rows in the top-N's order go on; the others are freed here.
      ARROW_ASSIGN_OR_RAISE(
          rows_[p], KeepFirstRows(std::move(rows_[p]), *comparator_, top_n_->keep, pool_, budget_));
    }
    return arrow::Status::OK();
  });
}

arrow::Result<std::vector<std::shared_ptr<arrow::RecordBatch>>> KeepFirstRows(
    std::vector<std::shared_ptr<arrow::RecordBatch>> rows, const RowComparator& comparator,
    int64_t keep, arrow::MemoryPool* pool, MemoryBudget* budget) {
  int64_t total = 0;
  for (const auto& chunk : rows) {
    total += chunk->num_rows();
  }
  if (total <= keep) {
    return rows;
  }
  SortBuffer buffer(comparator, keep);
  MemoryReservation memory;
  memory.Reset(budget);
  for (auto& chunk : rows) {
    ARROW_RETURN_NOT_OK(buffer.Add(std::move(chunk), pool));
    ARROW_RETURN_NOT_OK(memory.Resize(buffer.memory_usage()));
  }
  rows.clear();
  ARROW_RETURN_NOT_OK(memory.Resize(buffer.memory_usage() + buffer.sort_memory()));
  ARROW_RETURN_NOT_OK(buffer.Sort(pool));
  ARROW_RETURN_NOT_OK(memory.Resize(buffer.memory_usage()));
  ARROW_ASSIGN_OR_RAISE(auto kept, buffer.Slice(0, buffer.num_rows(), pool));
  rows.push_back(std::move(kept));
  return rows;
}

arrow::Status PartGroupAggregateOperator::Close() {
  scheduler_.reset();
  tables_.clear();
  rows_.clear();
  next_table_ = 0;
  next_chunk_ = 0;
  built_ = 0;
  opened_ = false;
  return arrow::Status::OK();
}

// ---- PartTwoLevelAggregateOperator ----

namespace {

// The summary of the sample's K hashes: every K with more than 1 / (2 * kPartitions) of the
// sample's inner groups is heavy (it would take more than half a fair partition's share).
constexpr std::size_t kHeavyCounters = 512;
constexpr double kHeavyShare = 1.0 / (2.0 * static_cast<double>(GroupTable::kPartitions));

// What every part builds its inner tables from.
struct InnerSpec {
  std::vector<plan::BoundColumn> keys;
  std::vector<plan::BoundColumn> distinct;
  std::vector<plan::AggregateCall> plain;
  int width = 0;
};

// One inner table per distinct column (keys K and the column, no calls), then the plain table (K,
// the plain calls) if there are plain calls. They keep no first-seen keys: the outer level reads
// their normalized keys, and no key it outputs is DOUBLE.
arrow::Result<std::vector<std::unique_ptr<GroupTable>>> MakeInnerTables(const InnerSpec& spec,
                                                                        arrow::MemoryPool* pool,
                                                                        MemoryBudget* budget) {
  std::vector<std::unique_ptr<GroupTable>> tables;
  for (const plan::BoundColumn& column : spec.distinct) {
    std::vector<plan::BoundColumn> keys = spec.keys;
    keys.push_back(column);
    ARROW_ASSIGN_OR_RAISE(auto table, GroupTable::Make(std::move(keys), {}, spec.width, pool,
                                                       budget, /*first_keys=*/false));
    tables.push_back(std::move(table));
  }
  if (!spec.plain.empty()) {
    ARROW_ASSIGN_OR_RAISE(auto table, GroupTable::Make(spec.keys, spec.plain, spec.width, pool,
                                                       budget, /*first_keys=*/false));
    tables.push_back(std::move(table));
  }
  return tables;
}

}  // namespace

bool TwoLevelAggregation(const std::vector<plan::BoundColumn>& keys,
                         const std::vector<plan::AggregateCall>& calls) {
  if (std::ranges::any_of(keys, [](const plan::BoundColumn& key) {
        return key.type == plan::LogicalType::kDouble;
      })) {
    return false;
  }
  const auto exact_integer = [](plan::LogicalType type) {
    return plan::IsInteger(type) && type != plan::LogicalType::kHugeInt;
  };
  bool distinct = false;
  for (const plan::AggregateCall& call : calls) {
    const std::optional<plan::LogicalType> input =
        call.arg.has_value() ? std::optional(call.arg->type) : std::nullopt;
    bool order_independent = false;
    if (call.kind == plan::AggKind::kCountDistinct) {
      order_independent =
          call.arg.has_value() && std::ranges::none_of(keys, [&](const plan::BoundColumn& key) {
            return key.index == call.arg->index;
          });
      distinct = true;
    } else if (call.kind == plan::AggKind::kCountStar || call.kind == plan::AggKind::kCount) {
      order_independent = true;
    } else if (call.kind == plan::AggKind::kMin || call.kind == plan::AggKind::kMax) {
      // -0.0 and 0.0 compare equal: which one a DOUBLE MIN keeps depends on the order.
      order_independent = input.has_value() && *input != plan::LogicalType::kDouble;
    } else if (call.kind == plan::AggKind::kSum) {
      order_independent = input.has_value() && exact_integer(*input);
    } else if (call.kind == plan::AggKind::kAvg) {
      order_independent =
          input.has_value() && (exact_integer(*input) || *input == plan::LogicalType::kDate ||
                                *input == plan::LogicalType::kTimestamp);
    }
    if (!order_independent) {
      return false;
    }
  }
  return distinct;
}

PartTwoLevelAggregateOperator::PartTwoLevelAggregateOperator(
    PartPipeline pipeline, int64_t num_parts, int64_t sample_parts, int input_width,
    std::vector<plan::BoundColumn> keys, std::vector<plan::AggregateCall> aggregates,
    std::shared_ptr<arrow::Schema> schema, bool global)
    : pipeline_(std::make_shared<const PartPipeline>(std::move(pipeline))),
      num_parts_(num_parts),
      sample_parts_(sample_parts),
      input_width_(input_width),
      keys_(std::move(keys)),
      aggregates_(std::move(aggregates)),
      schema_(std::move(schema)),
      global_(global) {
  for (const plan::BoundColumn& key : keys_) {
    key_types_.push_back(key.type);
  }
  for (const plan::AggregateCall& call : aggregates_) {
    if (call.kind == plan::AggKind::kCountDistinct && call.arg.has_value()) {
      auto it = std::ranges::find_if(distinct_, [&](const plan::BoundColumn& column) {
        return column.index == call.arg->index;
      });
      if (it == distinct_.end()) {
        distinct_.push_back(*call.arg);
        it = distinct_.end() - 1;
      }
      slots_.push_back(
          OuterSlot{.distinct = true, .index = static_cast<std::size_t>(it - distinct_.begin())});
    } else {
      plain_.push_back(call);
      slots_.push_back(OuterSlot{.distinct = false, .index = plain_.size() - 1});
    }
  }
}

PartTwoLevelAggregateOperator::~PartTwoLevelAggregateOperator() = default;

arrow::Status PartTwoLevelAggregateOperator::Open(ExecContext& ctx) {
  ARROW_RETURN_NOT_OK(Close());
  if (!TwoLevelAggregation(keys_, aggregates_) || (global_ && !keys_.empty())) {
    return arrow::Status::Invalid("a two-level aggregation of calls it cannot run");
  }
  pool_ = ctx.pool;
  budget_ = ctx.budget;
  executor_ = ctx.executor;
  part_ctx_ = PartContext(ctx);
  window_ = Window(ctx);
  opened_ = true;
  return arrow::Status::OK();
}

PartitionLanes::Merge PartTwoLevelAggregateOperator::MergeOf(PartTables inner) {
  ANTB1_CHECK(inner->tables.size() == tables_.size());
  return [this, part = std::move(inner)](std::size_t p) -> arrow::Status {
    const std::vector<std::unique_ptr<GroupTable>>& tables = part->tables;
    for (std::size_t t = 0; t < tables.size(); ++t) {
      if (p < tables[t]->num_partitions()) {  // a plain table without keys has one partition
        ARROW_RETURN_NOT_OK(tables_[t][p]->MergePartition(*tables[t], p));
      }
    }
    return arrow::Status::OK();
  };
}

arrow::Status PartTwoLevelAggregateOperator::Outer(std::size_t partition) {
  ARROW_ASSIGN_OR_RAISE(auto outer,
                        OuterGroups::Make(key_types_, distinct_.size(), plain_, pool_, budget_));
  for (std::size_t t = 0; t < tables_.size(); ++t) {
    const std::unique_ptr<GroupTable> inner = std::move(tables_[t][partition]);  // freed here
    if (t < distinct_.size()) {
      ARROW_ASSIGN_OR_RAISE(const arrow::compute::ExecBatch uniques, inner->uniques());
      ARROW_RETURN_NOT_OK(outer->AddDistinct(t, uniques));
    } else {
      ARROW_RETURN_NOT_OK(outer->AddPlain(*inner));
    }
  }
  // Without keys the one group is heavy: its inner groups are in every partition.
  std::vector<bool> heavy(outer->num_groups(), keys_.empty());
  if (!keys_.empty() && !heavy_.empty()) {
    ARROW_ASSIGN_OR_RAISE(const std::vector<std::uint64_t> hashes, outer->Hashes());
    for (std::size_t g = 0; g < hashes.size(); ++g) {
      heavy[g] = std::ranges::binary_search(heavy_, hashes[g]);
    }
  }
  // The light groups' rows, in chunks of at most kMaxMergeChunk groups.
  const std::uint32_t groups = outer->num_groups();
  for (std::uint32_t begin = 0; begin < groups;) {
    const auto end = static_cast<std::uint32_t>(
        std::min<std::size_t>(groups, begin + GroupTable::kMaxMergeChunk));
    std::vector<std::uint32_t> chunk;
    chunk.reserve(end - begin);
    for (std::uint32_t g = begin; g < end; ++g) {
      (heavy[g] ? heavy_groups_[partition] : chunk).push_back(g);
    }
    if (!chunk.empty()) {
      ARROW_ASSIGN_OR_RAISE(auto batch, outer->Rows(begin, end, chunk, slots_, schema_, !global_));
      rows_[partition].push_back(std::move(batch));
    }
    begin = end;
  }
  outer_[partition] = std::move(outer);
  return arrow::Status::OK();
}

arrow::Status PartTwoLevelAggregateOperator::Aggregate() {
  const auto spec = std::make_shared<const InnerSpec>(
      InnerSpec{.keys = keys_, .distinct = distinct_, .plain = plain_, .width = input_width_});
  const std::size_t prefix = keys_.size();
  // The parts from `first` on. A sampled part hashes its K; the others are partitioned by the
  // heavy keys. Both on the worker, not on the merging thread.
  const auto run = [&](int64_t first, int64_t count,
                       std::shared_ptr<const std::vector<std::uint64_t>> heavy_keys) {
    auto task = [pipeline = pipeline_, part_ctx = part_ctx_, spec, first, prefix,
                 heavy = std::move(heavy_keys), profile = profile()](
                    int64_t i, const std::atomic<bool>& stop) -> arrow::Result<PartTables> {
      const ProfileTimer part_time(profile, "part_time");
      auto part = std::make_shared<InnerPart>();
      ARROW_ASSIGN_OR_RAISE(part->tables, MakeInnerTables(*spec, part_ctx.pool, part_ctx.budget));
      ARROW_RETURN_NOT_OK(RunPart(
          *pipeline, first + i, part_ctx, stop, [&](const Batch& batch) -> arrow::Result<bool> {
            ARROW_ASSIGN_OR_RAISE(const auto rows, Materialize(batch, part_ctx.pool));
            for (const auto& table : part->tables) {
              ARROW_RETURN_NOT_OK(table->Consume(*rows));
            }
            return true;
          }));
      if (heavy != nullptr) {
        for (const auto& table : part->tables) {
          ARROW_RETURN_NOT_OK(table->Partition(prefix, *heavy));
        }
      } else if (prefix > 0) {
        for (std::size_t t = 0; t < spec->distinct.size(); ++t) {
          ARROW_ASSIGN_OR_RAISE(const std::vector<std::uint64_t> hashes,
                                part->tables[t]->PrefixHashes(prefix));
          part->key_hashes.insert(part->key_hashes.end(), hashes.begin(), hashes.end());
        }
      }
      return part;
    };
    return std::make_unique<PartScheduler<PartTables>>(count, std::move(task), executor_, window_,
                                                       budget_);
  };

  // Per inner table, one merged table per partition.
  tables_.clear();
  for (std::size_t p = 0; p < GroupTable::kPartitions; ++p) {
    ARROW_ASSIGN_OR_RAISE(auto made, MakeInnerTables(*spec, pool_, budget_));
    tables_.resize(made.size());
    for (std::size_t t = 0; t < made.size(); ++t) {
      tables_[t].push_back(std::move(made[t]));
    }
  }

  // The sample: its parts in part order decide the heavy keys, then partition and merge.
  const int64_t sample = std::clamp<int64_t>(sample_parts_, 0, num_parts_);
  std::vector<PartTables> sampled;
  {
    auto scheduler = run(0, sample, nullptr);
    while (!scheduler->done()) {
      PartTables part;
      {
        const ProfileTimer wait(profile(), "wait");
        ARROW_ASSIGN_OR_RAISE(part, scheduler->Next());
      }
      sampled.push_back(std::move(part));
    }
  }
  heavy_.clear();
  if (keys_.empty()) {
    // The one outer group is heavy: every table spreads its groups by the hash of its column.
    ARROW_ASSIGN_OR_RAISE(heavy_, KeyHashes(arrow::compute::ExecBatch({}, 1)));
  } else {
    HeavyHitters summary(kHeavyCounters);
    for (const PartTables& part : sampled) {
      for (const std::uint64_t hash : part->key_hashes) {
        summary.Add(hash);
      }
      part->key_hashes = {};
    }
    heavy_ = summary.Above(kHeavyShare);
    if (profile() != nullptr) {
      profile()->Add("sample_parts", MetricUnit::kCount, sample);
      profile()->Add("heavy_keys", MetricUnit::kCount, static_cast<int64_t>(heavy_.size()));
    }
  }
  ARROW_RETURN_NOT_OK(ForEach(executor_, sampled.size(), [&](std::size_t i) -> arrow::Status {
    for (const auto& table : sampled[i]->tables) {
      ARROW_RETURN_NOT_OK(table->Partition(prefix, heavy_));
    }
    return arrow::Status::OK();
  }));
  // Every part merges in part order, each partition in its own lane: the sample's parts, then the
  // others, partitioned on their workers.
  PartitionLanes lanes(GroupTable::kPartitions, executor_, window_, budget_);
  int64_t index = 0;
  arrow::Status status;
  for (PartTables& part : sampled) {
    status = lanes.Add(index++, MergeOf(std::move(part)));  // the lanes free it once merged
    if (!status.ok()) {
      break;
    }
  }
  sampled.clear();
  if (status.ok()) {
    auto scheduler = run(sample, num_parts_ - sample,
                         std::make_shared<const std::vector<std::uint64_t>>(heavy_));
    // A part that runs out of memory runs again alone once the earlier parts are merged and freed.
    scheduler->set_before_retry([&lanes] { lanes.Wait(); });
    while (!scheduler->done()) {
      arrow::Result<PartTables> part = [&] {
        const ProfileTimer wait(profile(), "wait");
        return scheduler->Next();
      }();
      if (!part.ok()) {
        status = part.status();
        break;
      }
      status = lanes.Add(index++, MergeOf(*std::move(part)));
      if (!status.ok()) {
        break;
      }
    }
  }
  // A merge failure is of an earlier part than any failure of the scheduler seen after it.
  {
    const ProfileTimer tail(profile(), "lanes_tail");  // merging after the last part came
    ARROW_RETURN_NOT_OK(lanes.Finish());
  }
  ARROW_RETURN_NOT_OK(status);

  // The outer level, partitions in parallel; then the heavy K's groups across the partitions.
  outer_.clear();
  outer_.resize(GroupTable::kPartitions);
  heavy_groups_.assign(GroupTable::kPartitions, {});
  rows_.assign(GroupTable::kPartitions, {});
  {
    const ProfileTimer outer(profile(), "outer");
    ARROW_RETURN_NOT_OK(
        ForEach(executor_, GroupTable::kPartitions, [&](std::size_t p) { return Outer(p); }));
  }
  tables_.clear();
  ARROW_ASSIGN_OR_RAISE(heavy_table_,
                        OuterGroups::Make(key_types_, distinct_.size(), plain_, pool_, budget_));
  int64_t light = 0;
  for (std::size_t p = 0; p < GroupTable::kPartitions; ++p) {
    light += static_cast<int64_t>(outer_[p]->num_groups()) -
             static_cast<int64_t>(heavy_groups_[p].size());
    ARROW_RETURN_NOT_OK(heavy_table_->Merge(*outer_[p], heavy_groups_[p]));
    outer_[p].reset();  // gives the memory back
  }
  outer_.clear();
  heavy_groups_.clear();
  if (global_) {
    ARROW_RETURN_NOT_OK(heavy_table_->EnsureGroup());
  }
  if (profile() != nullptr) {
    profile()->Add("groups", MetricUnit::kCount, light + heavy_table_->num_groups());
    profile()->Add("heavy_groups", MetricUnit::kCount, heavy_table_->num_groups());
  }
  return arrow::Status::OK();
}

arrow::Result<Batch> PartTwoLevelAggregateOperator::Next() {
  if (!opened_) {
    return arrow::Status::Invalid("two-level aggregate: Next() before Open() or after Close()");
  }
  if (!aggregated_) {
    ARROW_RETURN_NOT_OK(Aggregate());
    aggregated_ = true;
  }
  for (; next_partition_ < rows_.size(); ++next_partition_, next_rows_ = 0) {
    std::vector<std::shared_ptr<arrow::RecordBatch>>& rows = rows_[next_partition_];
    if (next_rows_ < rows.size()) {
      return Batch{.data = std::move(rows[next_rows_++]), .selection = {}};
    }
    rows = {};
  }
  const std::uint32_t groups = heavy_table_ == nullptr ? 0 : heavy_table_->num_groups();
  if (next_group_ < groups) {
    const std::uint32_t begin = next_group_;
    const auto end = static_cast<std::uint32_t>(
        std::min<std::size_t>(groups, begin + GroupTable::kMaxMergeChunk));
    next_group_ = end;
    std::vector<std::uint32_t> chunk(end - begin);
    std::ranges::iota(chunk, begin);
    ARROW_ASSIGN_OR_RAISE(auto rows,
                          heavy_table_->Rows(begin, end, chunk, slots_, schema_, !global_));
    return Batch{.data = std::move(rows), .selection = {}};
  }
  heavy_table_.reset();  // gives the memory back
  return Batch{};
}

arrow::Status PartTwoLevelAggregateOperator::Close() {
  tables_.clear();
  outer_.clear();
  heavy_groups_.clear();
  rows_.clear();
  heavy_table_.reset();
  heavy_.clear();
  next_partition_ = 0;
  next_rows_ = 0;
  next_group_ = 0;
  aggregated_ = false;
  opened_ = false;
  return arrow::Status::OK();
}

// ---- PartTopNOperator ----

PartTopNOperator::PartTopNOperator(PartPipeline pipeline, int64_t num_parts,
                                   std::shared_ptr<arrow::Schema> schema,
                                   std::vector<plan::SortKey> keys, int64_t limit, int64_t offset,
                                   std::optional<LateColumns> late)
    : pipeline_(std::make_shared<const PartPipeline>(std::move(pipeline))),
      num_parts_(num_parts),
      schema_(std::move(schema)),
      keys_(std::move(keys)),
      limit_(limit),
      offset_(offset),
      late_(std::move(late)) {}

PartTopNOperator::~PartTopNOperator() = default;

arrow::Status PartTopNOperator::Open(ExecContext& ctx) {
  ARROW_RETURN_NOT_OK(Close());
  if (keys_.empty()) {
    return arrow::Status::Invalid("sort without keys");
  }
  if (limit_ < 1 || offset_ < 0) {
    return arrow::Status::Invalid("a top-N needs a positive LIMIT and no negative OFFSET");
  }
  pool_ = ctx.pool;
  executor_ = ctx.executor;
  batch_size_ = std::max<int64_t>(ctx.batch_size, 1);
  int64_t keep = 0;
  if (__builtin_add_overflow(limit_, offset_, &keep)) {
    keep = std::numeric_limits<int64_t>::max();
  }
  // The rows the parts keep: the narrow pipeline's with late columns.
  const std::shared_ptr<arrow::Schema>& rows_schema =
      late_.has_value() ? late_->narrow_schema : schema_;
  if (late_.has_value()) {
    const LateColumns& late = late_.value();
    if (late.table == nullptr || late.parts == nullptr || late.slots.empty() ||
        late.slots.size() != late.fields.size() || rows_schema == nullptr ||
        rows_schema->num_fields() != schema_->num_fields() || late.row_id < 0 ||
        late.row_id >= rows_schema->num_fields() ||
        rows_schema->field(late.row_id)->type()->id() != arrow::Type::INT64) {
      return arrow::Status::Invalid("a late top-N without its late columns");
    }
  }
  ARROW_ASSIGN_OR_RAISE(RowComparator comparator, RowComparator::Make(rows_schema, keys_));
  merged_ = std::make_unique<SortBuffer>(std::move(comparator), keep);
  memory_.Reset(ctx.budget);
  sorted_ = false;
  next_ = 0;
  end_ = 0;
  window_.reset();
  auto task = [pipeline = pipeline_, part_ctx = PartContext(ctx), schema = rows_schema,
               keys = keys_, keep, profile = profile()](
                  int64_t part, const std::atomic<bool>& stop) -> arrow::Result<PartBuffer> {
    const ProfileTimer part_time(profile, "part_time");
    ARROW_ASSIGN_OR_RAISE(RowComparator part_comparator, RowComparator::Make(schema, keys));
    auto rows =
        std::make_shared<PartRows>(SortBuffer(std::move(part_comparator), keep), part_ctx.budget);
    SortBuffer& buffer = rows->buffer;
    ARROW_RETURN_NOT_OK(
        RunPart(*pipeline, part, part_ctx, stop, [&](const Batch& batch) -> arrow::Result<bool> {
          ARROW_ASSIGN_OR_RAISE(auto data, Materialize(batch, part_ctx.pool));
          ARROW_RETURN_NOT_OK(buffer.Add(std::move(data), part_ctx.pool));
          ARROW_RETURN_NOT_OK(rows->memory.Resize(buffer.memory_usage()));
          return true;
        }));
    ARROW_RETURN_NOT_OK(rows->memory.Resize(buffer.memory_usage() + buffer.sort_memory()));
    ARROW_RETURN_NOT_OK(buffer.Sort(part_ctx.pool));  // keeps the part's first `keep` rows
    ARROW_RETURN_NOT_OK(rows->memory.Resize(buffer.memory_usage()));
    return rows;
  };
  scheduler_ = std::make_unique<PartScheduler<PartBuffer>>(num_parts_, std::move(task),
                                                           ctx.executor, Window(ctx), ctx.budget);
  return arrow::Status::OK();
}

arrow::Result<Batch> PartTopNOperator::Next() {
  if (merged_ == nullptr) {
    return arrow::Status::Invalid("top-N: Next() before Open() or after Close()");
  }
  if (!sorted_) {
    while (!scheduler_->done()) {
      PartBuffer part;
      {
        const ProfileTimer wait(profile(), "wait");
        ARROW_ASSIGN_OR_RAISE(part, scheduler_->Next());
      }
      const ProfileTimer merge(profile(), "merge");
      ARROW_RETURN_NOT_OK(merged_->Merge(part->buffer, pool_));  // after the earlier parts' rows
      ARROW_RETURN_NOT_OK(memory_.Resize(merged_->memory_usage()));
    }
    scheduler_.reset();
    ARROW_RETURN_NOT_OK(memory_.Resize(merged_->memory_usage() + merged_->sort_memory()));
    ARROW_RETURN_NOT_OK(merged_->Sort(pool_));
    ARROW_RETURN_NOT_OK(memory_.Resize(merged_->memory_usage()));
    sorted_ = true;
    next_ = std::min(offset_, merged_->num_rows());
    int64_t end = 0;
    end_ = __builtin_add_overflow(next_, limit_, &end) ? merged_->num_rows()
                                                       : std::min(merged_->num_rows(), end);
  }
  if (next_ >= end_) {
    return Batch{};
  }
  const int64_t stop = end_ - next_ > batch_size_ ? next_ + batch_size_ : end_;
  if (late_.has_value()) {
    if (window_ == nullptr) {  // the whole window at once: its late columns in one fetch
      const int64_t begin = next_;
      ARROW_ASSIGN_OR_RAISE(auto narrow, merged_->Slice(begin, end_, pool_));
      ARROW_ASSIGN_OR_RAISE(window_, Fetch(*narrow, late_.value()));
      window_start_ = begin;
    }
    auto rows = window_->Slice(next_ - window_start_, stop - next_);
    next_ = stop;
    return Batch{.data = std::move(rows), .selection = {}};
  }
  ARROW_ASSIGN_OR_RAISE(auto rows, merged_->Slice(next_, stop, pool_));
  next_ = stop;
  return Batch{.data = std::move(rows), .selection = {}};
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> PartTopNOperator::Fetch(
    const arrow::RecordBatch& window, const LateColumns& late) {
  const ProfileTimer fetch(profile(), "late_fetch");
  const int64_t rows = window.num_rows();
  const auto& ids = static_cast<const arrow::Int64Array&>(*window.column(late.row_id));
  // The window's rows by part, the parts in order of their first row.
  std::vector<int64_t> ordinals;
  std::vector<std::vector<int64_t>> offsets;  // per part, the rows' positions in the part
  std::vector<std::vector<int64_t>> members;  // per part, the rows' positions in the window
  std::unordered_map<int64_t, std::size_t> group_of;
  for (int64_t r = 0; r < rows; ++r) {
    const int64_t id = ids.Value(r);
    const int64_t ordinal = RowIdOrdinal(id);
    if (ordinal < 0 || std::cmp_greater_equal(ordinal, late.parts->size())) {
      return arrow::Status::Invalid("a row id of part ", ordinal, " of ", late.parts->size());
    }
    const auto [it, added] = group_of.try_emplace(ordinal, ordinals.size());
    if (added) {
      ordinals.push_back(ordinal);
      offsets.emplace_back();
      members.emplace_back();
    }
    offsets[it->second].push_back(RowIdOffset(id));
    members[it->second].push_back(r);
  }
  // Where each window row lands when the parts' rows are put one part after another.
  std::vector<int64_t> order(static_cast<std::size_t>(rows));
  int64_t position = 0;
  for (const std::vector<int64_t>& part : members) {
    for (const int64_t r : part) {
      order[static_cast<std::size_t>(r)] = position++;
    }
  }
  if (profile() != nullptr) {
    profile()->Add("late_columns", MetricUnit::kCount, static_cast<int64_t>(late.slots.size()));
    profile()->Add("late_parts", MetricUnit::kCount, static_cast<int64_t>(ordinals.size()));
  }
  // One task per part and late column: the column of that part, then its rows.
  const std::size_t columns = late.slots.size();
  std::vector<std::shared_ptr<arrow::Array>> pieces(ordinals.size() * columns);
  arrow::compute::ExecContext kernels(pool_);
  ARROW_RETURN_NOT_OK(ForEach(executor_, pieces.size(), [&](std::size_t task) -> arrow::Status {
    const std::size_t part = task / columns;
    const std::size_t column = task % columns;
    ARROW_ASSIGN_OR_RAISE(
        auto reader, late.table->ScanPart((*late.parts)[static_cast<std::size_t>(ordinals[part])],
                                          {late.fields[column]}, batch_size_, pool_));
    arrow::ArrayVector read;
    while (true) {
      std::shared_ptr<arrow::RecordBatch> batch;
      ARROW_RETURN_NOT_OK(reader->ReadNext(&batch));
      if (batch == nullptr) {
        break;
      }
      read.push_back(batch->column(0));
    }
    ARROW_RETURN_NOT_OK(reader->Close());
    if (read.empty()) {
      return arrow::Status::IOError("part ", ordinals[part], " has no rows to fetch");
    }
    ARROW_ASSIGN_OR_RAISE(
        auto whole, read.size() == 1 ? arrow::Result<std::shared_ptr<arrow::Array>>(read.front())
                                     : arrow::Concatenate(read, pool_));
    arrow::Int64Builder indices(pool_);
    ARROW_RETURN_NOT_OK(indices.AppendValues(offsets[part]));
    ARROW_ASSIGN_OR_RAISE(auto positions, indices.Finish());
    arrow::compute::ExecContext task_kernels(pool_);
    ARROW_ASSIGN_OR_RAISE(
        const arrow::Datum taken,
        arrow::compute::Take(whole, positions, arrow::compute::TakeOptions::BoundsCheck(),
                             &task_kernels));
    pieces[task] = taken.make_array();
    return arrow::Status::OK();
  }));
  arrow::ArrayVector out = window.columns();
  arrow::Int64Builder order_builder(pool_);
  ARROW_RETURN_NOT_OK(order_builder.AppendValues(order));
  ARROW_ASSIGN_OR_RAISE(auto window_order, order_builder.Finish());
  for (std::size_t column = 0; column < columns; ++column) {
    arrow::ArrayVector parts;
    parts.reserve(ordinals.size());
    for (std::size_t part = 0; part < ordinals.size(); ++part) {
      parts.push_back(pieces[(part * columns) + column]);
    }
    std::shared_ptr<arrow::Array> joined;
    if (parts.empty()) {
      ARROW_ASSIGN_OR_RAISE(
          joined, arrow::MakeEmptyArray(schema_->field(late.slots[column])->type(), pool_));
    } else if (parts.size() == 1) {
      joined = parts.front();
    } else {
      ARROW_ASSIGN_OR_RAISE(joined, arrow::Concatenate(parts, pool_));
    }
    ARROW_ASSIGN_OR_RAISE(
        const arrow::Datum placed,
        arrow::compute::Take(joined, window_order, arrow::compute::TakeOptions::NoBoundsCheck(),
                             &kernels));
    out[static_cast<std::size_t>(late.slots[column])] = placed.make_array();
  }
  return arrow::RecordBatch::Make(schema_, rows, std::move(out));
}

arrow::Status PartTopNOperator::Close() {
  scheduler_.reset();
  merged_.reset();
  window_.reset();
  memory_.Release();
  return arrow::Status::OK();
}

}  // namespace antb1::exec
