#include "antb1/exec/limit.h"

#include <cstdint>
#include <memory>
#include <utility>

#include <arrow/api.h>

namespace antb1::exec {

LimitOperator::LimitOperator(std::unique_ptr<Operator> input, int64_t limit)
    : input_(std::move(input)), limit_(limit) {}

arrow::Status LimitOperator::Open(ExecContext& ctx) {
  if (limit_ < 0) {
    return arrow::Status::Invalid("LIMIT ", limit_, " is negative");
  }
  pool_ = ctx.pool;
  emitted_ = 0;
  return input_->Open(ctx);
}

arrow::Result<Batch> LimitOperator::Next() {
  while (emitted_ < limit_) {
    ARROW_ASSIGN_OR_RAISE(Batch in, input_->Next());
    if (in.end()) {
      return in;
    }
    ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::RecordBatch> rows, Materialize(in, pool_));
    const int64_t wanted = limit_ - emitted_;
    if (rows->num_rows() > wanted) {
      rows = rows->Slice(0, wanted);
    }
    if (rows->num_rows() == 0) {
      continue;
    }
    emitted_ += rows->num_rows();
    return Batch{.data = std::move(rows), .selection = {}};
  }
  return Batch{};  // the limit is reached: the input is not pulled again
}

}  // namespace antb1::exec
