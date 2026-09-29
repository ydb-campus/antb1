#include "part_operators.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <arrow/api.h>

#include "antb1/exec/operator.h"
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
  merged_ = false;
  ARROW_ASSIGN_OR_RAISE(table_, GroupTable::Make(keys_, aggregates_, input_width_, pool_, budget_));
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
    return table;
  };
  scheduler_ = std::make_unique<PartScheduler<PartTable>>(num_parts_, std::move(task), ctx.executor,
                                                          Window(ctx), ctx.budget);
  return arrow::Status::OK();
}

arrow::Result<Batch> PartGroupAggregateOperator::Next() {
  if (table_ == nullptr) {
    if (merged_) {
      return Batch{};
    }
    return arrow::Status::Invalid("part group aggregate: Next() before Open()");
  }
  if (!merged_) {
    while (!scheduler_->done()) {
      ARROW_ASSIGN_OR_RAISE(const PartTable part, scheduler_->Next());
      ARROW_RETURN_NOT_OK(table_->Merge(*part));
    }
    merged_ = true;
    scheduler_.reset();
  }
  ARROW_ASSIGN_OR_RAISE(auto chunk, table_->NextChunk(schema_));
  if (chunk == nullptr) {
    table_.reset();  // gives the memory back
    return Batch{};
  }
  return Batch{.data = std::move(chunk), .selection = {}};
}

arrow::Status PartGroupAggregateOperator::Close() {
  scheduler_.reset();
  table_.reset();
  return arrow::Status::OK();
}

}  // namespace antb1::exec
