#include "antb1/exec/operator.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <new>
#include <utility>

#include <arrow/api.h>
#include <arrow/compute/api_vector.h>
#include <arrow/compute/exec.h>

#include "antb1/plan/table.h"

#include "binary_view.h"
#include "part_scheduler.h"

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
  if (HasBinaryViews(*batch.data->schema())) {
    return FilterViewBatch(batch.data, batch.selection, pool, BinaryViewRetention::kShare);
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

// PullAll, with running out of memory outside the budget (std::bad_alloc from a container) as an
// OutOfMemory status: no exception leaves exec.
arrow::Result<arrow::RecordBatchVector> PullAllChecked(Operator& op, arrow::MemoryPool* pool) {
  try {
    return PullAll(op, pool);
  } catch (const std::bad_alloc&) {
    return arrow::Status::OutOfMemory("out of memory");
  }
}

arrow::Status FinishScanState(plan::TableScanState& state, arrow::MemoryPool* pool,
                              const ExecContext& ctx) {
  const int64_t parts = state.num_finish_parts();
  if (ctx.executor == nullptr || parts <= 1) {
    return state.Finish(pool);
  }
  // A small footer can cost less to validate than to schedule. Use a few ordered batches per
  // worker rather than allocating a future and result slot for every file. A batch retains only
  // one footer buffer at a time; retrying it after OutOfMemory repeats harmless content checks.
  const int64_t threads = std::max(1, ctx.threads);
  const int64_t batches = std::min(parts, 4 * threads);
  const int64_t width = parts / batches;
  const int64_t extra = parts % batches;
  PartScheduler<int> checks(
      batches,
      [&state, pool, width, extra](int64_t batch,
                                   const std::atomic<bool>& stop) -> arrow::Result<int> {
        const int64_t begin = (batch * width) + std::min(batch, extra);
        const int64_t end = begin + width + (batch < extra ? 1 : 0);
        for (int64_t part = begin; part < end && !stop; ++part) {
          ARROW_RETURN_NOT_OK(state.FinishPart(part, pool));
        }
        return 0;
      },
      ctx.executor, threads, ctx.budget);
  while (!checks.done()) {
    const auto checked = checks.Next();
    ARROW_RETURN_NOT_OK(checked.status());
  }
  return arrow::Status::OK();
}

}  // namespace

arrow::Result<std::shared_ptr<arrow::Table>> Drain(Operator& op, const ExecContext& ctx) {
  ExecContext run = ctx;
  try {
    run.scans = std::make_shared<plan::ScanContext>();
  } catch (const std::bad_alloc&) {
    return arrow::Status::OutOfMemory("out of memory preparing query scans");
  }
  ARROW_RETURN_NOT_OK(op.Open(run));
  auto batches = PullAllChecked(op, run.pool);
  const arrow::Status closed = op.Close();  // also after a failure: release files and readers
  ARROW_RETURN_NOT_OK(batches.status());
  ARROW_RETURN_NOT_OK(closed);
  ARROW_RETURN_NOT_OK(
      run.scans->Finish(run.pool, [&run](plan::TableScanState& state, arrow::MemoryPool* pool) {
        return FinishScanState(state, pool, run);
      }));
  return arrow::Table::FromRecordBatches(op.output_schema(), *batches);
}

}  // namespace antb1::exec
