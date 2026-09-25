#include "antb1/sql/unparse.h"

#include <cstddef>
#include <string>
#include <variant>

#include "antb1/sql/ast.h"

namespace antb1::sql {
namespace {

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

std::string Expr(const SelectExpr& expr) {
  if (const auto* agg = std::get_if<AggregateCall>(&expr)) {
    if (agg->kind == AggKind::kCountStar) {
      return "COUNT(*)";
    }
    return std::string(ToString(agg->kind)) + "(" + (agg->arg ? Column(*agg->arg) : "") + ")";
  }
  return Column(std::get<ColumnRef>(expr));
}

}  // namespace

std::string ToSql(const SelectStatement& stmt) {
  std::string sql = "SELECT ";
  if (stmt.star) {
    sql += '*';
  } else {
    for (std::size_t i = 0; i < stmt.items.size(); ++i) {
      if (i > 0) {
        sql += ", ";
      }
      sql += Expr(stmt.items[i].expr);
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
  for (std::size_t i = 0; i < stmt.where.size(); ++i) {
    const Comparison& c = stmt.where[i];
    sql += i == 0 ? " WHERE " : " AND ";
    sql += Column(c.column) + " " + std::string(ToString(c.op)) + " " + LiteralSql(c.literal);
  }
  if (stmt.limit) {
    sql += " LIMIT " + std::to_string(*stmt.limit);
  }
  return sql;
}

}  // namespace antb1::sql
