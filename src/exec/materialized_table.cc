#include "materialized_table.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <arrow/record_batch.h>
#include <arrow/status.h>
#include <arrow/type.h>
#include <arrow/util/byte_size.h>

#include "antb1/common/narrow.h"

namespace antb1::exec {
namespace {

// The bytes a batch holds, which is what the budget is charged for. Arrow's own accounting, so a
// buffer shared between columns is counted once and a slice is counted for what it references.
int64_t BytesOf(const arrow::RecordBatch& batch) { return arrow::util::TotalBufferSize(batch); }

// The batches of a materialised table, projected to `fields` and cut to `batch_size`.
class BatchesReader final : public arrow::RecordBatchReader {
 public:
  BatchesReader(std::shared_ptr<arrow::Schema> schema, arrow::RecordBatchVector batches)
      : schema_(std::move(schema)), batches_(std::move(batches)) {}

  [[nodiscard]] std::shared_ptr<arrow::Schema> schema() const override { return schema_; }

  arrow::Status ReadNext(std::shared_ptr<arrow::RecordBatch>* out) override {
    if (next_ >= batches_.size()) {
      *out = nullptr;
      return arrow::Status::OK();
    }
    *out = batches_[next_++];
    return arrow::Status::OK();
  }

 private:
  std::shared_ptr<arrow::Schema> schema_;
  arrow::RecordBatchVector batches_;
  std::size_t next_ = 0;
};

}  // namespace

void MaterializedTable::Reset(MemoryBudget* budget) {
  const std::scoped_lock lock(mutex_);
  batches_.clear();
  rows_ = 0;
  filled_ = false;
  memory_.Reset(budget);
}

arrow::Status MaterializedTable::Append(std::shared_ptr<arrow::RecordBatch> batch) {
  if (batch == nullptr) {
    return arrow::Status::OK();
  }
  const std::scoped_lock lock(mutex_);
  // Absolute, as MemoryReservation::Resize is: the running total plus this batch. A refusal leaves
  // the reservation and the rows as they were and fails the query, which is the loud failure a
  // shared result that outgrew its estimate should have (ADR 0026: memory is what broke this
  // workload before).
  ARROW_RETURN_NOT_OK(memory_.Resize(memory_.bytes() + BytesOf(*batch)));
  rows_ += batch->num_rows();
  batches_.push_back(std::move(batch));
  return arrow::Status::OK();
}

void MaterializedTable::Release() {
  const std::scoped_lock lock(mutex_);
  batches_.clear();
  rows_ = 0;
  filled_ = false;
  memory_.Release();
}

void MaterializedTable::Finish() {
  const std::scoped_lock lock(mutex_);
  filled_ = true;
}

std::optional<int64_t> MaterializedTable::exact_row_count() const {
  const std::scoped_lock lock(mutex_);
  // Only once filled: before that it is not that the table has no rows, it is that nobody knows,
  // and a planner that read 0 would make decisions from a number that is about to change.
  return filled_ ? std::optional<int64_t>(rows_) : std::nullopt;
}

arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> MaterializedTable::DoScan(
    const std::vector<int>& fields, int64_t batch_size, arrow::MemoryPool* /*pool*/) const {
  const std::scoped_lock lock(mutex_);
  if (!filled_) {
    // The operator that owns this fills it before it opens the consumers, so this cannot happen
    // from a plan the planner built -- and if it ever did, reading no rows would be a wrong answer
    // rather than an error, which is the one outcome worth refusing outright.
    return arrow::Status::Invalid("a shared sub-plan was read before it was computed");
  }
  arrow::FieldVector out_fields;
  out_fields.reserve(fields.size());
  for (const int field : fields) {
    if (field < 0 || field >= schema_->num_fields()) {
      return arrow::Status::Invalid("no field ", field, " of a shared sub-plan");
    }
    out_fields.push_back(schema_->field(field));
  }
  auto out_schema = arrow::schema(std::move(out_fields));
  arrow::RecordBatchVector out;
  for (const std::shared_ptr<arrow::RecordBatch>& batch : batches_) {
    for (int64_t start = 0; start < batch->num_rows(); start += batch_size) {
      const std::shared_ptr<arrow::RecordBatch> slice = batch->Slice(start, batch_size);
      arrow::ArrayVector columns;
      columns.reserve(fields.size());
      for (const int field : fields) {
        columns.push_back(slice->column(field));
      }
      out.push_back(arrow::RecordBatch::Make(out_schema, slice->num_rows(), std::move(columns)));
    }
  }
  return std::make_unique<BatchesReader>(std::move(out_schema), std::move(out));
}

}  // namespace antb1::exec
