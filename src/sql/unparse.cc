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
    return std::string(ToString(agg->kind)) + "(" + (agg->distinct ? "DISTINCT " : "") +
           (agg->arg ? Column(*agg->arg) : "") + ")";
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
    sql += Column(c.column) + " " + std::string(ToString(c.op)) + " ";
    if (c.op == CompareOp::kIn || c.op == CompareOp::kNotIn) {
      for (std::size_t k = 0; k < c.list.size(); ++k) {
        sql += (k == 0 ? "(" : ", ") + LiteralSql(c.list[k]);
      }
      sql += ')';
    } else {
      sql += LiteralSql(c.literal);
    }
  }
  for (std::size_t i = 0; i < stmt.group_by.size(); ++i) {
    sql += i == 0 ? " GROUP BY " : ", ";
    sql += Column(stmt.group_by[i]);
  }
  for (std::size_t i = 0; i < stmt.order_by.size(); ++i) {
    const OrderItem& item = stmt.order_by[i];
    sql += i == 0 ? " ORDER BY " : ", ";
    sql += Expr(item.expr);
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
