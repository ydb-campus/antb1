#include "union_branches.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "antb1/common/int128.h"
#include "antb1/plan/literal.h"
#include "antb1/plan/types.h"

namespace antb1::plan {
namespace {

LogicalNodePtr Make(LogicalNode node) {
  return std::make_shared<const LogicalNode>(std::move(node));
}

// The node over its inputs rewritten by `f` (every input, in order); the node itself when no
// input changes.
template <class F>
LogicalNodePtr MapInputs(const LogicalNodePtr& node, const F& f) {
  std::vector<LogicalNodePtr> inputs = InputsOf(*node);
  bool changed = false;
  for (LogicalNodePtr& input : inputs) {
    if (input == nullptr) {
      continue;
    }
    LogicalNodePtr rewritten = f(input);
    if (rewritten != input) {
      input = std::move(rewritten);
      changed = true;
    }
  }
  return changed ? WithInputs(*node, std::move(inputs)) : node;
}

std::optional<std::size_t> PositionOf(const std::vector<ColumnId>& ids, ColumnId id) {
  const auto at = std::ranges::find(ids, id);
  if (at == ids.end()) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(at - ids.begin());
}

// `constant` as the binder's exact widening cast to `to` makes it (IsExactWidening): an integer
// keeps its value in a wider integer, and an integer or a DECIMAL becomes a DECIMAL of a scale at
// least its own by multiplying up. std::nullopt for anything else, and for a value the result
// cannot hold, which an exact widening never meets.
std::optional<Constant> Widen(const Constant& constant, LogicalType to) {
  if (constant.type == to) {
    return constant;
  }
  const auto* value = std::get_if<Int128>(&constant.value);
  if (value == nullptr || !IsExactWidening(constant.type, to)) {
    return std::nullopt;
  }
  if (IsInteger(to)) {
    if (!IsInteger(constant.type)) {
      return std::nullopt;
    }
    const IntegerRange range = RangeOf(to);
    if (*value < range.min || *value > range.max) {
      return std::nullopt;
    }
    return Constant{.type = to, .value = *value};
  }
  if (to != LogicalType::kDecimal ||
      (!IsInteger(constant.type) && constant.type != LogicalType::kDecimal)) {
    return std::nullopt;
  }
  const int from_scale = constant.type == LogicalType::kDecimal ? int{constant.type.scale()} : 0;
  if (from_scale > int{to.scale()}) {
    return std::nullopt;  // never down: Rescale would round
  }
  const std::optional<Int128> scaled = Rescale(*value, from_scale, to.scale());
  const Int128 limit = PowerOfTen(to.width());
  if (!scaled.has_value() || *scaled >= limit || *scaled <= -limit) {
    return std::nullopt;
  }
  return Constant{.type = to, .value = *scaled};
}

// `predicate` with the union's columns it reads replaced by input `branch`'s: the id, name and
// qualifier of UnionAllNode::columns, whose type is the union's, which the predicate keeps.
// std::nullopt for a column the union does not output.
std::optional<Predicate> InBranch(const UnionAllNode& node, std::size_t branch,
                                  const Predicate& predicate) {
  Predicate out = predicate;
  for (std::optional<BoundColumn>* side : {&out.column, &out.other}) {
    if (!side->has_value()) {
      continue;
    }
    const std::optional<std::size_t> at = PositionOf(node.ids, (*side)->id);
    if (!at.has_value() || *at >= node.columns[branch].size()) {
      return std::nullopt;
    }
    const BoundColumn& source = node.columns[branch][*at];
    (*side)->id = source.id;
    (*side)->name = source.name;
    (*side)->qualifier = source.qualifier;
  }
  return out;
}

// Whether the pass may move `filter`'s predicates into `node`'s branches: every predicate reads
// columns the union outputs, and none is a FALSE (which already reads nothing above the union, and
// which the pass's own output for a union whose every branch is ruled out holds).
bool Movable(const FilterNode& filter, const UnionAllNode& node) {
  if (filter.predicates.empty() || node.inputs.empty() ||
      node.columns.size() != node.inputs.size()) {
    return false;
  }
  return std::ranges::all_of(filter.predicates, [&node](const Predicate& predicate) {
    if (predicate.kind == Predicate::Kind::kFalse || !predicate.column.has_value()) {
      return false;
    }
    return PositionOf(node.ids, predicate.column->id).has_value() &&
           (!predicate.other.has_value() || PositionOf(node.ids, predicate.other->id).has_value());
  });
}

LogicalNodePtr IntoBranches(const FilterNode& filter, const UnionAllNode& node) {
  std::vector<LogicalNodePtr> inputs;
  std::vector<std::vector<BoundColumn>> columns;
  for (std::size_t k = 0; k < node.inputs.size(); ++k) {
    std::vector<Predicate> kept;
    bool ruled_out = false;
    for (const Predicate& predicate : filter.predicates) {
      const BranchTruth truth = TruthInBranch(node, k, predicate);
      if (truth == BranchTruth::kNever) {
        ruled_out = true;
        break;
      }
      if (truth == BranchTruth::kAlways) {
        continue;
      }
      std::optional<Predicate> moved = InBranch(node, k, predicate);
      if (!moved.has_value()) {
        return nullptr;  // Movable checked every column: unreachable
      }
      kept.push_back(*std::move(moved));
    }
    if (ruled_out) {
      continue;
    }
    LogicalNodePtr input = node.inputs[k];
    if (!kept.empty()) {
      input = Make(FilterNode{.input = std::move(input),
                              .predicates = std::move(kept),
                              .needed_above = std::nullopt,
                              .span = filter.span});
    }
    inputs.push_back(FilterIntoUnionBranches(input));
    columns.push_back(node.columns[k]);
  }
  if (inputs.empty()) {
    UnionAllNode first{.inputs = {FilterIntoUnionBranches(node.inputs.front())},
                       .columns = {node.columns.front()},
                       .ids = node.ids,
                       .span = node.span};
    return Make(
        FilterNode{.input = Make(std::move(first)),
                   .predicates = {Predicate{.kind = Predicate::Kind::kFalse, .span = filter.span}},
                   .needed_above = std::nullopt,
                   .span = filter.span});
  }
  return Make(UnionAllNode{.inputs = std::move(inputs),
                           .columns = std::move(columns),
                           .ids = node.ids,
                           .span = node.span});
}

}  // namespace

std::optional<Constant> ConstantColumn(const LogicalNodePtr& node, ColumnId id) {
  if (node == nullptr) {
    return std::nullopt;
  }
  return std::visit(
      [&](const auto& n) -> std::optional<Constant> {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, FilterNode> || std::is_same_v<T, SortNode> ||
                      std::is_same_v<T, LimitNode>) {
          return ConstantColumn(n.input, id);
        } else if constexpr (std::is_same_v<T, ComputeNode>) {
          const std::optional<std::size_t> at = PositionOf(n.ids, id);
          if (!at.has_value()) {
            return ConstantColumn(n.input, id);
          }
          if (*at >= n.exprs.size() || n.exprs[*at] == nullptr) {
            return std::nullopt;
          }
          const Expr& expr = *n.exprs[*at];
          if (const auto* constant = std::get_if<ConstantExpr>(&expr.node)) {
            return constant->value;  // the NULL item of a branch (ADR 0037's NL3), for one
          }
          const auto* cast = std::get_if<CastExpr>(&expr.node);
          if (cast == nullptr || cast->operand == nullptr) {
            return std::nullopt;
          }
          const auto* column = std::get_if<ColumnExpr>(&cast->operand->node);
          if (column == nullptr) {
            return std::nullopt;
          }
          // Widen admits only an exact widening of the operand's type.
          const std::optional<Constant> from = ConstantColumn(n.input, column->id);
          if (!from.has_value() || from->type != cast->operand->type) {
            return std::nullopt;
          }
          return Widen(*from, expr.type);
        } else if constexpr (std::is_same_v<T, ProjectNode>) {
          const std::optional<std::size_t> at = PositionOf(n.ids, id);
          if (!at.has_value() || *at >= n.constants.size() || *at >= n.columns.size() ||
              !n.constants[*at].has_value() || n.constants[*at]->type != n.columns[*at].type) {
            return std::nullopt;
          }
          return n.constants[*at];
        } else if constexpr (std::is_same_v<T, ScanNode> || std::is_same_v<T, AggregateNode> ||
                             std::is_same_v<T, GroupAggregateNode> ||
                             std::is_same_v<T, GroupingSetsNode> ||
                             std::is_same_v<T, RowCountNode> || std::is_same_v<T, JoinNode> ||
                             std::is_same_v<T, DependentJoinNode> ||
                             std::is_same_v<T, UnionAllNode>) {
          return std::nullopt;
        } else {
          static_assert(false, "a node ConstantColumn does not know whether it keeps a constant");
        }
      },
      *node);
}

BranchTruth TruthInBranch(const UnionAllNode& node, std::size_t branch,
                          const Predicate& predicate) {
  if (branch >= node.inputs.size() || branch >= node.columns.size()) {
    return BranchTruth::kUnknown;
  }
  const auto constant_of =
      [&](const std::optional<BoundColumn>& column) -> std::optional<Constant> {
    if (!column.has_value()) {
      return std::nullopt;
    }
    const std::optional<std::size_t> at = PositionOf(node.ids, column->id);
    if (!at.has_value() || *at >= node.columns[branch].size()) {
      return std::nullopt;
    }
    const BoundColumn& source = node.columns[branch][*at];
    std::optional<Constant> value = ConstantColumn(node.inputs[branch], source.id);
    if (!value.has_value() || value->type != source.type) {
      return std::nullopt;
    }
    return value;
  };
  return TruthOver(predicate, constant_of(predicate.column), constant_of(predicate.other));
}

LogicalNodePtr FilterIntoUnionBranches(const LogicalNodePtr& root) {
  if (root == nullptr) {
    return root;
  }
  if (const auto* filter = std::get_if<FilterNode>(root.get());
      filter != nullptr && filter->input != nullptr) {
    if (const auto* node = std::get_if<UnionAllNode>(filter->input.get());
        node != nullptr && Movable(*filter, *node)) {
      if (LogicalNodePtr rewritten = IntoBranches(*filter, *node); rewritten != nullptr) {
        return rewritten;
      }
    }
  }
  return MapInputs(root, FilterIntoUnionBranches);
}

}  // namespace antb1::plan
