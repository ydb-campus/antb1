#include "antb1/exec/table_scan.h"

#include <memory>
#include <utility>
#include <vector>

#include <arrow/api.h>

#include "antb1/plan/table.h"

namespace antb1::exec {
namespace {

std::shared_ptr<arrow::Schema> FieldsOf(const plan::Table& table, const std::vector<int>& fields) {
  const arrow::Schema& schema = *table.schema();
  arrow::FieldVector out;
  out.reserve(fields.size());
  for (const int f : fields) {
    // An index outside the schema makes Open fail (plan::Table::Scan validates the request).
    out.push_back(f >= 0 && f < schema.num_fields() ? schema.field(f)
                                                    : arrow::field("?", arrow::null()));
  }
  return arrow::schema(std::move(out));
}

}  // namespace

TableScanOperator::TableScanOperator(std::shared_ptr<plan::Table> table, std::vector<int> fields)
    : table_(std::move(table)), fields_(std::move(fields)), schema_(FieldsOf(*table_, fields_)) {}

arrow::Status TableScanOperator::Open(ExecContext& ctx) {
  ARROW_ASSIGN_OR_RAISE(reader_, table_->Scan(fields_, ctx.batch_size));
  return arrow::Status::OK();
}

arrow::Result<Batch> TableScanOperator::Next() {
  if (reader_ == nullptr) {
    return arrow::Status::Invalid("table scan: Next() before Open()");
  }
  std::shared_ptr<arrow::RecordBatch> batch;
  ARROW_RETURN_NOT_OK(reader_->ReadNext(&batch));
  if (batch == nullptr) {
    return Batch{};
  }
  if (!batch->schema()->Equals(*schema_, /*check_metadata=*/false)) {
    // Same types under other names (e.g. an in-memory table); a type mismatch is an error.
    ARROW_ASSIGN_OR_RAISE(batch, batch->ReplaceSchema(schema_));
  }
  return Batch{.data = std::move(batch), .selection = {}};
}

arrow::Status TableScanOperator::Close() {
  if (reader_ == nullptr) {
    return arrow::Status::OK();
  }
  const arrow::Status status = reader_->Close();
  reader_.reset();
  return status;
}

}  // namespace antb1::exec
