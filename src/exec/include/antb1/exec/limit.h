#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "antb1/exec/operator.h"

namespace antb1::exec {

// Skips the first `offset` rows of its input, then passes on at most `limit` rows (none: all) and
// stops pulling: the input is never asked for another batch once the limit is reached (LIMIT 0
// reads nothing). Selections are narrowed, not materialized. Output: the input columns.
class LimitOperator final : public Operator {
 public:
  LimitOperator(std::unique_ptr<Operator> input, std::optional<int64_t> limit, int64_t offset = 0);

  [[nodiscard]] const std::shared_ptr<arrow::Schema>& output_schema() const override {
    return input_->output_schema();
  }
  arrow::Status Open(ExecContext& ctx) override;
  arrow::Result<Batch> Next() override;
  arrow::Status Close() override { return input_->Close(); }

 private:
  std::unique_ptr<Operator> input_;
  std::optional<int64_t> limit_;
  int64_t offset_;
  int64_t skipped_ = 0;
  int64_t emitted_ = 0;
};

}  // namespace antb1::exec
