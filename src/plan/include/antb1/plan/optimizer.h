#pragma once

#include "antb1/plan/logical_plan.h"

namespace antb1::plan {

// Rule-based rewrites of a bound plan, applied in this order (docs/architecture.md):
//  1. COUNT(*) as the only aggregate directly over a Scan (no WHERE) of a table with an exact row
//     count becomes a RowCount node: answered from metadata, no data is read.
//  2. Projection pruning: every Scan reads only the fields that the nodes above it reference
//     (possibly none), and the column indices above it are renumbered.
// The result has the same output columns and the same answer as the input plan.
LogicalPlan Optimize(const LogicalPlan& plan);

}  // namespace antb1::plan
