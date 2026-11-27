#pragma once

#include <optional>

#include "antb1/plan/logical_plan.h"

namespace antb1::plan {

// Factor repeated dimension lookups out of an inner join tree in a resolved plan. Each
// branch must already have a fact-side partial with at most one group per surrogate. Extra
// grouping keys must be fixed by predicates before grouping, so orphan keys cannot multiply
// rows before the lookup. The business key and surrogate must each identify a single non-NULL row.
// The final lookup join enforces that property on its actual build. A blocking sort with
// all observable tie breakers precedes output.
std::optional<LogicalPlan> FindDeferredDimensionJoin(const LogicalPlan& plan);

}  // namespace antb1::plan
