#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <arrow/type_fwd.h>

#include "antb1/exec/grouped_aggregate_state.h"
#include "antb1/exec/operator.h"
#include "antb1/plan/logical_plan.h"

namespace arrow::compute {
class ExecContext;
class Grouper;
}  // namespace arrow::compute

namespace antb1::exec {

// Grouped aggregation (GROUP BY). Materializes the selected rows of each input batch, maps their
// keys to group ids with arrow::compute::Grouper (NULL is a key value; DOUBLE keys are normalized
// first, so -0.0 groups with 0.0 and every NaN with every other NaN) and feeds the rows to one
// GroupedAggregateState per call. After the input ends it emits the groups in the grouper's id
// order (deterministic for a given input, not the order of first appearance): the keys of each
// group as first seen, then one column per call, one batch per input batch that made new groups
// (so VARCHAR keys are never concatenated past the 2 GiB of one binary array). Over no input rows
// it emits nothing. Output: plan::ToArrow of each key's and call's type.
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
  arrow::Status Close() override { return input_->Close(); }

 private:
  arrow::Status Consume(const arrow::RecordBatch& rows);

  std::unique_ptr<Operator> input_;
  std::vector<plan::BoundColumn> keys_;
  std::vector<plan::AggregateCall> aggregates_;
  std::shared_ptr<arrow::Schema> schema_;
  arrow::MemoryPool* pool_ = nullptr;
  std::unique_ptr<arrow::compute::ExecContext> kernels_;
  std::unique_ptr<arrow::compute::Grouper> grouper_;
  std::vector<std::unique_ptr<GroupedAggregateState>> states_;
  std::vector<std::vector<std::shared_ptr<arrow::Array>>> first_keys_;  // per key, group order
  std::uint32_t num_groups_ = 0;
  bool done_ = false;
  // After the input: the finalized aggregates (one array per call, every group), and the next
  // chunk of first_keys_ to emit with the group it starts at.
  std::vector<std::shared_ptr<arrow::Array>> finalized_;
  std::size_t next_chunk_ = 0;
  std::int64_t next_group_ = 0;
};

}  // namespace antb1::exec
