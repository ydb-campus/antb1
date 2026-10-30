#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "antb1/sql/ast.h"

#include "join_order.h"

// The relations a query block expands to (docs/adr/0022-joins-and-query-blocks.md, the relation
// limit): every FROM item of every block, a CTE's body once per reference. A derived table, a CTE
// reference and a subquery expression each count 1 plus their body's count, so the count bounds the
// size of the plan the binder will build -- which is what makes a chain of CTEs that each join the
// previous one with itself exit 4 before any sub-plan is bound, instead of binding 2^30 copies of
// the first, whether the chain is referenced from a FROM list or from a subquery in WHERE.
//
// The count is memoized per CTE and saturates at the limit, so neither the exponential chain nor an
// unreferenced CTE of 300 relations costs anything. Only reachable bodies are walked: an
// unreferenced CTE expands to nothing, as DuckDB never binds one.
//
// Private to the plan module: the binder and its tests include it. Counting is pure name matching
// over the AST, with no catalog and no binding, so it runs before anything resolves.

namespace antb1::plan {

// The limit itself is kMaxRelations, from join_order.h: a count that reaches it stops the walk
// rather than expanding the rest.

// What carries the count past the limit, if any: an index into the outermost block's FROM list, so
// the binder reports at an item the query text actually holds -- or, when it is one of that block's
// subquery expressions, that subquery's operator span, since a subquery in WHERE or HAVING has no
// FROM item of its own to report at.
struct RelationOverflow {
  std::size_t item = 0;
  std::optional<SourceSpan> span;
};

// The relations `block` expands to, saturated at kMaxRelations + 1; `std::nullopt` when the count
// stays within the limit. `block` is the outermost block, whose own WITH list is in scope for it.
std::optional<RelationOverflow> RelationsExceedLimit(const sql::SelectStatement& block);

// The relations `block` expands to, for tests and for the overflow search: saturated at
// kMaxRelations + 1, so a result of kMaxRelations + 1 means "at least that many".
std::size_t CountRelations(const sql::SelectStatement& block);

// Every block `block` reaches, outermost first: itself, each derived table's query, the query of
// each subquery expression, and the body of each CTE that a reached block references -- once per
// CTE however often it is referenced, and not at all when nothing references it, since DuckDB never
// binds an unreferenced one. Within a block, its FROM list's blocks come before its expressions',
// and a compound's branches come before the compound (ADR 0036), whose own clauses follow them, so
// a check over the result reports at the first unsupported construct in query order. Pure name
// matching, like the count, so a check over the result runs before anything resolves.
std::vector<const sql::SelectStatement*> ReachableBlocks(const sql::SelectStatement& block);

}  // namespace antb1::plan
