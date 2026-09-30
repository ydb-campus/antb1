#pragma once

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
  arrow::Status Open(ExecContext& ctx) override;
  arrow::Result<Batch> Next() override;
  arrow::Status Close() override;

 private:
  std::unique_ptr<Operator> input_;
  ProfileNode* node_;
};

}  // namespace antb1::exec
