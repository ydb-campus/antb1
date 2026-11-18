#pragma once

#include <memory>
#include <vector>

#include "antb1/exec/operator.h"
#include "antb1/plan/logical_plan.h"

namespace antb1::exec {

// Whole-partition aggregates (ADR 0043, W2). One upstream execution, shared grouping for
// compatible calls, one finalization per partition, and bounded gathers back to input row order.
class WindowOperator final : public Operator {
 public:
  WindowOperator(std::unique_ptr<Operator> input, std::vector<plan::WindowCall> calls);
  WindowOperator(const WindowOperator&) = delete;
  WindowOperator& operator=(const WindowOperator&) = delete;
  WindowOperator(WindowOperator&&) = delete;
  WindowOperator& operator=(WindowOperator&&) = delete;
  ~WindowOperator() override;
  const std::shared_ptr<arrow::Schema>& output_schema() const override { return schema_; }
  arrow::Status Open(ExecContext& ctx) override;
  arrow::Result<Batch> Next() override;
  arrow::Status Close() override;

 private:
  struct State;
  arrow::Status Prepare();
  arrow::Result<Batch> NextBatch();
  std::unique_ptr<Operator> input_;
  std::vector<plan::WindowCall> calls_;
  std::shared_ptr<arrow::Schema> schema_;
  std::unique_ptr<State> state_;
  bool input_open_ = false;
  bool done_ = false;
};

}  // namespace antb1::exec
