#include "predicate_domain.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <variant>

#include "antb1/common/narrow.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

namespace antb1::plan {
namespace {

// Unknown and bounds too large to represent both lose to any finite restriction in an AND.
constexpr int64_t kUnknown = std::numeric_limits<int64_t>::max();

struct Bounds {
  std::array<int64_t, kMaxPredicateDomainKeys> per_key{};
  int64_t tuples = 0;
};

int64_t Product(const Bounds& bounds, std::size_t size) {
  int64_t product = 1;
  for (std::size_t i = 0; i < size; ++i) {
    if (__builtin_mul_overflow(product, bounds.per_key[i], &product)) {
      product = kUnknown;
    }
  }
  return product;
}

int64_t Sum(int64_t a, int64_t b) {
  int64_t sum = 0;
  return __builtin_add_overflow(a, b, &sum) ? kUnknown : sum;
}

int64_t AllowedValues(const Predicate& predicate, LogicalType type) {
  if (predicate.kind == Predicate::Kind::kIsNull || predicate.kind == Predicate::Kind::kIsTrue) {
    return 1;
  }
  // Equality must compare in the key's own type. For example, several distinct DECIMAL values
  // can compare equal to the same DOUBLE literal. IN's bound literals all use one comparison type.
  if (predicate.kind == Predicate::Kind::kCompare && predicate.op == CompareOp::kEq &&
      predicate.constant.type == type && type != LogicalType::kDouble) {
    return 1;
  }
  if (predicate.kind == Predicate::Kind::kIn && !predicate.values.empty() &&
      predicate.values.front().type == type && type != LogicalType::kDouble) {
    return TryNarrow<int64_t>(predicate.values.size()).value_or(kUnknown);
  }
  return kUnknown;
}

// The tree can yield two bounds on the same tuple: independent restrictions on its keys, and the
// union of its arms. Keeping both lets AND intersect different keys and OR retain their
// correlation.
class Analysis {
 public:
  Analysis(std::span<const PredicateDomainKey> keys, Bounds all, int& remaining)
      : keys_(keys), all_(all), remaining_(remaining) {}

  std::optional<Bounds> Read(const Expr& expr) {
    if (!Spend()) {
      return std::nullopt;
    }
    if (const auto* boolean = std::get_if<BoolExpr>(&expr.node)) {
      if (boolean->op == BoolOp::kNot || boolean->args.empty()) {
        return all_;
      }
      Bounds result = boolean->op == BoolOp::kAnd ? all_ : Bounds{};
      for (const ExprPtr& arg : boolean->args) {
        const auto child = Read(*arg);
        if (!child.has_value()) {
          return std::nullopt;
        }
        result = Combine(result, *child, boolean->op);
      }
      return result;
    }
    if (const auto* leaf = std::get_if<PredicateExpr>(&expr.node)) {
      if (leaf->predicate.kind == Predicate::Kind::kFalse) {
        return Bounds{};
      }
      if (leaf->operands.size() == 1) {
        const Expr& operand = *leaf->operands.front();
        if (const auto* column = std::get_if<ColumnExpr>(&operand.node)) {
          return Leaf(leaf->predicate, column->id, operand.type);
        }
      }
    }
    return all_;
  }

  std::optional<Bounds> Read(const FilterNode& filter) {
    const auto* compute = std::get_if<ComputeNode>(filter.input.get());
    Bounds result = all_;
    for (const Predicate& predicate : filter.predicates) {
      if (!Spend()) {
        return std::nullopt;
      }
      if (predicate.kind == Predicate::Kind::kFalse) {
        return Bounds{};
      }
      if (!predicate.column.has_value()) {
        continue;
      }
      const ColumnId id = predicate.column->id;
      result = Combine(result, Leaf(predicate, id, predicate.column->type), BoolOp::kAnd);
      if (predicate.kind == Predicate::Kind::kIsTrue && compute != nullptr) {
        for (std::size_t i = 0; i < compute->ids.size(); ++i) {
          if (!Spend()) {
            return std::nullopt;
          }
          if (compute->ids[i] == id) {
            const auto condition = Read(*compute->exprs[i]);
            if (!condition.has_value()) {
              return std::nullopt;
            }
            result = Combine(result, *condition, BoolOp::kAnd);
            break;
          }
        }
      }
    }
    return result;
  }

 private:
  bool Spend() {
    if (remaining_ == 0) {
      return false;
    }
    --remaining_;
    return true;
  }

  Bounds Leaf(const Predicate& predicate, ColumnId id, LogicalType type) const {
    Bounds result = all_;
    for (std::size_t i = 0; i < keys_.size(); ++i) {
      if (keys_[i].id == id) {
        result.per_key[i] = std::min(result.per_key[i], AllowedValues(predicate, type));
      }
    }
    result.tuples = Product(result, keys_.size());
    return result;
  }

  Bounds Combine(const Bounds& a, const Bounds& b, BoolOp op) const {
    // Intersections satisfy both bounds; unions have at most the sum even when their arms overlap.
    const auto combine = [op](int64_t x, int64_t y) {
      return op == BoolOp::kAnd ? std::min(x, y) : Sum(x, y);
    };
    Bounds result;
    for (std::size_t i = 0; i < keys_.size(); ++i) {
      result.per_key[i] = std::min(all_.per_key[i], combine(a.per_key[i], b.per_key[i]));
    }
    result.tuples = std::min(combine(a.tuples, b.tuples), Product(result, keys_.size()));
    return result;
  }

  std::span<const PredicateDomainKey> keys_;
  Bounds all_;
  int& remaining_;
};

}  // namespace

std::optional<int64_t> PredicateTupleBound(const FilterNode& filter,
                                           std::span<const PredicateDomainKey> keys,
                                           int& remaining_nodes) {
  if (keys.empty() || keys.size() > kMaxPredicateDomainKeys || remaining_nodes <= 0) {
    return std::nullopt;
  }
  Bounds all;
  for (std::size_t i = 0; i < keys.size(); ++i) {
    if (keys[i].id == kNoColumnId || keys[i].max_values.value_or(0) < 0) {
      return std::nullopt;
    }
    all.per_key[i] = keys[i].max_values.value_or(kUnknown);
  }
  all.tuples = Product(all, keys.size());
  const int budget = std::min(remaining_nodes, kPredicateDomainNodeBudget);
  int remaining = budget;
  const auto result = Analysis(keys, all, remaining).Read(filter);
  remaining_nodes -= budget - remaining;
  if (result.has_value() && result->tuples < all.tuples) {
    return result->tuples;
  }
  return std::nullopt;
}

}  // namespace antb1::plan
