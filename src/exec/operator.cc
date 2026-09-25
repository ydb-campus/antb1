#include "antb1/exec/operator.h"

#include <memory>
#include <vector>

#include <arrow/api.h>

namespace antb1::exec {

arrow::Result<std::shared_ptr<arrow::Table>> Drain(Operator& op, ExecContext& ctx) {
  ARROW_RETURN_NOT_OK(op.Open(ctx));
  std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
  while (true) {
    ARROW_ASSIGN_OR_RAISE(auto batch, op.Next());
    if (!batch) {
      break;
    }
    batches.push_back(std::move(batch));
  }
  ARROW_RETURN_NOT_OK(op.Close());
  return arrow::Table::FromRecordBatches(op.output_schema(), batches);
}

}  // namespace antb1::exec
