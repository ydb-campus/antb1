#pragma once

#include <optional>

#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

// Join keys: the one type both sides of a cross-relation equality compare in, and the cast that
// takes a side to it. Private to the plan module. The binder builds a key when it writes a join
// (ADR 0022), and ADR 0023's unnesting pass builds one when it pulls a correlated equality into a
// semi or anti join; both must spell it the same way, since a CLI golden pins the cast's rendering
// and exec compares a key's declared type against the column's actual Arrow type.

namespace antb1::plan {

// The wider signed type of two integer types, where USMALLINT with SMALLINT is BIGINT and with a
// wider type that type (ADR 0021).
LogicalType CombineIntegers(LogicalType a, LogicalType b);

// An integer type as the DECIMAL DuckDB computes it in next to a DECIMAL (ADR 0021 rule 4): by its
// type, so a literal does not shrink to fit (7 is INTEGER, DECIMAL(10,0)).
LogicalType DecimalOfInteger(LogicalType type);

// The one type both sides of a cross-relation equality can be compared in as a join key, where an
// exact widening reaches it from each (ADR 0022's join keys); std::nullopt otherwise, and the
// equality then stays a residual. DOUBLE never has one, by decision: its comparison is not exact.
// BOOLEAN is defensive, mirroring exec's own Invalid: no condition is a comparison operand.
std::optional<LogicalType> KeyType(LogicalType a, LogicalType b);

// `expr` cast to `type`, or `expr` itself when it already has that type.
ExprPtr WidenTo(const ExprPtr& expr, LogicalType type);

}  // namespace antb1::plan
