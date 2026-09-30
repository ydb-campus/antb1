#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <arrow/compute/exec.h>
#include <arrow/memory_pool.h>
#include <arrow/record_batch.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/type_fwd.h>

#include "antb1/exec/grouped_aggregate_state.h"
#include "antb1/exec/memory_budget.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

#include "group_table.h"

namespace arrow::compute {
class ExecContext;
class Grouper;
}  // namespace arrow::compute

namespace antb1::exec {

// Where an output column of a two-level aggregation comes from: the count of distinct values of
// distinct column `index`, or the state of plain call `index`.
struct OuterSlot {
  bool distinct = false;
  std::size_t index = 0;
};

// The outer level of a two-level aggregation (docs/adr/0014-two-level-aggregation.md): groups by
// the outer keys K, each with a count per distinct column and a state per plain call. Inner groups
// arrive from the inner tables of one partition: a table over K and a distinct column x adds one
// to its column's count for each of its groups whose x is not NULL (the inner table holds each
// (K, x) once); the plain table over K merges its states. Outer tables of different partitions
// merge by their keys (Merge), counts adding up: every (K, x) is in exactly one partition. Keys
// are never DOUBLE, so the grouper's normalized keys are the key values themselves. Without keys
// there is at most one group. Its containers are charged to the budget. Single-threaded.
class OuterGroups {
 public:
  static arrow::Result<std::unique_ptr<OuterGroups>> Make(
      std::vector<plan::LogicalType> key_types, std::size_t distinct_columns,
      const std::vector<plan::AggregateCall>& plain, arrow::MemoryPool* pool, MemoryBudget* budget);
  OuterGroups(const OuterGroups&) = delete;
  OuterGroups& operator=(const OuterGroups&) = delete;
  OuterGroups(OuterGroups&&) = delete;
  OuterGroups& operator=(OuterGroups&&) = delete;
  ~OuterGroups();

  // The groups of an inner table over K and distinct column `column` (its uniques: K, then x).
  arrow::Status AddDistinct(std::size_t column, const arrow::compute::ExecBatch& inner);
  // The groups of the plain table over K (its calls are this table's plain calls).
  arrow::Status AddPlain(const GroupTable& plain);
  // Folds groups `from` of `other` (the same shape) into the groups of their keys.
  arrow::Status Merge(const OuterGroups& other, std::span<const std::uint32_t> from);
  // Adds the group of a table without keys if there is none: a global aggregation has one row.
  arrow::Status EnsureGroup();

  [[nodiscard]] std::uint32_t num_groups() const { return num_groups_; }
  // The hash (KeyHashes) of each group's keys, in group order (with keys only).
  [[nodiscard]] arrow::Result<std::vector<std::uint64_t>> Hashes() const;
  // The groups `groups` (ascending, in [begin, end), end - begin at most
  // GroupTable::kMaxMergeChunk) as one batch of `schema`: the keys (unless `with_keys` is false),
  // then one column per slot.
  arrow::Result<std::shared_ptr<arrow::RecordBatch>> Rows(
      std::uint32_t begin, std::uint32_t end, std::span<const std::uint32_t> groups,
      const std::vector<OuterSlot>& slots, const std::shared_ptr<arrow::Schema>& schema,
      bool with_keys);

 private:
  OuterGroups(std::vector<plan::LogicalType> key_types, arrow::MemoryPool* pool);

  // The groups of the key rows `keys` (the first key_types_.size() columns), added if new.
  arrow::Result<std::vector<std::uint32_t>> GroupsOf(const arrow::compute::ExecBatch& keys);
  arrow::Status Resize(std::uint32_t groups);
  arrow::Status Account();

  std::vector<plan::LogicalType> key_types_;
  arrow::MemoryPool* pool_;
  std::unique_ptr<arrow::compute::ExecContext> kernels_;
  std::unique_ptr<arrow::compute::Grouper> grouper_;  // nullptr without keys
  std::uint32_t num_groups_ = 0;
  std::vector<std::vector<std::int64_t>> counts_;               // per distinct column, per group
  std::vector<std::unique_ptr<GroupedAggregateState>> states_;  // per plain call
  std::optional<arrow::compute::ExecBatch> uniques_;            // Rows: the keys, taken once
  MemoryReservation memory_;
};

}  // namespace antb1::exec
