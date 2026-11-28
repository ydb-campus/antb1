#pragma once

#include <cstddef>
#include <memory>

#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/type_fwd.h>

#include "antb1/exec/operator.h"
#include "antb1/exec/profile.h"

namespace antb1::exec {

// An operator as profiled (docs/adr/0015-query-profiles.md): the wall time of the wrapped
// operator's Open, Next and Close (its inputs' included when they run on the same thread), its
// runs (Open calls: one per part in a part pipeline) and the rows and batches it returns, added
// to its ProfileNode.
class ProfiledOperator final : public Operator {
 public:
  ProfiledOperator(std::unique_ptr<Operator> input, ProfileNode* node)
      : input_(std::move(input)), node_(node) {}

  [[nodiscard]] const std::shared_ptr<arrow::Schema>& output_schema() const override {
    return input_->output_schema();
  }
  void SetMayStopEarly() override { input_->SetMayStopEarly(); }
  const BatchPartition* RequestPartitioning(int column, PartitionRequest request) override {
    return input_->RequestPartitioning(column, request);
  }
  [[nodiscard]] std::size_t ReadyBatches(std::size_t limit, int64_t min_rows,
                                         int64_t max_rows) const override {
    return input_->ReadyBatches(limit, min_rows, max_rows);
  }
  arrow::Status Open(ExecContext& ctx) override;
  arrow::Result<Batch> Next() override;
  arrow::Status Close() override;

 private:
  std::unique_ptr<Operator> input_;
  ProfileNode* node_;
};

}  // namespace antb1::exec
