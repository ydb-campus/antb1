#pragma once

#include <memory>

#include <arrow/result.h>

#include "antb1/exec/operator.h"
#include "antb1/plan/logical_plan.h"

namespace antb1::exec {

arrow::Result<std::unique_ptr<Operator>> BuildPhysicalPlan(const plan::LogicalPlan& plan);

}  // namespace antb1::exec
