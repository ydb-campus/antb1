#pragma once

#include <arrow/result.h>

#include "antb1/plan/logical_plan.h"
#include "antb1/plan/plan_options.h"

namespace antb1::plan {

// Rule-based rewrites of a bound plan, applied in this order (docs/architecture.md). The unnesting
// pass runs first and is the only one that can fail:
//  0. Unnesting (ADR 0023): every DependentJoin becomes ordinary joins, or the query is rejected
//     with kUnsupported (exit code 4) at the subquery's span. Roadmap id U1 has no rewrite yet, so
//     every correlated subquery is rejected here. A dependent join left in the plan afterwards is a
//     bug in the pass, and fails with no SqlErrorDetail (exit code 70), so that it can never pass
//     for a refusal (ADR 0005).
//  1. COUNT(*) as the only aggregate directly over a Scan (no WHERE) of a table with an exact row
//     count becomes a RowCount node: answered from metadata, no data is read.
//  2. A GROUP BY key computed only from other keys (not DOUBLE ones) is dropped from the
//     GroupAggregate and computed from them once per group, above it
//     (docs/adr/0018-dependent-group-keys.md): the same groups, fewer keys to hash.
//  3. Limit below Project and Compute, so that it sits right above a Sort (top-N).
//  4. Projection pruning: every Scan reads only the fields that the nodes above it reference
//     (possibly none), and Computes keep only the expressions used.
// Rules 1 to 4 find columns by their ids and ignore the input's indices; last,
// plan::ResolvePositions sets every index from the ids (ADR 0022). A plan it returns has the same
// output columns (and column ids) and the same answer as the input plan, and holds no dependent
// join, so no later pass and no operator meets one.
//
// The unnesting pass is the one rule that changes a NODE's output columns: Path 2's inner join
// exposes the group's key columns that the dependent join did not. The PLAN's output is unchanged
// all the same, since the binder always puts a Project or an aggregation above a subquery's join;
// a hand-built plan whose root is the join itself would see the wider output.
arrow::Result<LogicalPlan> Optimize(const LogicalPlan& plan, const BindOptions& options = {});

}  // namespace antb1::plan
