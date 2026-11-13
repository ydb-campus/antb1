#pragma once

#include "antb1/plan/logical_plan.h"

// Which expressions keep a NULL operand's NULL. Private to the plan module: the unnesting (ADR
// 0023's value mode, ADR 0041's empty values) and ADR 0032's aggregate placement include it.

namespace antb1::plan {

// Whether an expression is STRICT: NULL whenever an operand is NULL. Columns, literals, arithmetic,
// negation, casts and every function but concat are; CASE, COALESCE, OR, AND and predicates are not
// in general, since they can turn a NULL into a value.
[[nodiscard]] bool IsStrict(const Expr& expr);

// Whether an expression is NULL whenever every column it reads is NULL: strict, and reading at
// least one column. A literal is strict yet keeps its value where every column is NULL, so it does
// not propagate a NULL.
[[nodiscard]] bool PropagatesNull(const Expr& expr);

}  // namespace antb1::plan
