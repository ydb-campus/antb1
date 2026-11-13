#pragma once

#include <cstdint>

#include <arrow/result.h>

#include "antb1/plan/logical_plan.h"

// The unnesting as main had it before ADR 0041's SQ0, for unnesting_test's shadow test only.

namespace antb1::plan::legacy {

arrow::Result<LogicalNodePtr> Unnest(const LogicalNodePtr& root, int64_t build_side_ratio);
const DependentJoinNode* FirstDependentJoin(const LogicalNodePtr& node);

}  // namespace antb1::plan::legacy
