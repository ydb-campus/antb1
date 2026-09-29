#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <arrow/type_fwd.h>

#include "antb1/exec/operator.h"
#include "antb1/plan/logical_plan.h"

namespace antb1::exec {

class GroupTable;  // src/exec/group_table.h

// Grouped aggregation (GROUP BY). Materializes the selected rows of each input batch, maps their
// keys to group ids with arrow::compute::Grouper (NULL is a key value; DOUBLE keys are normalized
// first, so -0.0 groups with 0.0 and every NaN with every other NaN) and feeds the rows to one
// GroupedAggregateState per call. After the input ends it emits the groups in the grouper's id
// order (deterministic for a given input, not the order of first appearance): the keys of each
// group as first seen, then one column per call, one batch per input batch that made new groups
// (so neither VARCHAR keys nor VARCHAR MIN/MAX values are gathered past the 2 GiB of one binary
// array). Over no input rows it emits nothing. Without keys (GROUP BY constants only) every row is
// in one group, emitted when there was any row. Output: plan::ToArrow of each key's and call's
// type.
class GroupAggregateOperator final : public Operator {
 public:
  GroupAggregateOperator(std::unique_ptr<Operator> input, std::vector<plan::BoundColumn> keys,
                         std::vector<plan::AggregateCall> aggregates);
  GroupAggregateOperator(const GroupAggregateOperator&) = delete;
  GroupAggregateOperator& operator=(const GroupAggregateOperator&) = delete;
  GroupAggregateOperator(GroupAggregateOperator&&) = delete;
  GroupAggregateOperator& operator=(GroupAggregateOperator&&) = delete;
  ~GroupAggregateOperator() override;

  [[nodiscard]] const std::shared_ptr<arrow::Schema>& output_schema() const override {
    return schema_;
  }
  arrow::Status Open(ExecContext& ctx) override;
  arrow::Result<Batch> Next() override;
  arrow::Status Close() override;

 private:
  std::unique_ptr<Operator> input_;
  std::vector<plan::BoundColumn> keys_;
  std::vector<plan::AggregateCall> aggregates_;
  std::shared_ptr<arrow::Schema> schema_;
  arrow::MemoryPool* pool_ = nullptr;
  std::unique_ptr<GroupTable> table_;  // from Open to the end of the output
  bool opened_ = false;
  bool done_ = false;
};

}  // namespace antb1::exec
