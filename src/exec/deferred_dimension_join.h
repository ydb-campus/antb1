#pragma once

#include <memory>

#include <arrow/result.h>

#include "antb1/exec/operator.h"
#include "antb1/plan/logical_plan.h"

namespace antb1::exec {

using DimensionPlanBuilder = arrow::Result<std::unique_ptr<Operator>> (*)(const plan::LogicalPlan&,
                                                                          ProfileNode*);

// The candidate reads the dimension keys of this execution before it can commit to the late join.
// The original plan supplies the output schema and runs if the proof or candidate fails.
arrow::Result<std::unique_ptr<Operator>> MakeDeferredDimensionJoin(
    plan::LogicalPlan fast, plan::LogicalPlan original, ProfileNode* profile,
    DimensionPlanBuilder build, DimensionPlanBuilder build_fast = nullptr);

// An empty combined inner join need not have read both original inputs. Verify the sorted
// attempt and its scans before returning any rows; otherwise retry the independent groups.
arrow::Result<std::unique_ptr<Operator>> MakeGuardedSharedGroups(const plan::LogicalPlan& plan,
                                                                 ProfileNode* profile,
                                                                 DimensionPlanBuilder independent,
                                                                 DimensionPlanBuilder combined);

}  // namespace antb1::exec
