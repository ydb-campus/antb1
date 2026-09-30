#include "profiled_operator.h"

#include <chrono>

#include <arrow/result.h>
#include <arrow/status.h>

#include "antb1/exec/operator.h"
#include "antb1/exec/profile.h"

namespace antb1::exec {
namespace {

using Clock = std::chrono::steady_clock;

}  // namespace

arrow::Status ProfiledOperator::Open(ExecContext& ctx) {
  const auto start = Clock::now();
  arrow::Status status = input_->Open(ctx);
  node_->AddTime(Clock::now() - start);
  node_->AddInstance();
  return status;
}

arrow::Result<Batch> ProfiledOperator::Next() {
  const auto start = Clock::now();
  arrow::Result<Batch> batch = input_->Next();
  node_->AddTime(Clock::now() - start);
  if (batch.ok() && !batch->end()) {
    node_->AddRows(batch->selected_rows());
  }
  return batch;
}

arrow::Status ProfiledOperator::Close() {
  const auto start = Clock::now();
  arrow::Status status = input_->Close();
  node_->AddTime(Clock::now() - start);
  return status;
}

}  // namespace antb1::exec
