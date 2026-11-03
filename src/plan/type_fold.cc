#include "antb1/plan/type_fold.h"

#include <algorithm>
#include <cstdint>
#include <optional>

#include "antb1/common/narrow.h"
#include "antb1/plan/types.h"

#include "key_type.h"

namespace antb1::plan {
namespace {

// The integer type DuckDB gives two integer types together: unlike arithmetic, USMALLINT with
// SMALLINT is INTEGER.
LogicalType CommonInteger(LogicalType a, LogicalType b) {
  const bool mixed = (a == LogicalType::kUSmallInt && b == LogicalType::kSmallInt) ||
                     (a == LogicalType::kSmallInt && b == LogicalType::kUSmallInt);
  return mixed ? LogicalType::kInteger : CombineIntegers(a, b);
}

// A DECIMAL with a DECIMAL or an integer type (ADR 0021 rule 10).
LogicalType CommonDecimal(LogicalType decimal, LogicalType other) {
  constexpr int kMax = LogicalType::kMaxDecimalWidth;
  const int p = decimal.width();
  const int s = decimal.scale();
  if (other != LogicalType::kDecimal) {
    const int digits = DecimalOfInteger(other).width();
    return digits <= p - s ? decimal
                           : LogicalType::Decimal(Narrow<std::uint8_t>(std::min(kMax, digits + s)),
                                                  Narrow<std::uint8_t>(s));
  }
  const int integers = std::max(p - s, other.width() - other.scale());
  const int scale = std::max<int>(s, other.scale());
  if (integers + scale <= kMax) {
    return LogicalType::Decimal(Narrow<std::uint8_t>(integers + scale),
                                Narrow<std::uint8_t>(scale));
  }
  return LogicalType::Decimal(Narrow<std::uint8_t>(kMax), Narrow<std::uint8_t>(kMax - integers));
}

bool IsTemporal(LogicalType type) {
  return type == LogicalType::kDate || type == LogicalType::kTimestamp;
}

// An integer, a DECIMAL or a DOUBLE.
bool IsNumber(LogicalType type) { return IsNumeric(type) || type == LogicalType::kDecimal; }

}  // namespace

ValueType CommonValueType(ValueType a, ValueType b) {
  if (!a.has_value()) {
    return b;
  }
  if (!b.has_value() || *a == *b) {
    return a;
  }
  const LogicalType x = *a;
  const LogicalType y = *b;
  if (x == LogicalType::kVarchar || y == LogicalType::kVarchar) {
    return LogicalType::kVarchar;
  }
  if (IsTemporal(x) && IsTemporal(y)) {
    return LogicalType::kTimestamp;  // DATE with TIMESTAMP
  }
  if (IsTemporal(x) || IsTemporal(y)) {
    return IsTemporal(x) ? x : y;  // with a number or BOOLEAN
  }
  if (x == LogicalType::kBoolean || y == LogicalType::kBoolean) {
    return x == LogicalType::kBoolean ? y : x;  // with a number
  }
  if (x == LogicalType::kDouble || y == LogicalType::kDouble) {
    return LogicalType::kDouble;
  }
  if (x == LogicalType::kDecimal || y == LogicalType::kDecimal) {
    return x == LogicalType::kDecimal ? CommonDecimal(x, y) : CommonDecimal(y, x);
  }
  return CommonInteger(x, y);
}

bool DuckDbCaseMixes(ValueType a, ValueType b) {
  if (!a.has_value() || !b.has_value() || *a == *b) {
    return true;
  }
  const LogicalType x = *a;
  const LogicalType y = *b;
  if (IsTemporal(x) || IsTemporal(y)) {
    return IsTemporal(x) && IsTemporal(y);
  }
  if (x == LogicalType::kBoolean || y == LogicalType::kBoolean) {
    const LogicalType other = x == LogicalType::kBoolean ? y : x;
    return IsInteger(other);
  }
  return IsNumber(x) && IsNumber(y);
}

}  // namespace antb1::plan
