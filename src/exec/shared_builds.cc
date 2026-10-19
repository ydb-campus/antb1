#include "shared_builds.h"

#include <memory>
#include <utility>

#include <arrow/record_batch.h>
#include <arrow/result.h>
#include <arrow/status.h>

#include "antb1/exec/operator.h"
#include "antb1/exec/profile.h"

namespace antb1::exec {

arrow::Status SharedBuildsOperator::Open(ExecContext& ctx) {
  // Every one of them, to completion, before the consumers exist to read any of them. A failure
  // here fails the query: a half-computed shared result would be a wrong answer, which is the one
  // outcome worth refusing outright (MaterializedTable::DoScan refuses an unfinished table too,
  // so a bug on this path cannot read as "no rows").
  for (const SharedBuild& build : builds_) {
    if (build.op == nullptr || build.table == nullptr) {
      return arrow::Status::Invalid("a shared sub-plan without its operator or its table");
    }
    const ProfileTimer timer(profile(), "shared");
    build.table->Reset(ctx.budget);
    ARROW_RETURN_NOT_OK(build.op->Open(ctx));
    arrow::Status status = arrow::Status::OK();
    while (status.ok()) {
      arrow::Result<Batch> batch = build.op->Next();
      if (!batch.ok()) {
        status = batch.status();
        break;
      }
      if (batch->end()) {
        break;
      }
      // Materialized rather than kept with its selection: the consumers scan this as a table, and
      // a table's rows are its rows.
      arrow::Result<std::shared_ptr<arrow::RecordBatch>> rows = Materialize(*batch, ctx.pool);
      if (!rows.ok()) {
        status = rows.status();
        break;
      }
      if (*rows != nullptr && (*rows)->num_rows() > 0) {
        status = build.table->Append(*std::move(rows));
      }
    }
    const arrow::Status closed = build.op->Close();
    ARROW_RETURN_NOT_OK(status);
    ARROW_RETURN_NOT_OK(closed);
    build.table->Finish();
    if (profile() != nullptr) {
      profile()->Add("shared_rows", MetricUnit::kCount, build.table->exact_row_count().value_or(0));
    }
  }
  return input_->Open(ctx);
}

arrow::Result<Batch> SharedBuildsOperator::Next() { return input_->Next(); }

arrow::Status SharedBuildsOperator::Close() {
  const arrow::Status status = input_->Close();
  // Given back here and not before: a consumer may read its shared result at any point up to the
  // end of the query, and nothing tracks which was the last.
  for (const SharedBuild& build : builds_) {
    if (build.table != nullptr) {
      build.table->Release();
    }
  }
  return status;
}

}  // namespace antb1::exec
