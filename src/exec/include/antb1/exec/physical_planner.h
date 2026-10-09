#pragma once

#include <memory>

#include <arrow/result.h>

#include "antb1/exec/operator.h"
#include "antb1/exec/profile.h"
#include "antb1/plan/logical_plan.h"

namespace antb1::exec {

// Builds the operator tree of an (optimized) logical plan, one operator per node: Scan ->
// TableScanOperator, Filter -> FilterOperator, Project -> ProjectOperator, Aggregate ->
// ScalarAggregateOperator, GroupAggregate -> GroupAggregateOperator, Sort -> SortOperator,
// Limit -> LimitOperator, RowCount -> RowCountOperator, a Join of any kind -> a hash join (a build
// of one input probed by the other, keyless for a one-row join,
// docs/adr/0022-joins-and-query-blocks.md); a Limit with a positive limit directly over a Sort
// becomes one SortOperator with that limit (a top-N). A chain of Filter, Compute, Project and
// joins' probes over a scan runs once per table part (a part pipeline). Invalid for a malformed
// plan (a node without its input or table; a join of another kind than inner that builds on its
// left input; a one-row join with keys or residuals; a join of another kind without keys; a
// null-aware anti join of other than one key or with residuals; a key of two types, of DOUBLE or
// BOOLEAN, or outside its input; a residual that is missing, not BOOLEAN or reads outside the
// join's two inputs; a root whose width differs from plan.output), with a profile or without.
// With a `profile` node, the plan is profiled into it (profile.h): one node per physical operator,
// each input a child, a part pipeline profiled once per part into the same nodes, a join's build
// under the operator that prepares it.
arrow::Result<std::unique_ptr<Operator>> BuildPhysicalPlan(const plan::LogicalPlan& plan,
                                                           ProfileNode* profile = nullptr);

}  // namespace antb1::exec
