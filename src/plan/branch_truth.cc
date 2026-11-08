#include "antb1/plan/branch_truth.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "antb1/common/int128.h"
#include "antb1/plan/types.h"

namespace antb1::plan {
namespace {

// A NULL constant (ADR 0037's NL3) is never true of a comparison and never IS NOT NULL (TruthOver):
// a new kind of constant is decided here first.
static_assert(
    std::variant_size_v<decltype(Constant::value)> == 4,
    "a new kind of Constant is decided in TruthOver before it reaches a UNION ALL branch");

bool IsNull(const std::optional<Constant>& value) {
  return value.has_value() && std::holds_alternative<std::monostate>(value->value);
}

// The domain in which two constants compare exactly, or none: DOUBLE (NaN, -0.0, DuckDB's own
// conversions) and BOOLEAN (no constant is one) are never decided here.
enum class Domain : std::uint8_t { kNumber, kDate, kTimestamp, kText };

std::optional<Domain> DomainOf(LogicalType type) {
  switch (type.id()) {
    case LogicalType::kSmallInt:
    case LogicalType::kInteger:
    case LogicalType::kBigInt:
    case LogicalType::kUSmallInt:
    case LogicalType::kHugeInt:
    case LogicalType::kDecimal:
      return Domain::kNumber;
    case LogicalType::kDate:
      return Domain::kDate;
    case LogicalType::kTimestamp:
      return Domain::kTimestamp;
    case LogicalType::kVarchar:
      return Domain::kText;
    case LogicalType::kDouble:
    case LogicalType::kBoolean:
      return std::nullopt;
  }
  return std::nullopt;
}

int ScaleOf(LogicalType type) { return type == LogicalType::kDecimal ? int{type.scale()} : 0; }

int Sign(int compared) {
  if (compared < 0) {
    return -1;
  }
  return compared > 0 ? 1 : 0;
}

// a <=> b as -1, 0 or 1, or none where the two do not compare exactly here: numbers by value at
// any scale (CompareScaled, as exec compares a DECIMAL with an integer), dates and timestamps by
// their integers, text by unsigned bytes (std::string_view::compare, as Arrow's binary kernels and
// DuckDB compare).
std::optional<int> Order(const Constant& a, const Constant& b) {
  const std::optional<Domain> domain = DomainOf(a.type);
  if (!domain.has_value() || DomainOf(b.type) != domain) {
    return std::nullopt;
  }
  return std::visit(
      [&](const auto& x) -> std::optional<int> {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, Int128>) {
          const auto* y = std::get_if<Int128>(&b.value);
          if (y == nullptr || *domain == Domain::kText) {
            return std::nullopt;
          }
          if (*domain == Domain::kNumber) {
            return CompareScaled(x, ScaleOf(a.type), *y, ScaleOf(b.type));
          }
          if (x < *y) {
            return -1;
          }
          return x > *y ? 1 : 0;
        } else if constexpr (std::is_same_v<T, double> || std::is_same_v<T, std::monostate>) {
          return std::nullopt;
        } else if constexpr (std::is_same_v<T, std::string>) {
          const auto* y = std::get_if<std::string>(&b.value);
          if (y == nullptr || *domain != Domain::kText) {
            return std::nullopt;
          }
          return Sign(std::string_view(x).compare(*y));
        } else {
          static_assert(false, "a kind of constant Order does not compare");
        }
      },
      a.value);
}

BranchTruth Truth(bool holds) { return holds ? BranchTruth::kAlways : BranchTruth::kNever; }

BranchTruth Truth(CompareOp op, int order) {
  switch (op) {
    case CompareOp::kEq:
      return Truth(order == 0);
    case CompareOp::kNe:
      return Truth(order != 0);
    case CompareOp::kLt:
      return Truth(order < 0);
    case CompareOp::kLe:
      return Truth(order <= 0);
    case CompareOp::kGt:
      return Truth(order > 0);
    case CompareOp::kGe:
      return Truth(order >= 0);
  }
  return BranchTruth::kUnknown;
}

// `value` where it is a constant of `column`'s own type, as exec requires of a comparison's
// constant (exec/filter.cc); nullptr otherwise.
const Constant* OfItsType(const std::optional<BoundColumn>& column,
                          const std::optional<Constant>& value) {
  if (!column.has_value() || !value.has_value() || value->type != column->type) {
    return nullptr;
  }
  return &*value;
}

// value IN (values), for a value of the column's type: always where one of them equals it, never
// where every one differs, and unknown where one is of another type (exec rejects such a list) or
// does not compare exactly. Neither side is NULL, so IN is two-valued here.
BranchTruth InList(const std::vector<Constant>& values, const Constant& value) {
  if (values.empty()) {
    return BranchTruth::kUnknown;
  }
  bool equal = false;
  for (const Constant& candidate : values) {
    const std::optional<int> order =
        candidate.type == value.type ? Order(value, candidate) : std::nullopt;
    if (!order.has_value()) {
      return BranchTruth::kUnknown;
    }
    equal = equal || *order == 0;
  }
  return equal ? BranchTruth::kAlways : BranchTruth::kNever;
}

BranchTruth Negated(BranchTruth truth) {
  switch (truth) {
    case BranchTruth::kNever:
      return BranchTruth::kAlways;
    case BranchTruth::kAlways:
      return BranchTruth::kNever;
    case BranchTruth::kUnknown:
      return BranchTruth::kUnknown;
  }
  return BranchTruth::kUnknown;
}

}  // namespace

std::string_view ToString(BranchTruth truth) {
  switch (truth) {
    case BranchTruth::kNever:
      return "never";
    case BranchTruth::kAlways:
      return "always";
    case BranchTruth::kUnknown:
      return "unknown";
  }
  return "unknown";
}

BranchTruth TruthOver(const Predicate& predicate, const std::optional<Constant>& column_value,
                      const std::optional<Constant>& other_value) {
  // A NULL value makes no predicate but IS NULL and the DISTINCT FROM pair true: a comparison, IN
  // and NOT IN, LIKE and IS TRUE are NULL, and IS NOT NULL false. FALSE reads no column.
  if (predicate.kind == Predicate::Kind::kIsDistinct ||
      predicate.kind == Predicate::Kind::kIsNotDistinct) {
    // NULL is not distinct from NULL and distinct from every value; never NULL (ADR 0037's OJ5b).
    const bool not_distinct = predicate.kind == Predicate::Kind::kIsNotDistinct;
    const auto decided = [not_distinct](bool equal) {
      return equal == not_distinct ? BranchTruth::kAlways : BranchTruth::kNever;
    };
    const bool null = IsNull(column_value);
    const bool other_null = IsNull(other_value);
    const Constant* value = OfItsType(predicate.column, column_value);
    const Constant* other = OfItsType(predicate.other, other_value);
    if (null && other_null) {
      return decided(true);
    }
    if ((null && other != nullptr) || (other_null && value != nullptr)) {
      return decided(false);
    }
    if (value == nullptr || other == nullptr) {
      return BranchTruth::kUnknown;
    }
    const std::optional<int> order = Order(*value, *other);
    return order.has_value() ? decided(*order == 0) : BranchTruth::kUnknown;
  }
  if (predicate.kind == Predicate::Kind::kIsNull) {
    if (IsNull(column_value)) {
      return BranchTruth::kAlways;
    }
    return OfItsType(predicate.column, column_value) != nullptr ? BranchTruth::kNever
                                                                : BranchTruth::kUnknown;
  }
  if (predicate.kind != Predicate::Kind::kFalse &&
      ((predicate.column.has_value() && IsNull(column_value)) ||
       (predicate.other.has_value() && IsNull(other_value)))) {
    return BranchTruth::kNever;
  }
  switch (predicate.kind) {
    case Predicate::Kind::kCompare: {
      const Constant* value = OfItsType(predicate.column, column_value);
      if (value == nullptr || predicate.constant.type != value->type) {
        return BranchTruth::kUnknown;
      }
      const std::optional<int> order = Order(*value, predicate.constant);
      return order.has_value() ? Truth(predicate.op, *order) : BranchTruth::kUnknown;
    }
    case Predicate::Kind::kCompareColumns: {
      const Constant* value = OfItsType(predicate.column, column_value);
      const Constant* other = OfItsType(predicate.other, other_value);
      if (value == nullptr || other == nullptr) {
        return BranchTruth::kUnknown;
      }
      const std::optional<int> order = Order(*value, *other);
      return order.has_value() ? Truth(predicate.op, *order) : BranchTruth::kUnknown;
    }
    case Predicate::Kind::kIn:
    case Predicate::Kind::kNotIn: {
      const Constant* value = OfItsType(predicate.column, column_value);
      if (value == nullptr) {
        return BranchTruth::kUnknown;
      }
      const BranchTruth in = InList(predicate.values, *value);
      return predicate.kind == Predicate::Kind::kIn ? in : Negated(in);
    }
    case Predicate::Kind::kIsNotNull:
      return OfItsType(predicate.column, column_value) != nullptr ? BranchTruth::kAlways
                                                                  : BranchTruth::kUnknown;
    case Predicate::Kind::kFalse:
      return BranchTruth::kNever;
    case Predicate::Kind::kIsNull:  // decided above, as are the DISTINCT FROM pair
    case Predicate::Kind::kIsDistinct:
    case Predicate::Kind::kIsNotDistinct:
    case Predicate::Kind::kLike:
    case Predicate::Kind::kNotLike:
    case Predicate::Kind::kIsTrue:
      return BranchTruth::kUnknown;
  }
  return BranchTruth::kUnknown;
}

}  // namespace antb1::plan
