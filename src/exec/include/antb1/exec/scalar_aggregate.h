#pragma once

#include <memory>
#include <vector>

#include "antb1/exec/aggregate_state.h"
#include "antb1/exec/operator.h"
#include "antb1/plan/logical_plan.h"

namespace antb1::exec {

// Global aggregation (no GROUP BY): consumes its whole input (selections included, without
// materializing them) into one AggregateState per call, then emits one row with one column per
// call. Output: plan::ToArrow of each call's result type.
class ScalarAggregateOperator final : public Operator {
 public:
  ScalarAggregateOperator(std::unique_ptr<Operator> input,
                          std::vector<plan::AggregateCall> aggregates);

  [[nodiscard]] const std::shared_ptr<arrow::Schema>& output_schema() const override {
    return schema_;
  }
  arrow::Status Open(ExecContext& ctx) override;
  arrow::Result<Batch> Next() override;
  arrow::Status Close() override { return input_->Close(); }

 private:
  std::unique_ptr<Operator> input_;
  std::vector<plan::AggregateCall> aggregates_;
  std::shared_ptr<arrow::Schema> schema_;
  std::vector<std::unique_ptr<AggregateState>> states_;
  arrow::MemoryPool* pool_ = arrow::default_memory_pool();
  bool done_ = false;
};

}  // namespace antb1::exec
