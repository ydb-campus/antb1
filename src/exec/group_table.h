#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <arrow/memory_pool.h>
#include <arrow/record_batch.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/type_fwd.h>

#include "antb1/exec/grouped_aggregate_state.h"
#include "antb1/exec/memory_budget.h"
#include "antb1/plan/logical_plan.h"

namespace arrow::compute {
class ExecContext;
class Grouper;
}  // namespace arrow::compute

namespace antb1::exec {

// The groups of a GROUP BY: a grouper over the keys (NULL is a key value; DOUBLE keys are
// normalized first, so -0.0 groups with 0.0 and every NaN with every other NaN), one
// GroupedAggregateState per call, and each group's key values as first seen, in chunks of new
// groups. GroupAggregateOperator consumes its whole input into one table;
// PartGroupAggregateOperator consumes each part into its own and merges them in part order
// (docs/adr/0013-parallel-execution.md). Groups are numbered by the grouper (deterministic for a
// given input, not the order of first appearance). Without keys every row is in one group. The
// states' own containers are charged to the budget. Single-threaded.
class GroupTable {
 public:
  // The most new groups a Merge adds as one chunk (one output batch).
  static constexpr std::size_t kMaxMergeChunk = std::size_t{64} * 1024;

  // Checks the keys and calls against an input `input_width` columns wide.
  static arrow::Result<std::unique_ptr<GroupTable>> Make(
      std::vector<plan::BoundColumn> keys, std::vector<plan::AggregateCall> aggregates,
      int input_width, arrow::MemoryPool* pool, MemoryBudget* budget);
  GroupTable(const GroupTable&) = delete;
  GroupTable& operator=(const GroupTable&) = delete;
  GroupTable(GroupTable&&) = delete;
  GroupTable& operator=(GroupTable&&) = delete;
  ~GroupTable();

  // Adds the rows of a batch without a selection.
  arrow::Status Consume(const arrow::RecordBatch& rows);
  // Adds the groups of `part` (a table over the same keys and calls, of the rows after this
  // table's): a group new to this table keeps part's first-seen key values; states merge.
  arrow::Status Merge(const GroupTable& part);

  [[nodiscard]] std::uint32_t num_groups() const { return num_groups_; }
  // The next chunk of groups as one batch (the keys as first seen, then one column per call, in
  // `schema`), or nullptr after the last. Moves the keys out: call it after the last Consume or
  // Merge.
  arrow::Result<std::shared_ptr<arrow::RecordBatch>> NextChunk(
      const std::shared_ptr<arrow::Schema>& schema);

 private:
  GroupTable(std::vector<plan::BoundColumn> keys, std::vector<plan::AggregateCall> aggregates,
             arrow::MemoryPool* pool);

  // Numbers `rows` groups new from num_groups_ on (a new chunk) and gives every state room.
  arrow::Status AddGroups(std::uint32_t after, std::vector<std::shared_ptr<arrow::Array>> keys);
  // Adds the groups new since num_groups_ (up to `after`) that `part`'s groups map to through
  // `group_map`, with part's first-seen keys, in chunks of at most kMaxMergeChunk groups.
  arrow::Status AddPartGroups(const GroupTable& part, std::span<const std::uint32_t> group_map,
                              std::uint32_t after);
  arrow::Status Account();

  std::vector<plan::BoundColumn> keys_;
  std::vector<plan::AggregateCall> aggregates_;
  arrow::MemoryPool* pool_;
  std::unique_ptr<arrow::compute::ExecContext> kernels_;
  std::unique_ptr<arrow::compute::Grouper> grouper_;  // nullptr without keys
  std::vector<std::unique_ptr<GroupedAggregateState>> states_;
  std::vector<std::vector<std::shared_ptr<arrow::Array>>> first_keys_;  // per key, per chunk
  std::vector<std::uint32_t> chunk_groups_;  // the number of new groups of each chunk
  std::uint32_t num_groups_ = 0;
  std::size_t next_chunk_ = 0;  // NextChunk: the next chunk and the group it starts at
  std::uint32_t next_group_ = 0;
  MemoryReservation memory_;  // the states' and chunk_groups_' containers
};

}  // namespace antb1::exec
