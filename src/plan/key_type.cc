#include "key_type.h"

#include <algorithm>
#include <format>
#include <memory>
#include <optional>

#include "antb1/common/narrow.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

namespace antb1::plan {
namespace {

int SignedRank(LogicalType type) {
  switch (type.id()) {
    case LogicalType::kSmallInt:
      return 1;
    case LogicalType::kInteger:
      return 2;
    case LogicalType::kBigInt:
      return 3;
    case LogicalType::kHugeInt:
      return 4;
    default:
      return 0;
  }
}

}  // namespace

// other's type): the wider signed type, where USMALLINT with SMALLINT is BIGINT and with a wider
// type that type.
LogicalType CombineIntegers(LogicalType a, LogicalType b) {
  if (a == b) {
    return a;
  }
  if (a == LogicalType::kUSmallInt || b == LogicalType::kUSmallInt) {
    const LogicalType other = a == LogicalType::kUSmallInt ? b : a;
    return other == LogicalType::kSmallInt ? LogicalType::kBigInt : other;
  }
  return SignedRank(a) >= SignedRank(b) ? a : b;
}

// An integer type as the DECIMAL DuckDB computes it in next to a DECIMAL (ADR 0021 rule 4): by its
// type, so a literal does not shrink to fit (7 is INTEGER, DECIMAL(10,0)).
LogicalType DecimalOfInteger(LogicalType type) {
  switch (type.id()) {
    case LogicalType::kSmallInt:
    case LogicalType::kUSmallInt:
      return LogicalType::Decimal(5, 0);
    case LogicalType::kInteger:
      return LogicalType::Decimal(10, 0);
    case LogicalType::kBigInt:
      return LogicalType::Decimal(19, 0);
    default:
      break;
  }
  return LogicalType::Decimal(LogicalType::kMaxDecimalWidth, 0);  // HUGEINT
}

std::optional<LogicalType> KeyType(LogicalType a, LogicalType b) {
  if (a == LogicalType::kDouble || b == LogicalType::kDouble || a == LogicalType::kBoolean ||
      b == LogicalType::kBoolean) {
    return std::nullopt;
  }
  if (a == b) {
    return a;
  }
  if (IsInteger(a) && IsInteger(b)) {
    return CombineIntegers(a, b);
  }
  const bool a_decimal = a == LogicalType::kDecimal;
  const bool b_decimal = b == LogicalType::kDecimal;
  if (!(a_decimal || IsInteger(a)) || !(b_decimal || IsInteger(b))) {
    return std::nullopt;  // a VARCHAR with a DATE, say: no type holds both their values
  }
  const LogicalType wide_a = a_decimal ? a : DecimalOfInteger(a);
  const LogicalType wide_b = b_decimal ? b : DecimalOfInteger(b);
  const int scale = std::max(wide_a.scale(), wide_b.scale());
  const int digits = std::max(wide_a.width() - wide_a.scale(), wide_b.width() - wide_b.scale());
  if (digits + scale > LogicalType::kMaxDecimalWidth) {
    return std::nullopt;  // BIGINT against DECIMAL(38,30) needs 49 digits
  }
  return LogicalType::Decimal(Narrow<std::uint8_t>(digits + scale), Narrow<std::uint8_t>(scale));
}

ExprPtr WidenTo(const ExprPtr& expr, LogicalType type) {
  if (expr->type == type) {
    return expr;
  }
  return std::make_shared<const Expr>(
      Expr{.node = CastExpr{.operand = expr},
           .type = type,
           .name = std::format("CAST({} AS {})", expr->name, ToString(type))});
}

}  // namespace antb1::plan
