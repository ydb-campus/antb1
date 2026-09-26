#pragma once

#include <memory>
#include <vector>

#include <arrow/scalar.h>

#include "antb1/exec/operator.h"
#include "antb1/plan/logical_plan.h"

namespace antb1::exec {

// Keeps the rows for which every predicate is true, without copying data: each input batch gets a
// selection (the AND of the comparisons, computed with Arrow's comparison kernels and and_kleene;
// a NULL comparison rejects the row). Batches without a selected row are skipped, and a batch whose
// rows all pass keeps no selection. A folded FALSE predicate ends the stream without reading input.
// Output: the input columns.
class FilterOperator final : public Operator {
 public:
  FilterOperator(std::unique_ptr<Operator> input, std::vector<plan::Predicate> predicates);

  [[nodiscard]] const std::shared_ptr<arrow::Schema>& output_schema() const override {
    return input_->output_schema();
  }
  arrow::Status Open(ExecContext& ctx) override;
  arrow::Result<Batch> Next() override;
  arrow::Status Close() override { return input_->Close(); }

 private:
  // The selection of one batch (with NULLs, before normalization).
  arrow::Result<std::shared_ptr<arrow::Array>> Evaluate(const arrow::RecordBatch& batch) const;

  std::unique_ptr<Operator> input_;
  std::vector<plan::Predicate> predicates_;
  // Per predicate, from Open: the input column (-1 for kFalse) and the constant (kCompare only).
  std::vector<int> columns_;
  std::vector<std::shared_ptr<arrow::Scalar>> constants_;
  bool never_true_ = false;
  arrow::MemoryPool* pool_ = arrow::default_memory_pool();
};

}  // namespace antb1::exec
