#pragma once

#include "antb1/plan/logical_plan.h"

namespace antb1::plan {

bool CanSkipPredicate(const Predicate& predicate);

// A value that can be computed before matching a scalar aggregate's correlation keys.
bool CanPrecomputeScalarKey(const Expr& expr, const LogicalNodePtr& input);

// Whether an expression can run on an unmatched row without raising a value error.
bool CanEvaluateOnUnmatchedRow(const Expr& expr, const LogicalNodePtr& input);

// Also accepts an existing projection's integer cast when the input range fits the target.
bool CanEvaluateUnboundProjection(const Expr& expr, const LogicalNodePtr& input);

// Whether forming a group can raise a value error, even with a plain column argument.
bool CanFailOnGroup(const AggregateCall& call);

}  // namespace antb1::plan
