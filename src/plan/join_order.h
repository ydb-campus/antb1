#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "antb1/plan/logical_plan.h"
#include "antb1/plan/table.h"

#include "scope.h"

// The join order of an inner block (docs/adr/0022-joins-and-query-blocks.md, "Join order",
// decision C9): greedy and left-deep, from footer statistics only, in integer arithmetic, so that a
// plan depends on metadata alone and is the same for any thread count. Private to the plan module:
// the binder and its tests include it.

namespace antb1::plan {

// The most relations a query may join (ADR 0022): the binder rejects more FROM items, which keeps
// every walk of a plan, recursive per node, bounded.
inline constexpr std::size_t kMaxRelations = 256;

// A relation of an inner block, in FROM order.
struct JoinRelation {
  std::optional<int64_t> rows;  // estimated (RelationRows); unknown counts as the largest
};

// An equality between a column of relation `a` and one of relation `b` that can be a join key.
struct JoinEdge {
  std::size_t a = 0;
  std::size_t b = 0;  // not a
  // The distinct values of each side's column (KeyDomain); unknown: that side's relation's rows.
  std::optional<int64_t> a_domain;
  std::optional<int64_t> b_domain;
};

// One step of a join order: the relation it joins to those of the earlier steps.
struct JoinStep {
  std::size_t relation = 0;
  // The edges between it and the relations of the earlier steps, ascending: the keys of its join
  // (none for the first step, the probe).
  std::vector<std::size_t> edges;
  // The side the step's join builds on: always the relation it adds. ADR 0022's rule, the input
  // with fewer rows (a join below standing in with its estimate), is deferred until the estimates
  // can be trusted (they ignore filters); it would be decided here.
  BuildSide build = BuildSide::kRight;
  int64_t estimate = 0;  // the estimated rows of the relations joined so far, saturated
};

// The first relation, in FROM order, that the edges do not connect to relation 0, or std::nullopt
// when they connect every relation (the binder rejects a disconnected graph: a cross product).
// Every edge must join two relations below `relations`.
std::optional<std::size_t> FirstUnconnected(std::size_t relations, std::span<const JoinEdge> edges);

// The join order of connected relations (FirstUnconnected gives std::nullopt), one step per
// relation:
//   1. The probe, first, is the relation with the most rows (unknown counts as the most).
//   2. Then, of the relations with an edge to those already joined, the one with the smallest
//      estimate joins next. Its edges to them all become its keys, so the edge that closes a cycle
//      becomes a second key.
//   3. Joining R to rows L estimates |L| x |R| / d, d the largest domain of any of those edges'
//      sides, at least 1: computed in 128 bits, rounded down, at least 1 and saturated at
//      INT64_MAX. It is 0 when L or R has no row, or a side of the edges has an empty domain.
//   4. Ties go to the relation first in FROM.
// Edges that do not join two different relations below relations.size(), or relations that are not
// connected, are a programming error.
std::vector<JoinStep> OrderJoins(std::span<const JoinRelation> relations,
                                 std::span<const JoinEdge> edges);

// The domain of top-level field `field` of `table` (the number of its distinct non-NULL values,
// estimated), from the parts' footer statistics:
//   1. when every part has exact statistics (Table::part_stats; a part with values needs its min
//      and max, the min at most the max), its integer range capped at its non-NULL values,
//      min(max - min + 1, non-NULL rows), saturated at INT64_MAX, so that a sparse key (few values
//      spread over a wide range) does not shrink every estimate to 1; 0 when every value is NULL;
//   2. else, when every part has a distinct-count hint (Table::part_distinct_count), the largest
//      of them, a lower bound of the column's count;
//   3. else std::nullopt (the estimate takes the relation's rows).
// A table without parts has no value: 0.
std::optional<int64_t> KeyDomain(const Table& table, int field);

// The rows of a binding: a table's exact row count, a sub-plan's estimate (EstimateRows).
std::optional<int64_t> RelationRows(const Binding& binding);

// The rows a plan's node outputs, estimated from footer row counts: a Scan's table's, one for an
// ungrouped Aggregate and a RowCount, its input's for any other single-input node (an upper bound),
// and unknown for a Join.
std::optional<int64_t> EstimateRows(const LogicalNode& node);

}  // namespace antb1::plan
