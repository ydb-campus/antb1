#pragma once

#include <memory>
#include <vector>

#include "antb1/exec/operator.h"

namespace antb1::exec {

// Selects input columns (by index, in the given order; a column may repeat) and materializes the
// selected rows with Arrow's Filter kernel: the only place where a selection is turned into data
// for a projection. Output: the listed input fields, batches without a selection.
class ProjectOperator final : public Operator {
 public:
  ProjectOperator(std::unique_ptr<Operator> input, std::vector<int> columns);

  [[nodiscard]] const std::shared_ptr<arrow::Schema>& output_schema() const override {
    return schema_;
  }
  arrow::Status Open(ExecContext& ctx) override;
  arrow::Result<Batch> Next() override;
  arrow::Status Close() override { return input_->Close(); }

 private:
  std::unique_ptr<Operator> input_;
  std::vector<int> columns_;
  std::shared_ptr<arrow::Schema> schema_;
  arrow::MemoryPool* pool_ = arrow::default_memory_pool();
};

}  // namespace antb1::exec
