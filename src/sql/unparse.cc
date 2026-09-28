#include "antb1/sql/unparse.h"

#include <cstddef>
#include <string>
#include <variant>
#include <vector>

#include "antb1/sql/ast.h"

namespace antb1::sql {
namespace {

// Binding powers, as in the parser: a child that binds more loosely than its place needs gets
// parentheses.
constexpr int kOr = 1;
constexpr int kAnd = 2;
constexpr int kNot = 3;
constexpr int kComparison = 4;
constexpr int kAdditive = 5;
constexpr int kMultiplicative = 6;
constexpr int kUnary = 7;
constexpr int kPrimary = 8;

std::string Quote(const std::string& text, char quote) {
  std::string out(1, quote);
  for (const char c : text) {
    out.push_back(c);
    if (c == quote) {
      out.push_back(quote);
    }
  }
  out.push_back(quote);
  return out;
}

std::string Column(const ColumnRef& col) { return col.quoted ? Quote(col.name, '"') : col.name; }

std::string LiteralSql(const Literal& lit) {
  switch (lit.kind) {
    case Literal::Kind::kInteger:
    case Literal::Kind::kDecimal:
      return (lit.negative ? "-" : "") + lit.text;
    case Literal::Kind::kString:
      return Quote(lit.text, '\'');
    case Literal::Kind::kDate:
      return "DATE " + Quote(lit.text, '\'');
  }
  return "?";
}

int Precedence(BinaryOp op) {
  switch (op) {
    case BinaryOp::kOr:
      return kOr;
    case BinaryOp::kAnd:
      return kAnd;
    case BinaryOp::kEq:
    case BinaryOp::kNe:
    case BinaryOp::kLt:
    case BinaryOp::kLe:
    case BinaryOp::kGt:
    case BinaryOp::kGe:
      return kComparison;
    case BinaryOp::kAdd:
    case BinaryOp::kSubtract:
      return kAdditive;
    case BinaryOp::kMultiply:
    case BinaryOp::kDivide:
    case BinaryOp::kIntegerDivide:
    case BinaryOp::kModulo:
      return kMultiplicative;
  }
  return kPrimary;
}

int Precedence(const Expr& expr) {
  if (const auto* binary = std::get_if<BinaryExpr>(&expr)) {
    return Precedence(binary->op);
  }
  if (const auto* unary = std::get_if<UnaryExpr>(&expr)) {
    return unary->op == UnaryOp::kNot ? kNot : kUnary;
  }
  if (std::holds_alternative<LikeExpr>(expr) || std::holds_alternative<InExpr>(expr)) {
    return kComparison;
  }
  return kPrimary;
}

std::string Sql(const Expr& expr);

// `expr` in a place that needs at least binding power `min`.
std::string Sql(const Expr& expr, int min) {
  std::string text = Sql(expr);
  return Precedence(expr) < min ? "(" + text + ")" : text;
}

std::string List(const std::vector<Expr>& exprs) {
  std::string out;
  for (std::size_t i = 0; i < exprs.size(); ++i) {
    out += (i == 0 ? "" : ", ") + Sql(exprs[i], kOr);
  }
  return out;
}

struct SqlOf {
  std::string operator()(const ColumnRef& column) const { return Column(column); }
  std::string operator()(const Literal& lit) const { return LiteralSql(lit); }
  std::string operator()(const AggregateCall& agg) const {
    if (agg.kind == AggKind::kCountStar) {
      return "COUNT(*)";
    }
    return std::string(ToString(agg.kind)) + "(" + (agg.distinct ? "DISTINCT " : "") +
           (agg.arg.has_value() ? Sql(**agg.arg, kOr) : "") + ")";
  }
  std::string operator()(const UnaryExpr& unary) const {
    if (unary.op == UnaryOp::kNot) {
      return "NOT " + Sql(*unary.operand, kNot);
    }
    // Always parenthesized: "-5" would read back as a negative literal, "- -a" as a comment.
    return "-(" + Sql(*unary.operand, kOr) + ")";
  }
  std::string operator()(const BinaryExpr& binary) const {
    const int p = Precedence(binary.op);
    // Left-associative, except comparisons, which do not chain at all.
    const int left = p == kComparison ? kComparison + 1 : p;
    return Sql(*binary.left, left) + " " + std::string(ToString(binary.op)) + " " +
           Sql(*binary.right, p + 1);
  }
  std::string operator()(const LikeExpr& like) const {
    return Sql(*like.operand, kComparison + 1) + (like.negated ? " NOT LIKE " : " LIKE ") +
           Sql(*like.pattern, kComparison + 1);
  }
  std::string operator()(const InExpr& in) const {
    return Sql(*in.operand, kComparison + 1) + (in.negated ? " NOT IN (" : " IN (") +
           List(in.list) + ")";
  }
  std::string operator()(const FunctionCall& call) const {
    return (call.quoted ? Quote(call.name, '"') : call.name) + "(" + List(call.args) + ")";
  }
  std::string operator()(const CaseExpr& c) const {
    std::string out = "CASE";
    if (c.operand.has_value()) {
      out += " " + Sql(**c.operand, kOr);
    }
    for (const CaseBranch& branch : c.branches) {
      out += " WHEN " + Sql(*branch.when, kOr) + " THEN " + Sql(*branch.then, kOr);
    }
    if (c.otherwise.has_value()) {
      out += " ELSE " + Sql(**c.otherwise, kOr);
    }
    return out + " END";
  }
  std::string operator()(const ExtractExpr& e) const {
    return "EXTRACT(" + e.field + " FROM " + Sql(*e.source, kOr) + ")";
  }
};

std::string Sql(const Expr& expr) {
  return std::visit(SqlOf{}, static_cast<const ExprNode&>(expr));
}

// A predicate split into conjuncts; a conjunct that is itself an AND or OR is parenthesized, so it
// reads back as one conjunct.
std::string Conjuncts(const std::vector<Expr>& conjuncts) {
  std::string out;
  for (std::size_t i = 0; i < conjuncts.size(); ++i) {
    out += (i == 0 ? "" : " AND ") + Sql(conjuncts[i], kNot);
  }
  return out;
}

}  // namespace

std::string ToSql(const Expr& expr) { return Sql(expr); }

std::string ToSql(const SelectStatement& stmt) {
  std::string sql = "SELECT ";
  if (stmt.star) {
    sql += '*';
  } else {
    for (std::size_t i = 0; i < stmt.items.size(); ++i) {
      if (i > 0) {
        sql += ", ";
      }
      sql += Sql(stmt.items[i].expr, kOr);
      if (const auto& alias = stmt.items[i].alias; alias.has_value()) {
        sql += " AS " + Quote(*alias, '"');
      }
    }
  }
  sql += " FROM ";
  if (stmt.from.kind == TableRef::Kind::kPath) {
    sql += Quote(stmt.from.name, '\'');
  } else {
    sql += stmt.from.quoted ? Quote(stmt.from.name, '"') : stmt.from.name;
  }
  if (!stmt.where.empty()) {
    sql += " WHERE " + Conjuncts(stmt.where);
  }
  if (!stmt.group_by.empty()) {
    sql += " GROUP BY " + List(stmt.group_by);
  }
  if (!stmt.having.empty()) {
    sql += " HAVING " + Conjuncts(stmt.having);
  }
  for (std::size_t i = 0; i < stmt.order_by.size(); ++i) {
    const OrderItem& item = stmt.order_by[i];
    sql += i == 0 ? " ORDER BY " : ", ";
    sql += Sql(item.expr, kOr);
    if (item.descending) {
      sql += " DESC";
    }
    if (item.nulls != NullsOrder::kDefault) {
      sql += " " + std::string(ToString(item.nulls));
    }
  }
  if (stmt.limit) {
    sql += " LIMIT " + std::to_string(*stmt.limit);
  }
  if (stmt.offset) {
    sql += " OFFSET " + std::to_string(*stmt.offset);
  }
  return sql;
}

}  // namespace antb1::sql
