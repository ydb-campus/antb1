#include "antb1/sql/ast.h"

#include <string_view>
#include <variant>

namespace antb1::sql {
namespace {

bool Eq(const ColumnRef& a, const ColumnRef& b) { return a.name == b.name && a.quoted == b.quoted; }

bool Eq(const Literal& a, const Literal& b) {
  return a.kind == b.kind && a.negative == b.negative && a.text == b.text;
}

bool Eq(const AggregateCall& a, const AggregateCall& b) {
  if (a.kind != b.kind || a.arg.has_value() != b.arg.has_value()) {
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
  }
  return "?";
}

bool EqualIgnoringSpans(const SelectStatement& a, const SelectStatement& b) {
  if (a.star != b.star || a.items.size() != b.items.size() || a.where.size() != b.where.size() ||
      a.limit != b.limit || a.from.kind != b.from.kind || a.from.name != b.from.name ||
      a.from.quoted != b.from.quoted) {
    return false;
  }
  for (std::size_t i = 0; i < a.items.size(); ++i) {
    if (!Eq(a.items[i].expr, b.items[i].expr) || a.items[i].alias != b.items[i].alias) {
      return false;
    }
  }
  for (std::size_t i = 0; i < a.where.size(); ++i) {
    if (!Eq(a.where[i].column, b.where[i].column) || a.where[i].op != b.where[i].op ||
        !Eq(a.where[i].literal, b.where[i].literal)) {
      return false;
    }
  }
  return true;
}

}  // namespace antb1::sql
