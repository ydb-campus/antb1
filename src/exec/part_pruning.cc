#include "part_pruning.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

#include "antb1/common/int128.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/table.h"
#include "antb1/plan/types.h"

namespace antb1::exec {
namespace {

// A constant of an integer-valued column (integers, USMALLINT, DATE) as a number.
std::optional<Int128> IntegerOf(const plan::Constant& constant) {
  const plan::LogicalType type = constant.type;
  if (!plan::IsInteger(type) && type != plan::LogicalType::kDate) {
    return std::nullopt;
  }
  if (const auto* value = std::get_if<Int128>(&constant.value)) {
    return *value;
  }
  return std::nullopt;
}

// Whether `predicate` is false for every row of a part with these statistics of its column.
bool NeverTrue(const plan::Predicate& predicate, const plan::PartStats& stats) {
  using Kind = plan::Predicate::Kind;
  if (stats.rows <= 0) {
    return false;
  }
  const bool all_null = stats.null_count >= stats.rows;
  switch (predicate.kind) {
    case Kind::kCompare: {
      if (all_null) {
        return true;  // a comparison with NULL is never true
      }
      const auto c = IntegerOf(predicate.constant);
      if (!c.has_value() || !stats.min.has_value() || !stats.max.has_value()) {
        return false;
      }
      const Int128 lo = *stats.min;
      const Int128 hi = *stats.max;
      switch (predicate.op) {
        case plan::CompareOp::kEq:
          return *c < lo || *c > hi;
        case plan::CompareOp::kNe:
          return lo == *c && hi == *c;  // every non-NULL value is c; NULL fails too
        case plan::CompareOp::kLt:
          return lo >= *c;
        case plan::CompareOp::kLe:
          return lo > *c;
        case plan::CompareOp::kGt:
          return hi <= *c;
        case plan::CompareOp::kGe:
          return hi < *c;
      }
      return false;
    }
    case Kind::kIn: {
      if (all_null) {
        return true;
      }
      if (!stats.min.has_value() || !stats.max.has_value()) {
        return false;
      }
      // Every value outside [min, max]; a value that may match (or is not a number) keeps it.
      return std::ranges::all_of(predicate.values, [&](const plan::Constant& value) {
        const auto v = IntegerOf(value);
        return v.has_value() && (*v < *stats.min || *v > *stats.max);
      });
    }
    case Kind::kIsNotNull:
      return all_null;
    default:
      return false;  // NOT IN, LIKE, columns, conditions: never skip
  }
}

}  // namespace

bool PartMayMatch(const plan::Table& table, int64_t part, const std::vector<int>& fields,
                  const std::vector<plan::Predicate>& predicates) {
  return std::ranges::none_of(predicates, [&](const plan::Predicate& predicate) {
    if (predicate.kind == plan::Predicate::Kind::kFalse) {
      return true;
    }
    if (!predicate.column.has_value() || predicate.column->index < 0 ||
        static_cast<std::size_t>(predicate.column->index) >= fields.size()) {
      return false;
    }
    // The predicate's column is a column of the scan's output: its table field is fields[index].
    const auto stats =
        table.part_stats(part, fields[static_cast<std::size_t>(predicate.column->index)]);
    return stats.has_value() && NeverTrue(predicate, *stats);
  });
}

std::vector<int64_t> KeptParts(const plan::Table& table, const std::vector<int>& fields,
                               const std::vector<plan::Predicate>& predicates) {
  std::vector<int64_t> kept;
  const int64_t parts = table.num_parts();
  kept.reserve(static_cast<std::size_t>(parts));
  for (int64_t part = 0; part < parts; ++part) {
    if (predicates.empty() || PartMayMatch(table, part, fields, predicates)) {
      kept.push_back(part);
    }
  }
  return kept;
}

}  // namespace antb1::exec
