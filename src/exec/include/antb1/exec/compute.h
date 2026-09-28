#pragma once

#include <memory>
#include <vector>

#include <arrow/array.h>
#include <arrow/memory_pool.h>
#include <arrow/record_batch.h>
#include <arrow/result.h>

#include "antb1/exec/operator.h"
#include "antb1/plan/logical_plan.h"

namespace antb1::exec {

// The values of `expr` in every row of `batch` (a batch without a selection), with DuckDB's
// semantics (docs/sql-subset.md): integer +, - and * in the expression's type, an overflow being
// an execution error; / in DOUBLE (x / 0 is +-inf, 0 / 0 NaN); // truncates and % takes the sign of
// the dividend, both NULL for a zero divisor (on DOUBLE: // divides, % is fmod); NULL in, NULL out.
arrow::Result<std::shared_ptr<arrow::Array>> EvaluateExpr(const plan::Expr& expr,
                                                          const arrow::RecordBatch& batch,
                                                          arrow::MemoryPool* pool);

// Appends one column per expression to the selected rows of its input. Output: the input fields,
// then the expressions' types; batches without a selection.
class ComputeOperator final : public Operator {
 public:
  ComputeOperator(std::unique_ptr<Operator> input, std::vector<plan::ExprPtr> exprs);

  [[nodiscard]] const std::shared_ptr<arrow::Schema>& output_schema() const override {
    return schema_;
  }
  arrow::Status Open(ExecContext& ctx) override;
  arrow::Result<Batch> Next() override;
  arrow::Status Close() override { return input_->Close(); }

 private:
  std::unique_ptr<Operator> input_;
  std::vector<plan::ExprPtr> exprs_;
  std::shared_ptr<arrow::Schema> schema_;
  arrow::MemoryPool* pool_ = arrow::default_memory_pool();
};

}  // namespace antb1::exec
