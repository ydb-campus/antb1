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

// Days since 1970-01-01 of a date written exactly as YYYY-MM-DD (proleptic Gregorian, years 0000 to
// 9999); std::nullopt for any other text or an invalid date.
std::optional<int32_t> ParseDate(std::string_view text);

// YYYY-MM-DD of a day number.
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
