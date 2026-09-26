#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "antb1/exec/operator.h"

namespace antb1::exec {

// Emits a single BIGINT row with a precomputed row count (COUNT(*) from metadata).
class RowCountOperator final : public Operator {
 public:
  RowCountOperator(std::string column_name, int64_t row_count);

  [[nodiscard]] const std::shared_ptr<arrow::Schema>& output_schema() const override {
    return schema_;
  }
  arrow::Status Open(ExecContext& ctx) override;
  arrow::Result<Batch> Next() override;
  arrow::Status Close() override { return arrow::Status::OK(); }

 private:
  std::shared_ptr<arrow::Schema> schema_;
  int64_t row_count_;
  arrow::MemoryPool* pool_ = nullptr;
  bool done_ = false;
};

}  // namespace antb1::exec
