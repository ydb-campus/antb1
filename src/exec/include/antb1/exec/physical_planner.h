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
// Limit -> LimitOperator, RowCount -> RowCountOperator; a Limit with a positive limit directly
// over a Sort becomes one SortOperator with that limit (a top-N). Invalid for a malformed plan (a
// node without its input or table, or a root whose width differs from plan.output). With a
// `profile` node, the plan is profiled into it (profile.h): one node per physical operator, each
// input a child, a part pipeline profiled once per part into the same nodes.
arrow::Result<std::unique_ptr<Operator>> BuildPhysicalPlan(const plan::LogicalPlan& plan,
                                                           ProfileNode* profile = nullptr);

}  // namespace antb1::exec
