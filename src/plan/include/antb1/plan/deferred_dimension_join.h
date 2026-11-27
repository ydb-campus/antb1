#pragma once

#include <optional>

#include "antb1/plan/logical_plan.h"

namespace antb1::plan {

// Factor repeated dimension lookups out of an inner join tree in a resolved plan. Each
// branch must already have a fact-side partial whose groups identify the final groups
// when the business key and the surrogate each identify a single non-NULL dimension row.
// The final lookup join enforces that property on its actual build. A blocking sort with
// all observable tie breakers precedes output.
std::optional<LogicalPlan> FindDeferredDimensionJoin(const LogicalPlan& plan);

}  // namespace antb1::plan
