#include "strictness.h"

#include <type_traits>
#include <variant>
#include <vector>

namespace antb1::plan {

bool IsStrict(const Expr& expr) {
  return std::visit(
      [](const auto& node) {
        using Node = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<Node, ColumnExpr> || std::is_same_v<Node, ConstantExpr>) {
          return true;  // a column, or a literal
        } else if constexpr (std::is_same_v<Node, ArithExpr>) {
          return IsStrict(*node.left) && IsStrict(*node.right);
        } else if constexpr (std::is_same_v<Node, NegateExpr> || std::is_same_v<Node, CastExpr>) {
          // Unary minus, and the implicit casts of an operator's operands.
          return IsStrict(*node.operand);
        } else {
          // FunctionExpr, PredicateExpr, BoolExpr, CaseExpr: none is strict in general.
          return false;
        }
      },
      expr.node);
}

bool PropagatesNull(const Expr& expr) {
  std::vector<ColumnId> reads;
  CollectColumnIds(expr, reads);
  return !reads.empty() && IsStrict(expr);
}

}  // namespace antb1::plan
