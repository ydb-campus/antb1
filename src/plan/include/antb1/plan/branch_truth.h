#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "antb1/plan/logical_plan.h"

namespace antb1::plan {

// What a predicate is over rows that all hold the same constants in the columns it reads: true for
// every such row, for none, or not decided here. The optimizer prunes a UNION ALL branch whose
// constants make a condition above the union never true, and drops a condition they make always
// true (ADR 0036's X7a). Whatever is not decided exactly stays a condition, so only an exact answer
// changes a plan.
enum class BranchTruth : std::uint8_t { kNever, kAlways, kUnknown };

// "never", "always", "unknown".
std::string_view ToString(BranchTruth truth);

// What `predicate` is over rows whose `column` holds `column_value` and whose `other` holds
// `other_value` (std::nullopt: not one constant). Decided only where it is exact and the same as
// exec's evaluation (exec/filter.cc):
// - a comparison or an IN list only when its constants have the column's own type, and each value
//   has its column's type;
// - numbers (the integer types and DECIMAL, at any scale) exactly by value, DATE and TIMESTAMP by
//   their integers, VARCHAR by unsigned bytes;
// - IS NOT NULL is always true of a constant that is not NULL;
// - a NULL value (ADR 0037's NL3) makes every predicate that reads it never true but IS NULL,
//   which it makes always true; IS NULL is never true of a constant that is not NULL.
// Unknown for DOUBLE, LIKE, a computed condition and anything else: an answer is never kNever or
// kAlways by default.
BranchTruth TruthOver(const Predicate& predicate, const std::optional<Constant>& column_value,
                      const std::optional<Constant>& other_value);

}  // namespace antb1::plan
