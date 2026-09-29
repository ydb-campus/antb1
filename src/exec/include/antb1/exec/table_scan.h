#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <arrow/record_batch.h>

#include "antb1/exec/operator.h"
#include "antb1/plan/table.h"

namespace antb1::exec {

// Reads the given top-level fields of a table (plan::Table::Scan), or of one of its parts
// (plan::Table::ScanPart), in batches of at most ExecContext::batch_size rows. Only the referenced
// fields are decoded; with no fields the batches carry row counts only. Output: the fields, in the
// given order.
class TableScanOperator final : public Operator {
 public:
  TableScanOperator(std::shared_ptr<plan::Table> table, std::vector<int> fields,
                    std::optional<int64_t> part = std::nullopt);

  [[nodiscard]] const std::shared_ptr<arrow::Schema>& output_schema() const override {
    return schema_;
  }
  arrow::Status Open(ExecContext& ctx) override;
  arrow::Result<Batch> Next() override;
  arrow::Status Close() override;

 private:
  std::shared_ptr<plan::Table> table_;
  std::vector<int> fields_;
  std::optional<int64_t> part_;
  std::shared_ptr<arrow::Schema> schema_;
  std::unique_ptr<arrow::RecordBatchReader> reader_;
};

}  // namespace antb1::exec
