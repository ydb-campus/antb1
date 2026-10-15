#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "antb1/common/int128.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/table.h"

#include "scope.h"

// The join order of an inner block (docs/adr/0022-joins-and-query-blocks.md, "Join order",
// decision C9): greedy and left-deep, from footer statistics only, in integer arithmetic, so that a
// plan depends on metadata alone and is the same for any thread count. Private to the plan module:
// the binder, the optimizer's unnesting pass (which chooses its own join's build side by the same
// rule, ADR 0026) and their tests include it.

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

// An input of a join step: a relation of the block, or the result of an earlier step. Naming the
// inputs rather than "the relation this step adds" is what lets an order be something other than a
// chain, since either input may be a composite (ADR 0026's C5).
struct JoinOperand {
  bool step = false;      // whether `index` names a step rather than a relation
  std::size_t index = 0;  // a relation of the block, or a step before this one
};

// One join of an order: its two inputs and the edges between them. The steps are in dependency
// order, so every operand that names a step names an earlier one, and the last step is the root.
struct JoinStep {
  JoinOperand left;
  JoinOperand right;
  // The edges between the two inputs, ascending: the keys of this join. Never empty -- a step
  // joins two inputs that an edge connects, and the binder rejects a cross product.
  std::vector<std::size_t> edges;
  // The side this join builds on: its right input, unless that input's estimate exceeds its left
  // input's by more than `build_side_ratio` (ADR 0026). An unknown estimate on either side keeps
  // the right: unknown must never read as small.
  BuildSide build = BuildSide::kRight;
  int64_t estimate = 0;  // the rows this join outputs, estimated and saturated
};

// The first relation, in FROM order, that the edges do not connect to relation 0, or std::nullopt
// when they connect every relation (the binder rejects a disconnected graph: a cross product).
// Every edge must join two relations below `relations`.
std::optional<std::size_t> FirstUnconnected(std::size_t relations, std::span<const JoinEdge> edges);

// The join order of connected relations (FirstUnconnected gives std::nullopt), one step per join,
// so relations.size() - 1 of them, in dependency order (none for a single relation):
//   1. The probe, first, is the relation with the most rows (unknown counts as the most).
//   2. Then, of the relations with an edge to those already joined, the one with the smallest
//      estimate joins next. Its edges to them all become its keys, so the edge that closes a cycle
//      becomes a second key.
//   3. Joining R to rows L estimates |L| x |R| / d, d the largest domain of any of those edges'
//      sides, at least 1: computed in 128 bits, rounded down, at least 1 and saturated at
//      INT64_MAX. It is 0 when L or R has no row, or a side of the edges has an empty domain.
//   4. Ties go to the relation first in FROM.
//   5. Each step builds on its right input, unless that input's rows exceed its left input's
//      estimate by more than `build_side_ratio` hundredths, when it builds on the left instead
//      (JoinStep::build).
// Edges that do not join two different relations below relations.size(), relations that are not
// connected, or a ratio below 100, are a programming error.
std::vector<JoinStep> OrderJoins(std::span<const JoinRelation> relations,
                                 std::span<const JoinEdge> edges,
                                 int64_t build_side_ratio = kBuildSideRatio);

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

// The side a join builds on, from its two inputs' estimated rows (rule 5 of OrderJoins;
// docs/adr/0026-join-performance.md): `BuildSide::kRight`, the side every join built on before that
// ADR, unless the right input's estimate exceeds the left's by more than `build_side_ratio`
// hundredths, when it builds on the left instead. An unknown estimate on either side, and an
// estimate of no row at all, keep `kRight`: neither may be read as small, since building on a side
// that turns out to be large is the mistake the rule exists to avoid. A ratio below 100 is a
// programming error. One function so that the order's rule and the unnesting pass's cannot drift.
BuildSide ChooseBuildSide(const std::optional<int64_t>& left, const std::optional<int64_t>& right,
                          int64_t build_side_ratio = kBuildSideRatio);

// What the parts' footers say about one top-level column over the whole table, as the estimates of
// ADR 0026 read them: the range of its non-NULL values and how many of its rows are not NULL.
struct ColumnRange {
  std::optional<Int128> min;  // over the non-NULL values of every part; empty when all are NULL
  std::optional<Int128> max;
  int64_t non_null = 0;  // rows that are not NULL, summed over the parts; only when `exact`
  bool exact = false;    // every part had statistics with its min at most its max, or only NULLs
};

// The footer statistics of top-level field `field` of `table` (Table::part_stats). A column whose
// type has no statistics -- DECIMAL, VARCHAR, DOUBLE (ADR 0021) -- is not exact.
ColumnRange ColumnRangeOf(const Table& table, int field);

// KeyDomain's answer for a column whose range has already been read, so that an estimate needing
// both pays for one walk of the parts rather than two: rule 1 comes from `range` alone, and only a
// column without exact statistics is walked again for rule 2's distinct-count hints.
std::optional<int64_t> ColumnDomain(const Table& table, int field, const ColumnRange& range);

// The rows of `rows` that `predicate` keeps, estimated (ADR 0026): from `range` and `domain`
// (KeyDomain) where the footers have them, and from a documented default factor for every shape
// they do not cover -- a pattern match, a comparison of two columns, a computed BOOLEAN, and any
// column whose type has no statistics. Integer arithmetic throughout, as join-order rule 3 is, so
// that the estimate depends on the metadata alone and is the same on every host. Never below 0, and
// never above `rows`; `rows` must not be negative.
int64_t PredicateRows(int64_t rows, const Predicate& predicate, const ColumnRange& range,
                      std::optional<int64_t> domain);

// The rows of a binding: a table's exact row count, a sub-plan's estimate (EstimateRows).
std::optional<int64_t> RelationRows(const Binding& binding);

// The rows a plan's node outputs, estimated from footer row counts: a Scan's table's, one for an
// ungrouped Aggregate and a RowCount, its input's for any other single-input node (an upper bound),
// and a Join's own `estimate`, which the binder kept there because the domains rule 3 divides by
// are not reachable from a plan node (unknown where nothing estimated it). A DependentJoin reads
// through to its left input, which bounds it for every kind the unnesting pass admits: the outer of
// two nested dependent joins is rewritten while its left input still holds the inner one, so
// without that it could not choose a build side at all (ADR 0026).
std::optional<int64_t> EstimateRows(const LogicalNode& node);

}  // namespace antb1::plan
