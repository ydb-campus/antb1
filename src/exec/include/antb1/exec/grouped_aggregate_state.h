#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include <arrow/array/array_base.h>
#include <arrow/memory_pool.h>
#include <arrow/result.h>
#include <arrow/status.h>

#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

// Aggregate functions of the grouped aggregation (GROUP BY): the semantics of the scalar states in
// aggregate_state.h (exact 128-bit integer SUM and AVG, DOUBLE sums in row order, NaN ignored by
// MIN and MAX unless every value is NaN, NULL values skipped), kept per group; COUNT(DISTINCT)
// keeps the distinct (group, value) pairs. Rows arrive already selected (WHERE applied), each with
// the id of its group.
//
// Merge folds the groups of another state into this one through a group map. Partial states built
// over separate parts of the input (for example row groups read by different threads) and merged
// in input order give exactly the single-pass result, except DOUBLE SUM and AVG: those add the
// parts' partial sums, which can round differently from one running sum, but are still the same
// for the same part boundaries whatever the thread count.

namespace antb1::exec {

class GroupedAggregateState {
 public:
  GroupedAggregateState() = default;
  GroupedAggregateState(const GroupedAggregateState&) = delete;
  GroupedAggregateState& operator=(const GroupedAggregateState&) = delete;
  GroupedAggregateState(GroupedAggregateState&&) = delete;
  GroupedAggregateState& operator=(GroupedAggregateState&&) = delete;
  virtual ~GroupedAggregateState() = default;

  [[nodiscard]] virtual std::uint32_t num_groups() const = 0;
  // Grows the state to `num_groups` groups; new groups have seen no value. Never shrinks.
  virtual void Resize(std::uint32_t num_groups) = 0;
  // The bytes the state holds outside Arrow buffers (its per-group vectors and VARCHAR values),
  // which operators charge to their MemoryBudget. Arrow buffers are counted by their pool.
  [[nodiscard]] virtual std::int64_t memory_usage() const = 0;
  // Adds rows: row i of `values` belongs to group group_ids[i] (< num_groups()). `values` is
  // nullptr for COUNT(*), which counts group_ids.size() rows.
  virtual arrow::Status Consume(const arrow::Array* values,
                                std::span<const std::uint32_t> group_ids) = 0;
  // Folds group g of `other` (the same aggregate over the same input type) into group
  // group_map[g] of this state, for every g < other.num_groups().
  virtual arrow::Status Merge(const GroupedAggregateState& other,
                              std::span<const std::uint32_t> group_map) = 0;
  // One value per group of [begin, end), in group order: an array of plan::ToArrow(result type).
  // Invalid unless begin <= end <= num_groups(). Finalizing the groups in ranges keeps each array
  // small (a VARCHAR MIN or MAX over millions of groups would not fit the 2 GiB of one binary
  // array).
  [[nodiscard]] virtual arrow::Result<std::shared_ptr<arrow::Array>> Finalize(
      std::uint32_t begin, std::uint32_t end, arrow::MemoryPool* pool) const = 0;
  // Every group.
  [[nodiscard]] arrow::Result<std::shared_ptr<arrow::Array>> Finalize(
      arrow::MemoryPool* pool) const {
    return Finalize(0, num_groups(), pool);
  }
};

// The grouped state of `kind` over an argument of type `input` (std::nullopt for COUNT(*))
// producing `result`; `pool` holds the buffers of COUNT(DISTINCT)'s grouper. Invalid for a
// combination the binder never produces.
arrow::Result<std::unique_ptr<GroupedAggregateState>> MakeGroupedAggregateState(
    plan::AggKind kind, std::optional<plan::LogicalType> input, plan::LogicalType result,
    arrow::MemoryPool* pool = arrow::default_memory_pool());

}  // namespace antb1::exec
