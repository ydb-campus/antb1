#include "antb1/exec/table_scan.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <arrow/api.h>

#include "antb1/plan/table.h"

namespace antb1::exec {
namespace {

std::shared_ptr<arrow::Schema> FieldsOf(const plan::Table& table, const std::vector<int>& fields,
                                        const std::optional<LateScan>& late) {
  const arrow::Schema& schema = *table.schema();
  arrow::FieldVector out;
  out.reserve(fields.size());
  for (std::size_t i = 0; i < fields.size(); ++i) {
    const int f = fields[i];
    // An index outside the schema makes Open fail (plan::Table::Scan validates the request).
    std::shared_ptr<arrow::Field> field =
        f >= 0 && f < schema.num_fields() ? schema.field(f) : arrow::field("?", arrow::null());
    if (late.has_value() && i < late->late.size() && late->late[i]) {
      field = std::cmp_equal(late->row_id, i) ? arrow::field("row_id", arrow::int64(), false)
                                              : arrow::field(field->name(), arrow::null());
    }
    out.push_back(std::move(field));
  }
  return arrow::schema(std::move(out));
}

}  // namespace

TableScanOperator::TableScanOperator(std::shared_ptr<plan::Table> table, std::vector<int> fields,
                                     std::optional<int64_t> part, std::optional<LateScan> late)
    : table_(std::move(table)),
      fields_(std::move(fields)),
      part_(part),
      late_(std::move(late)),
      schema_(FieldsOf(*table_, fields_, late_)) {
  for (std::size_t i = 0; i < fields_.size(); ++i) {
    if (!late_.has_value() || i >= late_->late.size() || !late_->late[i]) {
      read_.push_back(fields_[i]);
    }
  }
}

arrow::Status TableScanOperator::Open(ExecContext& ctx) {
  offset_ = 0;
  pool_ = ctx.pool;
  if (late_.has_value()) {
    const LateScan& late = late_.value();
    if (!part_.has_value() || late.late.size() != fields_.size() || late.row_id < 0 ||
        std::cmp_greater_equal(late.row_id, fields_.size()) ||
        !late.late[static_cast<std::size_t>(late.row_id)] || late.ordinal < 0 ||
        late.ordinal >= kRowIdParts) {
      return arrow::Status::Invalid("a narrow scan of a part needs its late columns and row id");
    }
    ARROW_ASSIGN_OR_RAISE(reader_, table_->ScanPart(*part_, read_, ctx.batch_size, ctx.pool));
  } else if (part_.has_value()) {
    ARROW_ASSIGN_OR_RAISE(reader_, table_->ScanPart(*part_, fields_, ctx.batch_size, ctx.pool));
  } else {
    ARROW_ASSIGN_OR_RAISE(reader_, table_->Scan(fields_, ctx.batch_size, ctx.pool));
  }
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
  if (late_.has_value()) {
    return Narrow(*batch, late_.value());
  }
  if (!batch->schema()->Equals(*schema_, /*check_metadata=*/false)) {
    // Same types under other names (e.g. an in-memory table); a type mismatch is an error.
    ARROW_ASSIGN_OR_RAISE(batch, batch->ReplaceSchema(schema_));
  }
  return Batch{.data = std::move(batch), .selection = {}};
}

arrow::Result<Batch> TableScanOperator::Narrow(const arrow::RecordBatch& read,
                                               const LateScan& late) {
  const int64_t rows = read.num_rows();
  if (offset_ + rows > kRowIdParts * 2) {
    return arrow::Status::Invalid("a part of more than 2^32 rows has no row ids");
  }
  arrow::ArrayVector columns;
  columns.reserve(fields_.size());
  int next = 0;  // the next column read
  for (std::size_t i = 0; i < fields_.size(); ++i) {
    if (!late.late[i]) {
      columns.push_back(read.column(next++));
    } else if (std::cmp_equal(late.row_id, i)) {
      arrow::Int64Builder ids(pool_);
      ARROW_RETURN_NOT_OK(ids.Reserve(rows));
      for (int64_t r = 0; r < rows; ++r) {
        ids.UnsafeAppend(RowId(late.ordinal, offset_ + r));
      }
      ARROW_ASSIGN_OR_RAISE(auto array, ids.Finish());
      columns.push_back(std::move(array));
    } else {
      columns.push_back(std::make_shared<arrow::NullArray>(rows));
    }
  }
  offset_ += rows;
  return Batch{.data = arrow::RecordBatch::Make(schema_, rows, std::move(columns)),
               .selection = {}};
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
