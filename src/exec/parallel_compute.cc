#include "parallel_compute.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/util/future.h>

namespace antb1::exec {

ParallelComputeOperator::ParallelComputeOperator(std::unique_ptr<Operator> input,
                                                 std::vector<plan::ExprPtr> exprs)
    : input_(std::move(input)) {
  auto schema = ComputedSchema(*input_->output_schema(), exprs);
  shared_ = std::make_shared<const Shared>(
      Shared{.exprs = std::move(exprs), .schema = std::move(schema)});
}

ParallelComputeOperator::~ParallelComputeOperator() { Drop(); }

arrow::Status ParallelComputeOperator::Open(ExecContext& ctx) {
  Drop();
  pool_ = ctx.pool;
  executor_ = ctx.executor;
  budget_ = ctx.budget;
  window_ = static_cast<std::size_t>(std::max(ctx.threads, 1));
  input_done_ = false;
  input_error_ = arrow::Status::OK();
  return input_->Open(ctx);
}

arrow::Result<Batch> ParallelComputeOperator::Next() {
  if (executor_ == nullptr) {
    ARROW_ASSIGN_OR_RAISE(const Batch in, input_->Next());
    if (in.end()) {
      return in;
    }
    return ComputeBatch(in, shared_->exprs, shared_->schema, pool_);
  }
  // Keep the window full: read the input on this thread, compute on the executor.
  while (!input_done_ && in_flight_.size() < window_) {
    if (!in_flight_.empty() && budget_ != nullptr && budget_->under_pressure()) {
      break;
    }
    arrow::Result<Batch> in = input_->Next();
    if (!in.ok()) {
      input_done_ = true;
      input_error_ = in.status();
      break;
    }
    if (in->end()) {
      input_done_ = true;
      break;
    }
    ARROW_ASSIGN_OR_RAISE(
        arrow::Future<Batch> future,
        executor_->Submit([shared = shared_, batch = *std::move(in), pool = pool_]() {
          return ComputeBatch(batch, shared->exprs, shared->schema, pool);
        }));
    in_flight_.push_back(std::move(future));
  }
  if (in_flight_.empty()) {
    ARROW_RETURN_NOT_OK(input_error_);
    return Batch{};
  }
  const arrow::Future<Batch> next = std::move(in_flight_.front());
  in_flight_.pop_front();
  return next.result();
}

arrow::Status ParallelComputeOperator::Close() {
  Drop();
  return input_->Close();
}

void ParallelComputeOperator::Drop() {
  for (const arrow::Future<Batch>& future : in_flight_) {
    future.Wait();
  }
  in_flight_.clear();
}

}  // namespace antb1::exec
