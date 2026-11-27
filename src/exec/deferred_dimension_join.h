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
arrow::Result<std::unique_ptr<Operator>> MakeDeferredDimensionJoin(plan::LogicalPlan fast,
                                                                   plan::LogicalPlan original,
                                                                   ProfileNode* profile,
                                                                   DimensionPlanBuilder build);

}  // namespace antb1::exec
