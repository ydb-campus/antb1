#pragma once

#include <string>

#include "antb1/plan/logical_plan.h"

namespace antb1::plan {

// Indented, deterministic text rendering of a logical plan (used by `antb1 explain` and EXPLAIN
// tests).
std::string Explain(const LogicalPlan& plan);

}  // namespace antb1::plan
