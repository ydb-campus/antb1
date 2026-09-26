#pragma once

#include <cstdint>
#include <memory>

#include "antb1/exec/operator.h"

namespace antb1::exec {

// Passes on the first `limit` rows of its input (materializing selections) and then stops pulling:
// the input is never asked for another batch once the limit is reached (LIMIT 0 reads nothing).
// Output: the input columns.
class LimitOperator final : public Operator {
 public:
  LimitOperator(std::unique_ptr<Operator> input, int64_t limit);

  [[nodiscard]] const std::shared_ptr<arrow::Schema>& output_schema() const override {
    return input_->output_schema();
  }
  arrow::Status Open(ExecContext& ctx) override;
  arrow::Result<Batch> Next() override;
  arrow::Status Close() override { return input_->Close(); }

 private:
  std::unique_ptr<Operator> input_;
  int64_t limit_;
  int64_t emitted_ = 0;
  arrow::MemoryPool* pool_ = arrow::default_memory_pool();
};

}  // namespace antb1::exec
