#include "antb1/exec/operator.h"

#include <cstdint>
#include <memory>
#include <utility>

#include <arrow/api.h>
#include <arrow/compute/api_vector.h>
#include <arrow/compute/exec.h>

namespace antb1::exec {

int64_t Batch::selected_rows() const {
  if (data == nullptr) {
    return 0;
  }
  return selection == nullptr ? data->num_rows() : selection->true_count();
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> Materialize(const Batch& batch,
                                                               arrow::MemoryPool* pool) {
  if (batch.data == nullptr || batch.selection == nullptr) {
    return batch.data;
  }
  if (batch.data->num_columns() == 0) {
    return arrow::RecordBatch::Make(batch.data->schema(), batch.selected_rows(),
                                    arrow::ArrayVector{});
  }
  arrow::compute::ExecContext kernels(pool);
  ARROW_ASSIGN_OR_RAISE(
      const arrow::Datum filtered,
      arrow::compute::Filter(batch.data, batch.selection, arrow::compute::FilterOptions::Defaults(),
                             &kernels));
  return filtered.record_batch();
}

namespace {

arrow::Result<arrow::RecordBatchVector> PullAll(Operator& op, arrow::MemoryPool* pool) {
  arrow::RecordBatchVector batches;
  while (true) {
    ARROW_ASSIGN_OR_RAISE(const Batch batch, op.Next());
    if (batch.end()) {
      return batches;
    }
    ARROW_ASSIGN_OR_RAISE(auto rows, Materialize(batch, pool));
    batches.push_back(std::move(rows));
  }
}

}  // namespace

arrow::Result<std::shared_ptr<arrow::Table>> Drain(Operator& op, ExecContext& ctx) {
  ARROW_RETURN_NOT_OK(op.Open(ctx));
  auto batches = PullAll(op, ctx.pool);
  const arrow::Status closed = op.Close();  // also after a failure: release files and readers
  ARROW_RETURN_NOT_OK(batches.status());
  ARROW_RETURN_NOT_OK(closed);
  return arrow::Table::FromRecordBatches(op.output_schema(), *batches);
}

}  // namespace antb1::exec
