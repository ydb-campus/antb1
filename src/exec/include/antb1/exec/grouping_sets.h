#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <arrow/type_fwd.h>

#include "antb1/exec/operator.h"
#include "antb1/plan/logical_plan.h"

namespace antb1::exec {

class GroupTable;          // src/exec/group_table.h
struct GroupingSetsShape;  // src/exec/grouping_sets_shape.h

// ROLLUP, CUBE and GROUPING SETS (plan::GroupingSetsNode; docs/adr/0038-grouping-sets.md), in two
// levels where every call merges exactly (MergesExactly in grouping_sets_shape.h): each set that no
// other set contains is aggregated from the input into a GroupTable of its own, as
// GroupAggregateOperator aggregates; then every other set is merged from the groups of the smallest
// such set that contains it (GroupTable::MergeProjected), with no finalization between. Otherwise,
// or with `per_set`, every distinct set is aggregated from the input into a table of its own. Each
// set's rows are then the rows GroupAggregateOperator would give for it. Every set's finalize
// errors are raised before the first batch. Output (GroupingSetsSchema): every key, NULL where a
// row's set lacks it, then one column per call, then one BIGINT per GROUPING call; the sets one
// after another in list order (a set listed twice emits its groups twice), each in its table's
// group order. The empty set has one row even over no input.
class GroupingSetsOperator final : public Operator {
 public:
  GroupingSetsOperator(std::unique_ptr<Operator> input, std::vector<plan::BoundColumn> keys,
                       const std::vector<std::vector<std::size_t>>& sets,
                       std::vector<plan::AggregateCall> aggregates,
                       std::vector<plan::GroupingCall> grouping, bool per_set = false);
  GroupingSetsOperator(const GroupingSetsOperator&) = delete;
  GroupingSetsOperator& operator=(const GroupingSetsOperator&) = delete;
  GroupingSetsOperator(GroupingSetsOperator&&) = delete;
  GroupingSetsOperator& operator=(GroupingSetsOperator&&) = delete;
  ~GroupingSetsOperator() override;

  [[nodiscard]] const std::shared_ptr<arrow::Schema>& output_schema() const override {
    return schema_;
  }
  arrow::Status Open(ExecContext& ctx) override;
  arrow::Result<Batch> Next() override;
  arrow::Status Close() override;

 private:
  // Consumes the input into the roots' tables, merges the other sets and checks every finalize.
  arrow::Status Aggregate();

  std::unique_ptr<Operator> input_;
  std::vector<plan::BoundColumn> keys_;
  std::vector<plan::AggregateCall> aggregates_;
  std::vector<plan::GroupingCall> grouping_;
  std::unique_ptr<GroupingSetsShape> shape_;
  std::shared_ptr<arrow::Schema> schema_;
  ExecContext ctx_;
  std::vector<std::unique_ptr<GroupTable>> tables_;  // per distinct set
  // Per distinct set: its rows (SetRows), built when first emitted, kept to its last entry.
  std::vector<std::vector<std::shared_ptr<arrow::RecordBatch>>> rows_;
  std::size_t next_entry_ = 0;  // the entry of the list being emitted
  std::size_t next_batch_ = 0;  // and its next batch
  bool opened_ = false;
  bool aggregated_ = false;
};

}  // namespace antb1::exec
