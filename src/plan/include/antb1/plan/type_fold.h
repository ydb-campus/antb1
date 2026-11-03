#pragma once

#include <optional>

#include "antb1/plan/types.h"

// DuckDB's type of the values that one column of a set operation, or one CASE, combines (ADR 0037's
// NL1): one fold for both, with the NULL type as its identity, so that a NULL value takes the type
// of the others in either context. The contexts differ in what they accept, not in the type: a
// UNION ALL takes only exact widenings below it (ADR 0036), and DuckDB's CASE refuses some of the
// mixes a union types (DuckDbCaseMixes).

namespace antb1::plan {

// A value's type in the fold: a LogicalType, or the NULL type of a NULL value (std::nullopt).
using ValueType = std::optional<LogicalType>;

// The type DuckDB gives two values together, as its UNION ALL types a column; commutative,
// but not associative, so a column folds left in written order, as in DuckDB. NULL with a type is
// that type, and NULL with NULL is NULL. Of two types:
//   - integers: their common integer type, where USMALLINT with SMALLINT is INTEGER;
//   - a DECIMAL with a DECIMAL or an integer: DECIMAL(e + s, s), e the larger number of integer
//     digits and s the larger scale (an integer counts its type's digits: SMALLINT 5, INTEGER 10,
//     BIGINT 19, HUGEINT 38), or DECIMAL(38, 38 - e) beyond 38 digits (ADR 0021 rule 10);
//   - DOUBLE with a number: DOUBLE;
//   - BOOLEAN with a number: the number;
//   - DATE with TIMESTAMP: TIMESTAMP; either with a number or BOOLEAN: the DATE or TIMESTAMP (whose
//     cast then fails in DuckDB);
//   - VARCHAR with any type: VARCHAR.
ValueType CommonValueType(ValueType a, ValueType b);

// Whether DuckDB's CASE takes values of `a` and `b` together, at CommonValueType's type: every pair
// of numbers but BOOLEAN with a DECIMAL or DOUBLE, DATE with TIMESTAMP, and any type with itself or
// with NULL. It refuses a VARCHAR next to another type, BOOLEAN next to a DECIMAL or DOUBLE, and a
// DATE or TIMESTAMP next to a number or BOOLEAN ("Cannot mix values of type ...").
bool DuckDbCaseMixes(ValueType a, ValueType b);

}  // namespace antb1::plan
