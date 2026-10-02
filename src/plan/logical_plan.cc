#include "antb1/plan/logical_plan.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <arrow/api.h>

#include "antb1/common/check.h"
#include "antb1/common/int128.h"
#include "antb1/common/narrow.h"
#include "antb1/plan/literal.h"
#include "antb1/plan/types.h"

namespace antb1::plan {
namespace {

template <class T>
std::optional<T> NarrowInt128(Int128 value) {
  return Int128ToInt64(value).and_then([](int64_t v) { return TryNarrow<T>(v); });
}

template <class ScalarType, class CType>
arrow::Result<std::shared_ptr<arrow::Scalar>> IntegerScalar(Int128 value, LogicalType type) {
  const auto narrow = NarrowInt128<CType>(value);
  if (!narrow.has_value()) {
    return arrow::Status::Invalid("constant ", Int128ToString(value), " is outside the range of ",
                                  ToString(type));
  }
  return std::make_shared<ScalarType>(*narrow);
}

arrow::Result<std::shared_ptr<arrow::Scalar>> HugeIntScalar(Int128 value) {
  const IntegerRange range = RangeOf(LogicalType::kHugeInt);
  if (value < range.min || value > range.max) {
    return arrow::Status::Invalid("constant ", Int128ToString(value),
                                  " is outside the range of HUGEINT");
  }
  const auto bits = static_cast<UInt128>(value);
  const arrow::Decimal128 decimal(static_cast<int64_t>(bits >> 64U), static_cast<uint64_t>(bits));
  return std::make_shared<arrow::Decimal128Scalar>(decimal, ToArrow(LogicalType::kHugeInt));
}

// One overload per node type: a node type without one fails to compile.
struct NodeNameOf {
  std::string_view operator()(const ScanNode& /*node*/) const { return "Scan"; }
  std::string_view operator()(const FilterNode& /*node*/) const { return "Filter"; }
  std::string_view operator()(const ComputeNode& /*node*/) const { return "Compute"; }
  std::string_view operator()(const ProjectNode& /*node*/) const { return "Project"; }
  std::string_view operator()(const AggregateNode& /*node*/) const { return "Aggregate"; }
  std::string_view operator()(const GroupAggregateNode& /*node*/) const { return "GroupAggregate"; }
  std::string_view operator()(const SortNode& /*node*/) const { return "Sort"; }
  std::string_view operator()(const LimitNode& /*node*/) const { return "Limit"; }
  std::string_view operator()(const RowCountNode& /*node*/) const { return "RowCount"; }
};

struct InputOfNode {
  const LogicalNodePtr* operator()(const ScanNode& /*node*/) const { return nullptr; }
  const LogicalNodePtr* operator()(const FilterNode& node) const { return &node.input; }
  const LogicalNodePtr* operator()(const ComputeNode& node) const { return &node.input; }
  const LogicalNodePtr* operator()(const ProjectNode& node) const { return &node.input; }
  const LogicalNodePtr* operator()(const AggregateNode& node) const { return &node.input; }
  const LogicalNodePtr* operator()(const GroupAggregateNode& node) const { return &node.input; }
  const LogicalNodePtr* operator()(const SortNode& node) const { return &node.input; }
  const LogicalNodePtr* operator()(const LimitNode& node) const { return &node.input; }
  const LogicalNodePtr* operator()(const RowCountNode& /*node*/) const { return nullptr; }
};

}  // namespace

std::string EscapeText(std::string_view text, char quote) {
  std::string out;
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte >= 0x7F || c == '\\') {
      out += std::format("\\x{:02X}", byte);
    } else {
      out += c;
      if (c == quote) {
        out += quote;
      }
    }
  }
  return out;
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

std::string_view ToString(ArithOp op) {
  switch (op) {
    case ArithOp::kAdd:
      return "+";
    case ArithOp::kSubtract:
      return "-";
    case ArithOp::kMultiply:
      return "*";
    case ArithOp::kDivide:
      return "/";
    case ArithOp::kIntegerDivide:
      return "//";
    case ArithOp::kModulo:
      return "%";
  }
  return "?";
}

namespace {

bool SameConstant(const Constant& a, const Constant& b) {
  if (a.type != b.type || a.value.index() != b.value.index()) {
    return false;
  }
  if (const auto* d = std::get_if<double>(&a.value)) {
    // Bitwise, so that NaN matches itself and -0.0 is not 0.0.
    return std::bit_cast<std::uint64_t>(*d) ==
           std::bit_cast<std::uint64_t>(std::get<double>(b.value));
  }
  return a.value == b.value;
}

bool SameExprs(const std::vector<ExprPtr>& a, const std::vector<ExprPtr>& b) {
  return std::ranges::equal(a, b, [](const ExprPtr& x, const ExprPtr& y) {
    return (x == nullptr) == (y == nullptr) && (x == nullptr || SameExpr(*x, *y));
  });
}

bool SamePredicate(const Predicate& a, const Predicate& b) {
  return a.kind == b.kind && a.op == b.op && SameConstant(a.constant, b.constant) &&
         std::ranges::equal(a.values, b.values, SameConstant) &&
         a.column.has_value() == b.column.has_value() && a.other.has_value() == b.other.has_value();
}

struct SameNode {
  const Expr& other;
  bool operator()(const ColumnExpr& a) const {
    return std::get<ColumnExpr>(other.node).index == a.index;
  }
  bool operator()(const ConstantExpr& a) const {
    return SameConstant(a.value, std::get<ConstantExpr>(other.node).value);
  }
  bool operator()(const ArithExpr& a) const {
    const auto& b = std::get<ArithExpr>(other.node);
    return a.op == b.op && SameExpr(*a.left, *b.left) && SameExpr(*a.right, *b.right);
  }
  bool operator()(const NegateExpr& a) const {
    return SameExpr(*a.operand, *std::get<NegateExpr>(other.node).operand);
  }
  bool operator()(const FunctionExpr& a) const {
    const auto& b = std::get<FunctionExpr>(other.node);
    if (a.function != b.function || a.args.size() != b.args.size()) {
      return false;
    }
    for (std::size_t i = 0; i < a.args.size(); ++i) {
      if (!SameExpr(*a.args[i], *b.args[i])) {
        return false;
      }
    }
    return true;
  }
  bool operator()(const PredicateExpr& a) const {
    const auto& b = std::get<PredicateExpr>(other.node);
    return SamePredicate(a.predicate, b.predicate) && SameExprs(a.operands, b.operands);
  }
  bool operator()(const BoolExpr& a) const {
    const auto& b = std::get<BoolExpr>(other.node);
    return a.op == b.op && SameExprs(a.args, b.args);
  }
  bool operator()(const CaseExpr& a) const {
    const auto& b = std::get<CaseExpr>(other.node);
    return SameExprs(a.whens, b.whens) && SameExprs(a.thens, b.thens) &&
           SameExprs({a.otherwise}, {b.otherwise});
  }
};

// The children of an expression node, in order (a null ELSE included).
std::vector<ExprPtr*> Children(Expr& expr) {
  std::vector<ExprPtr*> out;
  const auto all = [&](std::vector<ExprPtr>& exprs) {
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
  }
  return out;
}

}  // namespace

bool SameExpr(const Expr& a, const Expr& b) {
  return a.type == b.type && a.node.index() == b.node.index() &&
         std::visit(SameNode{.other = b}, a.node);
}

ExprPtr Renumber(const ExprPtr& expr, const std::vector<int>& remap) {
  Expr out = *expr;
  if (auto* column = std::get_if<ColumnExpr>(&out.node)) {
    column->index = remap.at(Narrow<std::size_t>(column->index));
    ANTB1_CHECK(column->index >= 0);
  }
  for (ExprPtr* child : Children(out)) {
    if (*child != nullptr) {
      *child = Renumber(*child, remap);
    }
  }
  return std::make_shared<const Expr>(std::move(out));
}

void CollectColumns(const Expr& expr, std::vector<int>& out) {
  if (const auto* column = std::get_if<ColumnExpr>(&expr.node)) {
    out.push_back(column->index);
  }
  Expr copy = expr;  // Children takes a mutable node; the copy shares the children
  for (const ExprPtr* child : Children(copy)) {
    if (*child != nullptr) {
      CollectColumns(**child, out);
    }
  }
}

std::string_view ToString(Function function) {
  switch (function) {
    case Function::kStrlen:
      return "strlen";
    case Function::kRegexpReplace:
      return "regexp_replace";
    case Function::kEpochMs:
      return "epoch_ms";
    case Function::kExtract:
      return "extract";
    case Function::kDateTrunc:
      return "date_trunc";
  }
  return "?";
}

std::string_view ToString(AggKind kind) {
  switch (kind) {
    case AggKind::kCountStar:
    case AggKind::kCount:
    case AggKind::kCountDistinct:
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

std::string ToString(const Constant& constant) {
  if (const auto* v = std::get_if<Int128>(&constant.value)) {
    if (constant.type == LogicalType::kTimestamp) {
      const auto micros = Int128ToInt64(*v);
      return micros.has_value() ? "TIMESTAMP '" + FormatTimestamp(*micros) + "'"
                                : "TIMESTAMP <" + Int128ToString(*v) + " microseconds>";
    }
    if (constant.type != LogicalType::kDate) {
      return Int128ToString(*v);
    }
    const auto days = NarrowInt128<int32_t>(*v);
    return days.has_value() ? "DATE '" + FormatDate(*days) + "'"
                            : "DATE <" + Int128ToString(*v) + " days>";
  }
  if (const auto* d = std::get_if<double>(&constant.value)) {
    return std::format("{}", *d);
  }
  return "'" + EscapeText(std::get<std::string>(constant.value), '\'') + "'";
}

arrow::Result<std::shared_ptr<arrow::Scalar>> ToArrowScalar(const Constant& constant) {
  const LogicalType type = constant.type;
  if (const auto* v = std::get_if<Int128>(&constant.value)) {
    switch (type.id()) {
      case LogicalType::kSmallInt:
        return IntegerScalar<arrow::Int16Scalar, int16_t>(*v, type);
      case LogicalType::kInteger:
        return IntegerScalar<arrow::Int32Scalar, int32_t>(*v, type);
      case LogicalType::kBigInt:
        return IntegerScalar<arrow::Int64Scalar, int64_t>(*v, type);
      case LogicalType::kUSmallInt:
        return IntegerScalar<arrow::UInt16Scalar, uint16_t>(*v, type);
      case LogicalType::kDate:
        return IntegerScalar<arrow::Date32Scalar, int32_t>(*v, type);
      case LogicalType::kHugeInt:
        return HugeIntScalar(*v);
      case LogicalType::kTimestamp: {
        const auto micros = Int128ToInt64(*v);
        if (!micros.has_value()) {
          break;
        }
        return std::make_shared<arrow::TimestampScalar>(*micros, arrow::TimeUnit::MICRO);
      }
      case LogicalType::kDouble:
      case LogicalType::kVarchar:
      case LogicalType::kBoolean:
        break;
    }
  } else if (const auto* d = std::get_if<double>(&constant.value)) {
    if (type == LogicalType::kDouble) {
      return std::make_shared<arrow::DoubleScalar>(*d);
    }
  } else if (type == LogicalType::kVarchar) {
    return std::make_shared<arrow::BinaryScalar>(
        arrow::Buffer::FromString(std::get<std::string>(constant.value)));
  }
  return arrow::Status::Invalid("a ", ToString(type), " constant holds a value of another type");
}

std::string_view NodeName(const LogicalNode& node) { return std::visit(NodeNameOf{}, node); }

SourceSpan SpanOf(const LogicalNode& node) {
  return std::visit([](const auto& n) { return n.span; }, node);
}

const LogicalNodePtr* InputOf(const LogicalNode& node) { return std::visit(InputOfNode{}, node); }

}  // namespace antb1::plan
