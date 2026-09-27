#include "antb1/sql/ast.h"

#include <cstddef>
#include <string_view>
#include <variant>

namespace antb1::sql {
namespace {

bool Eq(const ColumnRef& a, const ColumnRef& b) { return a.name == b.name && a.quoted == b.quoted; }

bool Eq(const Literal& a, const Literal& b) {
  return a.kind == b.kind && a.negative == b.negative && a.text == b.text;
}

bool Eq(const AggregateCall& a, const AggregateCall& b) {
  if (a.kind != b.kind || a.distinct != b.distinct || a.arg.has_value() != b.arg.has_value()) {
    return false;
  }
  return !a.arg.has_value() || Eq(*a.arg, *b.arg);
}

bool Eq(const SelectExpr& a, const SelectExpr& b) {
  if (a.index() != b.index()) {
    return false;
  }
  if (const auto* agg = std::get_if<AggregateCall>(&a)) {
    return Eq(*agg, std::get<AggregateCall>(b));
  }
  if (const auto* lit = std::get_if<Literal>(&a)) {
    return Eq(*lit, std::get<Literal>(b));
  }
  return Eq(std::get<ColumnRef>(a), std::get<ColumnRef>(b));
}

bool Eq(const GroupExpr& a, const GroupExpr& b) {
  if (a.index() != b.index()) {
    return false;
  }
  if (const auto* lit = std::get_if<Literal>(&a)) {
    return Eq(*lit, std::get<Literal>(b));
  }
  return Eq(std::get<ColumnRef>(a), std::get<ColumnRef>(b));
}

}  // namespace

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

bool EqualIgnoringSpans(const SelectStatement& a, const SelectStatement& b) {
  if (a.star != b.star || a.items.size() != b.items.size() || a.where.size() != b.where.size() ||
      a.group_by.size() != b.group_by.size() || a.order_by.size() != b.order_by.size() ||
      a.limit != b.limit || a.offset != b.offset || a.from.kind != b.from.kind ||
      a.from.name != b.from.name || a.from.quoted != b.from.quoted) {
    return false;
  }
  for (std::size_t i = 0; i < a.group_by.size(); ++i) {
    if (!Eq(a.group_by[i], b.group_by[i])) {
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
  for (std::size_t i = 0; i < a.where.size(); ++i) {
    if (!Eq(a.where[i].column, b.where[i].column) || a.where[i].op != b.where[i].op ||
        !Eq(a.where[i].literal, b.where[i].literal) ||
        a.where[i].list.size() != b.where[i].list.size()) {
      return false;
    }
    for (std::size_t k = 0; k < a.where[i].list.size(); ++k) {
      if (!Eq(a.where[i].list[k], b.where[i].list[k])) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace antb1::sql
