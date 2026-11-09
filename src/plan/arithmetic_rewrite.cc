#include "arithmetic_rewrite.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "antb1/common/check.h"
#include "antb1/common/int128.h"
#include "antb1/plan/literal.h"
#include "antb1/plan/types.h"
#include "antb1/sql/parser.h"

namespace antb1::plan {
namespace {

ExprPtr Share(Expr expr) { return std::make_shared<const Expr>(std::move(expr)); }

// The children of an expression node, in order (a null ELSE included), as logical_plan.cc lists
// them for MapColumns: every node with operands.
std::vector<ExprPtr*> ChildrenOf(Expr& expr) {
  std::vector<ExprPtr*> out;
  const auto all = [&out](std::vector<ExprPtr>& exprs) {
    for (ExprPtr& e : exprs) {
      out.push_back(&e);
    }
  };
  if (auto* arith = std::get_if<ArithExpr>(&expr.node)) {
    out = {&arith->left, &arith->right};
  } else if (auto* negate = std::get_if<NegateExpr>(&expr.node)) {
    out = {&negate->operand};
  } else if (auto* function = std::get_if<FunctionExpr>(&expr.node)) {
    all(function->args);
  } else if (auto* predicate = std::get_if<PredicateExpr>(&expr.node)) {
    all(predicate->operands);
  } else if (auto* boolean = std::get_if<BoolExpr>(&expr.node)) {
    all(boolean->args);
  } else if (auto* c = std::get_if<CaseExpr>(&expr.node)) {
    all(c->whens);
    all(c->thens);
    out.push_back(&c->otherwise);
  } else if (auto* cast = std::get_if<CastExpr>(&expr.node)) {
    out = {&cast->operand};
  }
  return out;
}

// Whether `expr` reads no column: DuckDB's IsFoldable, for antb1's expressions, whose aggregates
// and subqueries are columns by the time an expression reads them.
bool Foldable(const Expr& expr) {
  if (std::holds_alternative<ColumnExpr>(expr.node)) {
    return false;
  }
  Expr copy = expr;  // ChildrenOf takes a mutable node; the copy shares the children
  return std::ranges::all_of(ChildrenOf(copy), [](const ExprPtr* child) {
    return *child == nullptr || Foldable(**child);
  });
}

// The value of an integer expression that reads no column, in its type, as DuckDB's
// ConstantFolding computes it: an operation's operands cast to its type, the operation checked in
// it, // truncating and % with the dividend's sign. std::nullopt where that fails (an overflow, or
// a cast of a value its type cannot hold), for a zero divisor (DuckDB's NULL, which this does not
// fold: it is computed instead, to NULL) and for anything but literals and + - * // %, negation,
// casts and abs over them.
std::optional<Int128> Evaluate(const Expr& expr) {
  if (!IsInteger(expr.type)) {
    return std::nullopt;
  }
  const IntegerRange range = RangeOf(expr.type);
  const auto within = [&range](std::optional<Int128> v) -> std::optional<Int128> {
    return v.has_value() && *v >= range.min && *v <= range.max ? v : std::nullopt;
  };
  if (const auto* constant = std::get_if<ConstantExpr>(&expr.node)) {
    const auto* value = std::get_if<Int128>(&constant->value.value);
    return value == nullptr ? std::nullopt : within(*value);
  }
  if (const auto* arith = std::get_if<ArithExpr>(&expr.node)) {
    const std::optional<Int128> left = within(Evaluate(*arith->left));
    const std::optional<Int128> right = within(Evaluate(*arith->right));
    if (!left.has_value() || !right.has_value()) {
      return std::nullopt;
    }
    Int128 out = 0;
    bool overflow = true;
    switch (arith->op) {
      case ArithOp::kAdd:
        overflow = __builtin_add_overflow(*left, *right, &out);
        break;
      case ArithOp::kSubtract:
        overflow = __builtin_sub_overflow(*left, *right, &out);
        break;
      case ArithOp::kMultiply:
        overflow = __builtin_mul_overflow(*left, *right, &out);
        break;
      case ArithOp::kIntegerDivide:
      case ArithOp::kModulo:
        // The minimum divided by -1 overflows; its remainder is computed rather than decided here.
        if (*right != 0 && (*right != -1 || *left != range.min)) {
          out = arith->op == ArithOp::kIntegerDivide ? *left / *right : *left % *right;
          overflow = false;
        }
        break;
      default:
        break;
    }
    return overflow ? std::nullopt : within(out);
  }
  if (const auto* negate = std::get_if<NegateExpr>(&expr.node)) {
    const std::optional<Int128> value = Evaluate(*negate->operand);
    return value.has_value() ? within(-*value) : std::nullopt;
  }
  if (const auto* cast = std::get_if<CastExpr>(&expr.node)) {
    return within(Evaluate(*cast->operand));
  }
  if (const auto* function = std::get_if<FunctionExpr>(&expr.node);
      function != nullptr && function->function == Function::kAbs && function->args.size() == 1) {
    const std::optional<Int128> value = Evaluate(*function->args.front());
    return value.has_value() ? within(*value < 0 ? -*value : *value) : std::nullopt;
  }
  return std::nullopt;
}

// The operands of the chain `expr` heads, in order: the nodes of operator `op` and type `type`
// are its inner nodes, every other node an operand.
void ChainOperands(const ExprPtr& expr, ArithOp op, LogicalType type, std::vector<ExprPtr>& out) {
  if (const auto* arith = std::get_if<ArithExpr>(&expr->node);
      arith != nullptr && arith->op == op && expr->type == type) {
    ChainOperands(arith->left, op, type, out);
    ChainOperands(arith->right, op, type, out);
    return;
  }
  out.push_back(expr);
}

// The levels of `expr`'s tree, 1 for a leaf.
std::size_t Depth(const Expr& expr) {
  Expr copy = expr;  // ChildrenOf takes a mutable node; the copy shares the children
  std::size_t deepest = 0;
  for (const ExprPtr* child : ChildrenOf(copy)) {
    if (*child != nullptr) {
      deepest = std::max(deepest, Depth(**child));
    }
  }
  return deepest + 1;
}

// Rule 1, ConstantOrderNormalization (arithmetic_rewrite.h): the chain left-deep with its
// constants first, or nullptr where it does not apply. A chain at `level` of its expression (1 at
// the top) whose left-deep form would take the expression deeper than the parser lets one be
// (sql::kMaxExpressionDepth: a balanced chain of many operands) is left as it is, so that no walk
// of the plan recurses deeper than parsing does.
ExprPtr OrderConstants(const ExprPtr& expr, std::size_t level) {
  const auto* arith = std::get_if<ArithExpr>(&expr->node);
  if (arith == nullptr || (arith->op != ArithOp::kAdd && arith->op != ArithOp::kMultiply) ||
      !IsInteger(expr->type)) {
    return nullptr;
  }
  std::vector<ExprPtr> operands;
  ChainOperands(expr, arith->op, expr->type, operands);
  const auto constant = [](const ExprPtr& e) { return Foldable(*e); };
  const auto variable = std::ranges::find_if_not(operands, constant);
  if (std::ranges::count_if(operands, constant) < 2 ||
      std::find_if(variable, operands.end(), constant) == operands.end()) {
    return nullptr;
  }
  if (level - 1 + operands.size() > sql::kMaxExpressionDepth) {
    return nullptr;
  }
  std::size_t deepest = 0;
  for (const ExprPtr& operand : operands) {
    deepest = std::max(deepest, Depth(*operand));
  }
  if (level - 1 + operands.size() - 1 + deepest > sql::kMaxExpressionDepth) {
    return nullptr;
  }
  std::ranges::stable_partition(operands, constant);
  ExprPtr out = operands.front();
  for (std::size_t i = 1; i < operands.size(); ++i) {
    std::string name = std::format("({} {} {})", out->name, ToString(arith->op), operands[i]->name);
    out = Share(Expr{.node = ArithExpr{.op = arith->op, .left = out, .right = operands[i]},
                     .type = expr->type,
                     .name = std::move(name)});
  }
  return out;
}

// Rule 2, ConstantFolding: the value of an integer expression that reads no column, or nullptr
// where it is a constant already or its evaluation fails.
ExprPtr FoldConstant(const ExprPtr& expr) {
  if (std::holds_alternative<ConstantExpr>(expr->node)) {
    return nullptr;
  }
  const std::optional<Int128> value = Evaluate(*expr);
  if (!value.has_value()) {
    return nullptr;
  }
  return Share(Expr{.node = ConstantExpr{.value = Constant{.type = expr->type, .value = *value}},
                    .type = expr->type,
                    .name = expr->name});
}

// DuckDB's constant_or_null(0, x), its x * 0: 0 where x is not NULL, NULL where it is, x computed
// all the same. A CASE, so that no chain of * reads through it, as none reads through DuckDB's
// function: CASE WHEN x IS NOT NULL THEN 0 END, whose condition is a predicate that is NULL for a
// NULL x, and so takes no row there (PredicateExpr).
ExprPtr ConstantOrNullZero(const ExprPtr& x, LogicalType type, std::string name) {
  Predicate not_null{.kind = Predicate::Kind::kIsNotNull,
                     .column = BoundColumn{.index = 0, .name = x->name, .type = x->type}};
  ExprPtr when =
      Share(Expr{.node = PredicateExpr{.predicate = std::move(not_null), .operands = {x}},
                 .type = LogicalType::kBoolean,
                 .name = std::format("({} IS NOT NULL)", x->name)});
  ExprPtr zero =
      Share(Expr{.node = ConstantExpr{.value = Constant{.type = type, .value = Int128{0}}},
                 .type = type,
                 .name = "0"});
  return Share(Expr{
      .node =
          CaseExpr{.whens = {std::move(when)}, .thens = {std::move(zero)}, .otherwise = nullptr},
      .type = type,
      .name = std::move(name)});
}

// Rule 3, ArithmeticSimplification: the operand that x + 0, 0 + x, x - 0, x * 1, 1 * x or x // 1
// keeps, x * 0 and 0 * x as DuckDB's constant_or_null(0, x), or nullptr. DuckDB reads the first
// constant operand, the left one where both are.
ExprPtr Simplify(const ExprPtr& expr) {
  const auto* arith = std::get_if<ArithExpr>(&expr->node);
  if (arith == nullptr || !IsInteger(expr->type) || !IsInteger(arith->left->type) ||
      !IsInteger(arith->right->type)) {
    return nullptr;
  }
  const bool left_constant = std::holds_alternative<ConstantExpr>(arith->left->node);
  const ExprPtr& constant = left_constant ? arith->left : arith->right;
  const auto* value = std::get_if<ConstantExpr>(&constant->node);
  const auto* number = value == nullptr ? nullptr : std::get_if<Int128>(&value->value.value);
  if (number == nullptr) {
    return nullptr;
  }
  bool keep = false;
  switch (arith->op) {
    case ArithOp::kAdd:
      keep = *number == 0;
      break;
    case ArithOp::kSubtract:
      keep = *number == 0 && !left_constant;
      break;
    case ArithOp::kMultiply:
      if (*number == 0) {
        return ConstantOrNullZero(left_constant ? arith->right : arith->left, expr->type,
                                  expr->name);
      }
      keep = *number == 1;
      break;
    case ArithOp::kIntegerDivide:
      keep = *number == 1 && !left_constant;
      break;
    default:
      break;
  }
  if (!keep) {
    return nullptr;
  }
  const ExprPtr& kept = left_constant ? arith->right : arith->left;
  if (kept->type == expr->type) {
    return kept;
  }
  // DuckDB's operand is of the expression's type already, through a cast.
  if (!IsExactWidening(kept->type, expr->type)) {
    return nullptr;
  }
  std::string name = std::format("CAST({} AS {})", kept->name, ToString(expr->type));
  return Share(
      Expr{.node = CastExpr{.operand = kept}, .type = expr->type, .name = std::move(name)});
}

// One top-down pass: the node's rules until none fires, then its children; `changed` is set when
// a rule fires anywhere. `level` is the node's in its expression, 1 at the top.
ExprPtr Pass(ExprPtr expr, bool& changed, std::size_t level) {
  for (;;) {
    ExprPtr next = OrderConstants(expr, level);
    if (next == nullptr) {
      next = FoldConstant(expr);
    }
    if (next == nullptr) {
      next = Simplify(expr);
    }
    if (next == nullptr) {
      break;
    }
    expr = std::move(next);
    changed = true;
  }
  Expr copy = *expr;
  bool rebuilt = false;
  for (ExprPtr* child : ChildrenOf(copy)) {
    if (*child == nullptr) {
      continue;
    }
    ExprPtr rewritten = Pass(*child, changed, level + 1);
    if (rewritten != *child) {
      *child = std::move(rewritten);
      rebuilt = true;
    }
  }
  return rebuilt ? Share(std::move(copy)) : expr;
}

// `expr` without its casts of an integer value to the value's own type, which DuckDB's binder never
// writes (it returns the value itself) and antb1's keeps for GROUP BY to match: each is its
// operand, so that the rules read the chains DuckDB reads.
ExprPtr StripIdentityCasts(const ExprPtr& expr) {
  if (const auto* cast = std::get_if<CastExpr>(&expr->node);
      cast != nullptr && IsInteger(expr->type) && cast->operand->type == expr->type) {
    return StripIdentityCasts(cast->operand);
  }
  Expr copy = *expr;
  bool rebuilt = false;
  for (ExprPtr* child : ChildrenOf(copy)) {
    if (*child == nullptr) {
      continue;
    }
    ExprPtr stripped = StripIdentityCasts(*child);
    if (stripped != *child) {
      *child = std::move(stripped);
      rebuilt = true;
    }
  }
  return rebuilt ? Share(std::move(copy)) : expr;
}

// `out`, a rewrite of `expr`, named as `expr` is.
ExprPtr NamedAs(const ExprPtr& out, const ExprPtr& expr) {
  if (out == expr) {
    return expr;
  }
  ANTB1_CHECK(out->type == expr->type);
  if (out->name == expr->name) {
    return out;
  }
  Expr named = *out;
  named.name = expr->name;
  return Share(std::move(named));
}

}  // namespace

ExprPtr RewriteArithmetic(const ExprPtr& expr) {
  ExprPtr out = StripIdentityCasts(expr);
  for (bool changed = true; changed;) {
    changed = false;
    out = Pass(out, changed, 1);
  }
  return NamedAs(out, expr);
}

ExprPtr RewriteArithmeticPass(const ExprPtr& expr) {
  bool changed = false;
  return NamedAs(Pass(StripIdentityCasts(expr), changed, 1), expr);
}

}  // namespace antb1::plan
