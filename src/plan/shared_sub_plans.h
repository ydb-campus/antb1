#pragma once

#include <cstdint>
#include <vector>

#include "antb1/plan/logical_plan.h"

// Sub-plans of one plan that compute the same rows (docs/adr/0030-shared-sub-plans.md, E1).
//
// Nothing here shares anything: it reports what a later item could compute once. A plan built by
// the binder holds a subtree per reference -- ADR 0022 binds a CTE's body once per reference, and a
// subquery that repeats a FROM list produces the same duplication with no CTE in it -- and nothing
// after the binder notices that two subtrees are the same.
//
// Module-internal on purpose: E1's consumers are explain.cc and this module's tests. It moves to
// src/plan/include/ when exec needs it, not before (ADR 0027's S1: a producer shipped ahead of its
// consumer is a producer nothing measures).

namespace antb1::plan {

// One group of equivalent sub-plans.
struct SharedSubPlan {
  // Two or more, distinct nodes, in the order a walk from the root meets them. Every member
  // computes the same rows in the same order as every other.
  std::vector<LogicalNodePtr> members;
  // The table fields the one shared sub-plan would read: per scan, the union over the members, and
  // the sum of those. A member reads at most this many, so the difference is what sharing would
  // decode for a consumer that does not want it.
  int64_t fields = 0;
};

// The maximal groups of `plan`, in the order a walk from the root meets them; empty when nothing
// repeats.
//
// Two sub-plans are equivalent when they have the same shape and the same per-node content **with
// every column reference compared by where its column comes from** rather than by its position or
// its id, and their scans read the same table. The fields a scan reads are deliberately *not* part
// of the comparison: selecting fields does not change which rows a scan returns, so two scans of
// one table differing only there compute the same rows, and the shared sub-plan reads the union of
// what its consumers want. That is what makes this reach beyond exact equality -- and it is the
// whole of the difference in two of the three queries the ADR measures.
//
// Why by origin and not by id: BoundColumn::index is a position in the input, so a branch whose
// scan reads one field fewer has different indices in every node above it; and a ColumnId is minted
// per reference, so two references to one CTE never share one. Only the source of a column is the
// same on both sides.
//
// Groups are **outermost-first and never wholly nested**: where a subtree repeats so does every
// subtree inside it, and a group all of whose members lie inside another group's members is not
// reported -- the outer one is what a later item would materialise. A group with even one member
// outside is still reported, and its `members` then do include the nested ones, because sharing the
// outer group leaves that outside member duplicating work the report would otherwise hide. Three
// scans of one table where two of them sit under equivalent filters is exactly that shape.
//
// A sub-plan that holds a Sort, a Limit, a RowCount or a dependent join is never a member. Order
// and row limits are not properties a shared result can hand to two consumers without deciding
// whose order wins, a metadata row count is not worth sharing, and a dependent join cannot reach
// here (plan::Optimize removes every one). Refusing is always safe: it reports less.
std::vector<SharedSubPlan> SharedSubPlans(const LogicalPlan& plan);

}  // namespace antb1::plan
