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
// Limit -> LimitOperator, RowCount -> RowCountOperator, an inner Join -> a hash join (a build of
// one input probed by the other, docs/adr/0022-joins-and-query-blocks.md); a Limit with a positive
// limit directly over a Sort becomes one SortOperator with that limit (a top-N). A chain of
// Filter, Compute, Project and inner joins' probes over a scan runs once per table part (a part
// pipeline). A join of another kind is unsupported (exit code 4: a kUnsupported SqlErrorDetail at
// the join's span). Invalid for a malformed plan (a node without its input or table; a join
// without keys, with a key of two types, of DOUBLE or BOOLEAN, or outside its input; a residual
// that is missing, not BOOLEAN or reads outside the join; a root whose width differs from
// plan.output), with a profile or without.
// With a `profile` node, the plan is profiled into it (profile.h): one node per physical operator,
// each input a child, a part pipeline profiled once per part into the same nodes, a join's build
// under the operator that prepares it.
arrow::Result<std::unique_ptr<Operator>> BuildPhysicalPlan(const plan::LogicalPlan& plan,
                                                           ProfileNode* profile = nullptr);

}  // namespace antb1::exec
