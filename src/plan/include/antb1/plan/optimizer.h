#pragma once

#include "antb1/plan/logical_plan.h"

namespace antb1::plan {

// Rule-based rewrites of a bound plan, applied in this order (docs/architecture.md):
//  1. COUNT(*) as the only aggregate directly over a Scan (no WHERE) of a table with an exact row
//     count becomes a RowCount node: answered from metadata, no data is read.
//  2. A GROUP BY key computed only from other keys (not DOUBLE ones) is dropped from the
//     GroupAggregate and computed from them once per group, above it
//     (docs/adr/0018-dependent-group-keys.md): the same groups, fewer keys to hash.
//  3. Limit below Project and Compute, so that it sits right above a Sort (top-N).
//  4. Projection pruning: every Scan reads only the fields that the nodes above it reference
//     (possibly none), Computes keep only the expressions used, and the column indices above them
//     are renumbered.
// The result has the same output columns and the same answer as the input plan.
LogicalPlan Optimize(const LogicalPlan& plan);

}  // namespace antb1::plan
