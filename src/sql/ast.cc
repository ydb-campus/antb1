#include "antb1/sql/ast.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "antb1/common/source_span.h"

namespace antb1::sql {
namespace {

bool Eq(const Expr& a, const Expr& b);

bool Eq(const ColumnRef& a, const ColumnRef& b) {
  return a.name == b.name && a.quoted == b.quoted && a.qualifier == b.qualifier &&
         a.qualifier_quoted == b.qualifier_quoted;
}

bool Eq(const Literal& a, const Literal& b) {
  return a.kind == b.kind && a.negative == b.negative && a.text == b.text;
}

template <class T>
bool Eq(const std::optional<Box<T>>& a, const std::optional<Box<T>>& b) {
  if (a.has_value() != b.has_value()) {
    return false;
  }
  return !a.has_value() || Eq(**a, **b);
}

bool Eq(const std::vector<Expr>& a, const std::vector<Expr>& b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (!Eq(a[i], b[i])) {
      return false;
    }
  }
  return true;
}

bool Eq(const AggregateCall& a, const AggregateCall& b) {
  return a.kind == b.kind && a.distinct == b.distinct && Eq(a.arg, b.arg);
}

bool Eq(const UnaryExpr& a, const UnaryExpr& b) {
  return a.op == b.op && Eq(*a.operand, *b.operand);
}

bool Eq(const BinaryExpr& a, const BinaryExpr& b) {
  return a.op == b.op && Eq(*a.left, *b.left) && Eq(*a.right, *b.right);
}

bool Eq(const LikeExpr& a, const LikeExpr& b) {
  return a.negated == b.negated && Eq(*a.operand, *b.operand) && Eq(*a.pattern, *b.pattern);
}

bool Eq(const InExpr& a, const InExpr& b) {
  return a.negated == b.negated && Eq(*a.operand, *b.operand) && Eq(a.list, b.list);
}

bool Eq(const BetweenExpr& a, const BetweenExpr& b) {
  return a.negated == b.negated && Eq(*a.operand, *b.operand) && Eq(*a.low, *b.low) &&
         Eq(*a.high, *b.high);
}

bool Eq(const FunctionCall& a, const FunctionCall& b) {
  return a.name == b.name && a.quoted == b.quoted && Eq(a.args, b.args);
}

bool Eq(const CaseExpr& a, const CaseExpr& b) {
  if (!Eq(a.operand, b.operand) || !Eq(a.otherwise, b.otherwise) ||
      a.branches.size() != b.branches.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.branches.size(); ++i) {
    if (!Eq(*a.branches[i].when, *b.branches[i].when) ||
        !Eq(*a.branches[i].then, *b.branches[i].then)) {
      return false;
    }
  }
  return true;
}

bool Eq(const ExtractExpr& a, const ExtractExpr& b) {
  return a.field == b.field && Eq(*a.source, *b.source);
}

// The spelling (CAST or ::) is no part of the node: both are the same cast.
bool Eq(const CastExpr& a, const CastExpr& b) {
  return a.try_cast == b.try_cast && a.type == b.type && a.type_params == b.type_params &&
         Eq(*a.operand, *b.operand);
}

bool Eq(const Expr& a, const Expr& b) {
  if (a.index() != b.index()) {
    return false;
  }
  return std::visit(
      [&b](const auto& node) {
        using Node = std::decay_t<decltype(node)>;
        return Eq(node, std::get<Node>(b));
      },
      a);
}

bool Eq(const TableRef& a, const TableRef& b) {
  return a.kind == b.kind && a.name == b.name && a.quoted == b.quoted;
}

bool Eq(const FromItem& a, const FromItem& b) {
  return a.connector == b.connector && Eq(a.table, b.table) && a.alias == b.alias && Eq(a.on, b.on);
}

std::optional<CompareOp> CompareOpOf(BinaryOp op) {
  switch (op) {
    case BinaryOp::kEq:
      return CompareOp::kEq;
    case BinaryOp::kNe:
      return CompareOp::kNe;
    case BinaryOp::kLt:
      return CompareOp::kLt;
    case BinaryOp::kLe:
      return CompareOp::kLe;
    case BinaryOp::kGt:
      return CompareOp::kGt;
    case BinaryOp::kGe:
      return CompareOp::kGe;
    default:
      return std::nullopt;
  }
}

// The operator with swapped operands: 5 < c  <=>  c > 5.
CompareOp Mirror(CompareOp op) {
  switch (op) {
    case CompareOp::kLt:
      return CompareOp::kGt;
    case CompareOp::kLe:
      return CompareOp::kGe;
    case CompareOp::kGt:
      return CompareOp::kLt;
    case CompareOp::kGe:
      return CompareOp::kLe;
    default:
      return op;
  }
}

// A simple condition over an operand that `operand_of` accepts (it returns std::nullopt for any
// other expression): operand <op> literal, literal <op> operand (mirrored), operand [NOT] LIKE
// literal, operand [NOT] IN (literal, ...).
template <class Operand>
struct Simple {
  Operand operand;
  CompareOp op = CompareOp::kEq;
  Literal literal;
  std::vector<Literal> list;
  SourceSpan span;
};

template <class Operand, class OperandOf>
std::optional<Simple<Operand>> SimpleCondition(const Expr& expr, OperandOf operand_of) {
  using Result = Simple<Operand>;
  if (const auto* binary = std::get_if<BinaryExpr>(&expr)) {
    const std::optional<CompareOp> op = CompareOpOf(binary->op);
    if (!op.has_value()) {
      return std::nullopt;
    }
    if (auto operand = operand_of(*binary->left)) {
      if (const auto* lit = std::get_if<Literal>(&*binary->right)) {
        return Result{.operand = *std::move(operand),
                      .op = *op,
                      .literal = *lit,
                      .list = {},
                      .span = binary->span};
      }
      return std::nullopt;
    }
    if (auto operand = operand_of(*binary->right)) {
      if (const auto* lit = std::get_if<Literal>(&*binary->left)) {
        return Result{.operand = *std::move(operand),
                      .op = Mirror(*op),
                      .literal = *lit,
                      .list = {},
                      .span = binary->span};
      }
    }
    return std::nullopt;
  }
  if (const auto* like = std::get_if<LikeExpr>(&expr)) {
    auto operand = operand_of(*like->operand);
    const auto* pattern = std::get_if<Literal>(&*like->pattern);
    if (!operand.has_value() || pattern == nullptr) {
      return std::nullopt;
    }
    return Result{.operand = *std::move(operand),
                  .op = like->negated ? CompareOp::kNotLike : CompareOp::kLike,
                  .literal = *pattern,
                  .list = {},
                  .span = like->span};
  }
  if (const auto* in = std::get_if<InExpr>(&expr)) {
    auto operand = operand_of(*in->operand);
    if (!operand.has_value()) {
      return std::nullopt;
    }
    Result out{.operand = *std::move(operand),
               .op = in->negated ? CompareOp::kNotIn : CompareOp::kIn,
               .literal = {},
               .list = {},
               .span = in->span};
    for (const Expr& value : in->list) {
      const auto* lit = std::get_if<Literal>(&value);
      if (lit == nullptr) {
        return std::nullopt;
      }
      out.list.push_back(*lit);
    }
    return out;
  }
  return std::nullopt;
}

}  // namespace

const ColumnRef* AggregateCall::arg_column() const {
  return arg.has_value() ? std::get_if<ColumnRef>(&**arg) : nullptr;
}

SourceSpan Expr::span() const {
  return std::visit([](const auto& node) { return node.span; },
                    static_cast<const ExprNode&>(*this));
}

std::optional<Comparison> AsComparison(const Expr& expr) {
  const auto column_of = [](const Expr& e) -> std::optional<ColumnRef> {
    const auto* column = std::get_if<ColumnRef>(&e);
    return column != nullptr ? std::optional(*column) : std::nullopt;
  };
  auto simple = SimpleCondition<ColumnRef>(expr, column_of);
  if (!simple.has_value()) {
    return std::nullopt;
  }
  return Comparison{.column = std::move(simple->operand),
                    .op = simple->op,
                    .literal = std::move(simple->literal),
                    .list = std::move(simple->list),
                    .span = simple->span};
}

std::optional<HavingComparison> AsHavingComparison(const Expr& expr) {
  const auto operand_of = [](const Expr& e) -> std::optional<HavingOperand> {
    if (const auto* column = std::get_if<ColumnRef>(&e)) {
      return HavingOperand(*column);
    }
    if (const auto* call = std::get_if<AggregateCall>(&e)) {
      return HavingOperand(*call);
    }
    return std::nullopt;
  };
  auto simple = SimpleCondition<HavingOperand>(expr, operand_of);
  if (!simple.has_value()) {
    return std::nullopt;
  }
  return HavingComparison{.operand = std::move(simple->operand),
                          .op = simple->op,
                          .literal = std::move(simple->literal),
                          .list = std::move(simple->list),
                          .span = simple->span};
}

namespace {

BinaryOp BinaryOpOf(CompareOp op) {
  switch (op) {
    case CompareOp::kNe:
      return BinaryOp::kNe;
    case CompareOp::kLt:
      return BinaryOp::kLt;
    case CompareOp::kLe:
      return BinaryOp::kLe;
    case CompareOp::kGt:
      return BinaryOp::kGt;
    case CompareOp::kGe:
      return BinaryOp::kGe;
    default:
      return BinaryOp::kEq;
  }
}

Expr ConditionExpr(Expr operand, CompareOp op, const Literal& literal,
                   const std::vector<Literal>& list, SourceSpan span) {
  switch (op) {
    case CompareOp::kLike:
    case CompareOp::kNotLike:
      return Expr(LikeExpr{.operand = Box<Expr>(std::move(operand)),
                           .pattern = Box<Expr>(Expr(literal)),
                           .negated = op == CompareOp::kNotLike,
                           .op_span = {},
                           .span = span});
    case CompareOp::kIn:
    case CompareOp::kNotIn: {
      InExpr in{.operand = Box<Expr>(std::move(operand)),
                .list = {},
                .negated = op == CompareOp::kNotIn,
                .op_span = {},
                .span = span};
      for (const Literal& value : list) {
        in.list.emplace_back(value);
      }
      return Expr(std::move(in));
    }
    default:
      return Expr(BinaryExpr{.op = BinaryOpOf(op),
                             .left = Box<Expr>(std::move(operand)),
                             .right = Box<Expr>(Expr(literal)),
                             .op_span = {},
                             .span = span});
  }
}

}  // namespace

Expr ToExpr(const Comparison& cmp) {
  return ConditionExpr(Expr(cmp.column), cmp.op, cmp.literal, cmp.list, cmp.span);
}

Expr ToExpr(const HavingComparison& cmp) {
  Expr operand = std::visit([](const auto& node) { return Expr(node); }, cmp.operand);
  return ConditionExpr(std::move(operand), cmp.op, cmp.literal, cmp.list, cmp.span);
}

std::string_view ToString(AggKind kind) {
  switch (kind) {
    case AggKind::kCountStar:
    case AggKind::kCount:
      return "COUNT";
    case AggKind::kSum:
      return "SUM";
    case AggKind::kAvg:
      return "AVG";
    case AggKind::kMin:
      return "MIN";
    case AggKind::kMax:
      return "MAX";
  }
  return "?";
}

std::string_view ToString(NullsOrder nulls) {
  switch (nulls) {
    case NullsOrder::kDefault:
      return "";
    case NullsOrder::kFirst:
      return "NULLS FIRST";
    case NullsOrder::kLast:
      return "NULLS LAST";
  }
  return "?";
}

std::string_view ToString(Connector connector) {
  switch (connector) {
    case Connector::kFirst:
      return "";
    case Connector::kComma:
      return ",";
    case Connector::kCross:
      return "CROSS JOIN";
    case Connector::kInner:
      return "INNER JOIN";
    case Connector::kLeft:
      return "LEFT JOIN";
  }
  return "?";
}

std::string_view ToString(CompareOp op) {
  switch (op) {
    case CompareOp::kEq:
      return "=";
    case CompareOp::kNe:
      return "<>";
    case CompareOp::kLt:
      return "<";
    case CompareOp::kLe:
      return "<=";
    case CompareOp::kGt:
      return ">";
    case CompareOp::kGe:
      return ">=";
    case CompareOp::kLike:
      return "LIKE";
    case CompareOp::kNotLike:
      return "NOT LIKE";
    case CompareOp::kIn:
      return "IN";
    case CompareOp::kNotIn:
      return "NOT IN";
  }
  return "?";
}

std::string_view ToString(BinaryOp op) {
  switch (op) {
    case BinaryOp::kAdd:
      return "+";
    case BinaryOp::kSubtract:
      return "-";
    case BinaryOp::kMultiply:
      return "*";
    case BinaryOp::kDivide:
      return "/";
    case BinaryOp::kIntegerDivide:
      return "//";
    case BinaryOp::kModulo:
      return "%";
    case BinaryOp::kEq:
      return "=";
    case BinaryOp::kNe:
      return "<>";
    case BinaryOp::kLt:
      return "<";
    case BinaryOp::kLe:
      return "<=";
    case BinaryOp::kGt:
      return ">";
    case BinaryOp::kGe:
      return ">=";
    case BinaryOp::kAnd:
      return "AND";
    case BinaryOp::kOr:
      return "OR";
  }
  return "?";
}

bool EqualIgnoringSpans(const Expr& a, const Expr& b) { return Eq(a, b); }

namespace {

// One level for the node over its deepest child.
struct DepthOf {
  static std::size_t Of(const Expr& e) { return Depth(e); }
  static std::size_t Of(const std::optional<Box<Expr>>& e) { return e ? Depth(**e) : 0; }

  std::size_t operator()(const ColumnRef& /*c*/) const { return 1; }
  std::size_t operator()(const Literal& /*l*/) const { return 1; }
  std::size_t operator()(const AggregateCall& a) const { return 1 + Of(a.arg); }
  std::size_t operator()(const UnaryExpr& u) const { return 1 + Of(*u.operand); }
  std::size_t operator()(const BinaryExpr& b) const {
    return 1 + std::max(Of(*b.left), Of(*b.right));
  }
  std::size_t operator()(const LikeExpr& l) const {
    return 1 + std::max(Of(*l.operand), Of(*l.pattern));
  }
  std::size_t operator()(const InExpr& in) const {
    std::size_t deepest = Of(*in.operand);
    for (const Expr& value : in.list) {
      deepest = std::max(deepest, Of(value));
    }
    return 1 + deepest;
  }
  std::size_t operator()(const BetweenExpr& b) const {
    return 1 + std::max({Of(*b.operand), Of(*b.low), Of(*b.high)});
  }
  std::size_t operator()(const FunctionCall& f) const {
    std::size_t deepest = 0;
    for (const Expr& arg : f.args) {
      deepest = std::max(deepest, Of(arg));
    }
    return 1 + deepest;
  }
  std::size_t operator()(const CaseExpr& c) const {
    std::size_t deepest = std::max(Of(c.operand), Of(c.otherwise));
    for (const CaseBranch& branch : c.branches) {
      deepest = std::max({deepest, Of(*branch.when), Of(*branch.then)});
    }
    return 1 + deepest;
  }
  std::size_t operator()(const ExtractExpr& e) const { return 1 + Of(*e.source); }
  std::size_t operator()(const CastExpr& c) const { return 1 + Of(*c.operand); }
};

}  // namespace

std::size_t Depth(const Expr& expr) {
  return std::visit(DepthOf{}, static_cast<const ExprNode&>(expr));
}

// The FROM list is walked in loops, never recursively, so a long list costs no stack.
std::size_t Depth(const SelectStatement& stmt) {
  std::size_t deepest = 0;
  for (const SelectItem& item : stmt.items) {
    deepest = std::max(deepest, Depth(item.expr));
  }
  for (const FromItem& item : stmt.from) {
    for (const Expr& e : item.on) {
      deepest = std::max(deepest, Depth(e));
    }
  }
  for (const std::vector<Expr>* list : {&stmt.where, &stmt.group_by, &stmt.having}) {
    for (const Expr& e : *list) {
      deepest = std::max(deepest, Depth(e));
    }
  }
  for (const OrderItem& item : stmt.order_by) {
    deepest = std::max(deepest, Depth(item.expr));
  }
  return deepest;
}

bool EqualIgnoringSpans(const SelectStatement& a, const SelectStatement& b) {
  if (a.star != b.star || a.items.size() != b.items.size() ||
      a.order_by.size() != b.order_by.size() || a.limit != b.limit || a.offset != b.offset ||
      a.from.size() != b.from.size() || !Eq(a.where, b.where) || !Eq(a.group_by, b.group_by) ||
      !Eq(a.having, b.having)) {
    return false;
  }
  for (std::size_t i = 0; i < a.from.size(); ++i) {
    if (!Eq(a.from[i], b.from[i])) {
      return false;
    }
  }
  for (std::size_t i = 0; i < a.order_by.size(); ++i) {
    if (!Eq(a.order_by[i].expr, b.order_by[i].expr) ||
        a.order_by[i].descending != b.order_by[i].descending ||
        a.order_by[i].nulls != b.order_by[i].nulls) {
      return false;
    }
  }
  for (std::size_t i = 0; i < a.items.size(); ++i) {
    if (!Eq(a.items[i].expr, b.items[i].expr) || a.items[i].alias != b.items[i].alias) {
      return false;
    }
  }
  return true;
}

}  // namespace antb1::sql
