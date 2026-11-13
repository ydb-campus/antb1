#pragma once

#include <cstdint>
#include <optional>

#include <arrow/result.h>

#include "antb1/plan/logical_plan.h"

// The unnesting pass (docs/adr/0023-unnesting.md; ADR 0041's decisions 16 to 20): the dependent
// joins of a bound plan replaced by ordinary joins, outermost first. Private to the plan module:
// the optimizer and its tests include it.

namespace antb1::plan {

// `root` with every dependent join rewritten into ordinary joins, or the refusal of the first one
// the pass does not answer (exit code 4 at the subquery's span). `root` itself when it holds none.
arrow::Result<LogicalNodePtr> Unnest(const LogicalNodePtr& root, int64_t build_side_ratio);

// A column's value when the static aggregation below it had no input row (ADR 0041's decision 19):
// a Constant, whose std::monostate is NULL as branch_truth.h spells it, or std::nullopt where the
// pass does not know it.
using EmptyValue = std::optional<Constant>;

// Decision 19's table, an exhaustive switch over AggKind: 0 for the COUNT kinds and a sum of
// counts, NULL for every other call, both of the call's type.
EmptyValue EmptyValueOf(const AggregateCall& call);

// The outermost dependent join of the plan, nullptr when it has none. Pre-order, so the first one
// found is the one whose span names the whole correlated subquery.
const DependentJoinNode* FirstDependentJoin(const LogicalNodePtr& node);

}  // namespace antb1::plan
