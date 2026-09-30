#pragma once

#include <cstddef>
#include <deque>
#include <memory>
#include <vector>

#include <arrow/memory_pool.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/type_fwd.h>
#include <arrow/util/future.h>
#include <arrow/util/thread_pool.h>

#include "antb1/exec/memory_budget.h"
#include "antb1/exec/operator.h"
#include "antb1/plan/logical_plan.h"

namespace antb1::exec {

// The output schema of a Compute: the input fields, then one field per expression.
std::shared_ptr<arrow::Schema> ComputedSchema(const arrow::Schema& input,
                                              const std::vector<plan::ExprPtr>& exprs);

// The selected rows of `in` with one column per expression appended (ComputeOperator's batch).
arrow::Result<Batch> ComputeBatch(const Batch& in, const std::vector<plan::ExprPtr>& exprs,
                                  const std::shared_ptr<arrow::Schema>& schema,
                                  arrow::MemoryPool* pool);

// ComputeOperator for a Compute outside a part pipeline (over an aggregation or a sort, whose
// batches arrive on the consumer's thread): with an executor, it reads its input ahead and
// computes up to `threads` batches at a time on the executor (one under memory pressure). It
// returns them strictly in input order, so its output, and its first error in batch order, are
// those of ComputeOperator. A batch read ahead but never returned (a Limit above stopped) never
// reports its error, as ComputeOperator never computes it. Without an executor it is
// ComputeOperator.
class ParallelComputeOperator final : public Operator {
 public:
  ParallelComputeOperator(std::unique_ptr<Operator> input, std::vector<plan::ExprPtr> exprs);
  ~ParallelComputeOperator() override;
  ParallelComputeOperator(const ParallelComputeOperator&) = delete;
  ParallelComputeOperator& operator=(const ParallelComputeOperator&) = delete;
  ParallelComputeOperator(ParallelComputeOperator&&) = delete;
  ParallelComputeOperator& operator=(ParallelComputeOperator&&) = delete;

  [[nodiscard]] const std::shared_ptr<arrow::Schema>& output_schema() const override {
    return shared_->schema;
  }
  arrow::Status Open(ExecContext& ctx) override;
  arrow::Result<Batch> Next() override;
  arrow::Status Close() override;

 private:
  // What a task reads: shared, so that it outlives the operator if it has to.
  struct Shared {
    std::vector<plan::ExprPtr> exprs;
    std::shared_ptr<arrow::Schema> schema;
  };

  // Waits for the batches in flight and drops them.
  void Drop();

  std::unique_ptr<Operator> input_;
  std::shared_ptr<const Shared> shared_;
  arrow::MemoryPool* pool_ = arrow::default_memory_pool();
  arrow::internal::Executor* executor_ = nullptr;
  const MemoryBudget* budget_ = nullptr;
  std::size_t window_ = 1;
  bool input_done_ = false;
  arrow::Status input_error_;  // the input's failure, returned after the batches read before it
  std::deque<arrow::Future<Batch>> in_flight_;  // in input order
};

}  // namespace antb1::exec
