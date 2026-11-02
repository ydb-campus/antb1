#pragma once

#include <cstddef>
#include <optional>

#include "antb1/plan/branch_truth.h"
#include "antb1/plan/logical_plan.h"

// WHERE conditions into the branches of a UNION ALL, and the branches they rule out pruned
// ([ADR 0036](../../docs/adr/0036-union-all.md)'s X7a, the optimizer's rule 1). Private to the plan
// module: the optimizer runs the pass, and TruthInBranch is what the binder's estimate of a pruned
// union will share (ADR 0037's PF10).
namespace antb1::plan {

// The constant that column `id` of `node` holds in every row, typed as that column, or
// std::nullopt where that is not certain. Found only through nodes that keep a column's value
// (Filter, Sort, Limit, and a Compute that does not define it), an exact widening cast in a Compute
// (IsExactWidening, the binder's cast below a union) and a Project's constant; any other node ends
// the search, so a LEFT JOIN that pads NULLs or an aggregate never passes one through.
std::optional<Constant> ConstantColumn(const LogicalNodePtr& node, ColumnId id);

// What `predicate`, read over `node`'s output, is in every row of input `branch`: TruthOver of the
// constants that branch holds in the columns the predicate reads. Unknown for a column the union
// does not output and for a branch it does not have.
BranchTruth TruthInBranch(const UnionAllNode& node, std::size_t branch, const Predicate& predicate);

// Every Filter directly above a UnionAll -- the conditions the binder puts on a derived table or a
// CTE reference whose body is a union -- goes into the union's branches, in order:
// - a predicate never true in a branch (TruthInBranch) rules the branch out;
// - one always true there is dropped for it;
// - any other goes, with its columns read in the branch (UnionAllNode::columns, already of the
//   union's type), into a new Filter on top of the branch.
// The union keeps its ids and the branches left, in order; a union left one branch is a union of
// one input, so that nothing under it becomes an ADR 0030 shared result (ADR 0037's note on X7a).
// A union none of whose branches is left becomes Filter{FALSE} over a union of its first branch:
// above the union, which ends every part pipeline, it reads nothing. A Filter holding a FALSE, or
// one that reads a column the union does not output, stays as it is. Reads ids, never positions;
// running it twice changes nothing more.
LogicalNodePtr FilterIntoUnionBranches(const LogicalNodePtr& root);

}  // namespace antb1::plan
