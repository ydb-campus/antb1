#include "strictness.h"

#include <algorithm>
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
        } else if constexpr (std::is_same_v<Node, FunctionExpr>) {
          // Every function is NULL for a NULL argument, as DuckDB's are, but concat, which skips
          // one (ADR 0037's FN5).
          return node.function != Function::kConcat &&
                 std::ranges::all_of(node.args, [](const ExprPtr& arg) { return IsStrict(*arg); });
        } else {
          // PredicateExpr, BoolExpr, CaseExpr: none is strict in general (IS NULL, OR, COALESCE).
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
