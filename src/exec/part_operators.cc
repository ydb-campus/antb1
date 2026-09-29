#include "part_operators.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/util/future.h>
#include <arrow/util/thread_pool.h>

#include "antb1/exec/operator.h"
#include "antb1/exec/sort.h"
#include "antb1/plan/logical_plan.h"

#include "aggregate_set.h"
#include "part_scheduler.h"

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
  auto task = [pipeline = pipeline_, part_ctx = PartContext(ctx), cap = row_cap_](
                  int64_t part, const std::atomic<bool>& stop) -> arrow::Result<PartBatches> {
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
    ARROW_ASSIGN_OR_RAISE(current_, scheduler_->Next());
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
               width = input_width_](int64_t part,
                                     const std::atomic<bool>& stop) -> arrow::Result<PartStates> {
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
    ARROW_ASSIGN_OR_RAISE(const PartStates part, scheduler_->Next());
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

// ---- PartGroupAggregateOperator ----

PartGroupAggregateOperator::PartGroupAggregateOperator(PartPipeline pipeline, int64_t num_parts,
                                                       int input_width,
                                                       std::vector<plan::BoundColumn> keys,
                                                       std::vector<plan::AggregateCall> aggregates,
                                                       std::shared_ptr<arrow::Schema> schema)
    : pipeline_(std::make_shared<const PartPipeline>(std::move(pipeline))),
      num_parts_(num_parts),
      input_width_(input_width),
      keys_(std::move(keys)),
      aggregates_(std::move(aggregates)),
      schema_(std::move(schema)) {}

PartGroupAggregateOperator::~PartGroupAggregateOperator() = default;

arrow::Status PartGroupAggregateOperator::Open(ExecContext& ctx) {
  ARROW_RETURN_NOT_OK(Close());
  pool_ = ctx.pool;
  budget_ = ctx.budget;
  executor_ = ctx.executor;
  merged_ = false;
  opened_ = true;
  auto task = [pipeline = pipeline_, part_ctx = PartContext(ctx), keys = keys_,
               aggregates = aggregates_, width = input_width_](
                  int64_t part, const std::atomic<bool>& stop) -> arrow::Result<PartTable> {
    ARROW_ASSIGN_OR_RAISE(
        std::shared_ptr<GroupTable> table,
        GroupTable::Make(keys, aggregates, width, part_ctx.pool, part_ctx.budget));
    ARROW_RETURN_NOT_OK(
        RunPart(*pipeline, part, part_ctx, stop, [&](const Batch& batch) -> arrow::Result<bool> {
          ARROW_ASSIGN_OR_RAISE(const auto rows, Materialize(batch, part_ctx.pool));
          ARROW_RETURN_NOT_OK(table->Consume(*rows));
          return true;
        }));
    ARROW_RETURN_NOT_OK(table->Partition());  // on the worker, not on the merging thread
    return table;
  };
  scheduler_ = std::make_unique<PartScheduler<PartTable>>(num_parts_, std::move(task), ctx.executor,
                                                          Window(ctx), ctx.budget);
  return arrow::Status::OK();
}

arrow::Status PartGroupAggregateOperator::MergePart(const GroupTable& part) {
  const std::size_t partitions = part.num_partitions();
  if (tables_.empty()) {
    tables_.reserve(partitions);
    for (std::size_t p = 0; p < partitions; ++p) {
      ARROW_ASSIGN_OR_RAISE(auto table,
                            GroupTable::Make(keys_, aggregates_, input_width_, pool_, budget_));
      tables_.push_back(std::move(table));
    }
  }
  if (tables_.size() != partitions) {
    return arrow::Status::Invalid("a part of ", partitions, " partitions for ", tables_.size());
  }
  if (executor_ == nullptr || partitions == 1) {
    for (std::size_t p = 0; p < partitions; ++p) {
      ARROW_RETURN_NOT_OK(tables_[p]->MergePartition(part, p));
    }
    return arrow::Status::OK();
  }
  // Partitions are disjoint: each merges on the executor, and all finish before the next part.
  std::vector<arrow::Future<>> merges;
  merges.reserve(partitions);
  arrow::Status submitted;
  for (std::size_t p = 0; p < partitions && submitted.ok(); ++p) {
    GroupTable* table = tables_[p].get();
    auto merge = executor_->Submit([table, &part, p] -> arrow::Status {
      try {
        return table->MergePartition(part, p);
      } catch (const std::bad_alloc&) {
        return arrow::Status::OutOfMemory("out of memory while merging groups");
      }
    });
    if (merge.ok()) {
      merges.push_back(std::move(*merge));
    } else {
      submitted = merge.status();
    }
  }
  arrow::Status status = submitted;
  for (const arrow::Future<>& merge : merges) {  // every merge ends before `part` can go
    const arrow::Status merged = merge.status();
    if (status.ok()) {
      status = merged;  // the first failed partition, in partition order
    }
  }
  return status;
}

arrow::Result<Batch> PartGroupAggregateOperator::Next() {
  if (!opened_) {
    return arrow::Status::Invalid("part group aggregate: Next() before Open() or after Close()");
  }
  if (!merged_) {
    while (!scheduler_->done()) {
      ARROW_ASSIGN_OR_RAISE(const PartTable part, scheduler_->Next());
      ARROW_RETURN_NOT_OK(MergePart(*part));
    }
    merged_ = true;
    scheduler_.reset();
  }
  while (next_table_ < tables_.size()) {
    ARROW_ASSIGN_OR_RAISE(auto chunk, tables_[next_table_]->NextChunk(schema_));
    if (chunk != nullptr) {
      return Batch{.data = std::move(chunk), .selection = {}};
    }
    tables_[next_table_].reset();  // gives the memory back
    ++next_table_;
  }
  return Batch{};
}

arrow::Status PartGroupAggregateOperator::Close() {
  scheduler_.reset();
  tables_.clear();
  next_table_ = 0;
  opened_ = false;
  return arrow::Status::OK();
}

// ---- PartTopNOperator ----

PartTopNOperator::PartTopNOperator(PartPipeline pipeline, int64_t num_parts,
                                   std::shared_ptr<arrow::Schema> schema,
                                   std::vector<plan::SortKey> keys, int64_t limit, int64_t offset)
    : pipeline_(std::make_shared<const PartPipeline>(std::move(pipeline))),
      num_parts_(num_parts),
      schema_(std::move(schema)),
      keys_(std::move(keys)),
      limit_(limit),
      offset_(offset) {}

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
  batch_size_ = std::max<int64_t>(ctx.batch_size, 1);
  int64_t keep = 0;
  if (__builtin_add_overflow(limit_, offset_, &keep)) {
    keep = std::numeric_limits<int64_t>::max();
  }
  ARROW_ASSIGN_OR_RAISE(RowComparator comparator, RowComparator::Make(schema_, keys_));
  merged_ = std::make_unique<SortBuffer>(std::move(comparator), keep);
  memory_.Reset(ctx.budget);
  sorted_ = false;
  next_ = 0;
  end_ = 0;
  auto task = [pipeline = pipeline_, part_ctx = PartContext(ctx), schema = schema_, keys = keys_,
               keep](int64_t part, const std::atomic<bool>& stop) -> arrow::Result<PartBuffer> {
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
      ARROW_ASSIGN_OR_RAISE(const PartBuffer part, scheduler_->Next());
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
  ARROW_ASSIGN_OR_RAISE(auto rows, merged_->Slice(next_, stop, pool_));
  next_ = stop;
  return Batch{.data = std::move(rows), .selection = {}};
}

arrow::Status PartTopNOperator::Close() {
  scheduler_.reset();
  merged_.reset();
  memory_.Release();
  return arrow::Status::OK();
}

}  // namespace antb1::exec
