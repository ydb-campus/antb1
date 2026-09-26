#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "antb1/common/int128.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

// Exact literal handling for the binder (docs/sql-subset.md, "Literals"): numbers are never
// compared through a lossy cast, so a comparison with an integer column is folded exactly.

namespace antb1::plan {

// The exact value of a numeric literal, as far as comparisons with integer columns need it.
struct ExactNumber {
  bool negative = false;
  bool huge = false;      // |value| >= 10^38: outside every integer type (HUGEINT included)
  Int128 magnitude = 0;   // floor(|value|) when !huge
  bool fraction = false;  // |value| is not an integer
};

// Parses the text of an integer or decimal token without its sign ("42", "1.5", ".5", "5.",
// "1e3", "2.5E-3"); std::nullopt if the text is not such a number.
std::optional<ExactNumber> ParseExactNumber(std::string_view text, bool negative);

// The double nearest to a numeric literal (correctly rounded); like DuckDB, a value beyond the
// double range is +-inf and one below the smallest subnormal is +-0. std::nullopt if the text is
// not a number.
std::optional<double> ParseDoubleLiteral(std::string_view text, bool negative);

// Whether DuckDB reads the text of a numeric literal (without its sign) as a DOUBLE rather than as
// an exact integer or DECIMAL: it has an exponent ("1e3"), or it is a decimal with more than 38
// digits, DECIMAL's maximum width (leading zeros count, as in DuckDB).
bool IsApproximateNumber(std::string_view text);

// The FLOAT that DuckDB (1.5.5) turns a numeric literal into when it compares the literal with a
// FLOAT column, or std::nullopt when DuckDB types the literal as DOUBLE (compared in DOUBLE, as
// antb1 does for every DOUBLE column). DuckDB types an integer by value (INTEGER, BIGINT, HUGEINT
// or UHUGEINT; outside -2^127 to 2^128 - 1 DOUBLE) and a decimal as DECIMAL(digits, fraction
// digits), and it casts with its own arithmetic: BIGINT exactly rounded, HUGEINT and UHUGEINT
// through a double, a DECIMAL as unscaled / 10^scale in float or, when the unscaled value is
// beyond 2^24, as div + mod / 10^scale. The result can be +-inf and is not always the FLOAT
// nearest to the literal (docs/sql-subset.md, "Binding"). `text` is written without its sign.
std::optional<float> DuckDbFloatOf(std::string_view text, bool negative);

// The exact value of a double: +-inf and magnitudes of 10^38 or more are huge; NaN is zero.
ExactNumber ExactNumberOf(double value);

// Days since 1970-01-01 of a date written exactly as YYYY-MM-DD (proleptic Gregorian, years 0000 to
// 9999); std::nullopt for any other text or an invalid date.
std::optional<int32_t> ParseDate(std::string_view text);

// A day number as DuckDB prints a DATE, for every int32: YYYY-MM-DD (years past 9999 with more
// digits), YYYY-MM-DD (BC) before year 1 (year 0 is 1 BC), and DuckDB's sentinels INT32_MAX and
// -INT32_MAX as infinity and -infinity.
std::string FormatDate(int32_t days);

// Inclusive value range of an integer logical type; HUGEINT is decimal128(38, 0), so +-(10^38 - 1).
struct IntegerRange {
  Int128 min = 0;
  Int128 max = 0;
};
IntegerRange RangeOf(LogicalType integer_type);

// `column <op> literal` for an integer column, folded exactly: a non-integer literal becomes an
// integer bound (c > 1.5 -> c >= 2, c <= -1.5 -> c <= -2, c = 1.5 -> never true), and a bound
// outside the column's range makes the comparison true for every non-NULL value (kIsNotNull) or
// for no row (kFalse). Otherwise kind is kCompare and value lies inside the range.
struct FoldedComparison {
  Predicate::Kind kind = Predicate::Kind::kCompare;
  CompareOp op = CompareOp::kEq;
  Int128 value = 0;
};
FoldedComparison FoldIntegerComparison(CompareOp op, const ExactNumber& literal,
                                       IntegerRange range);

}  // namespace antb1::plan
