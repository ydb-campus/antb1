#include "antb1/plan/logical_plan.h"

#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include <arrow/api.h>

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
  std::string_view operator()(const ProjectNode& /*node*/) const { return "Project"; }
  std::string_view operator()(const AggregateNode& /*node*/) const { return "Aggregate"; }
  std::string_view operator()(const LimitNode& /*node*/) const { return "Limit"; }
  std::string_view operator()(const RowCountNode& /*node*/) const { return "RowCount"; }
};

struct InputOfNode {
  const LogicalNodePtr* operator()(const ScanNode& /*node*/) const { return nullptr; }
  const LogicalNodePtr* operator()(const FilterNode& node) const { return &node.input; }
  const LogicalNodePtr* operator()(const ProjectNode& node) const { return &node.input; }
  const LogicalNodePtr* operator()(const AggregateNode& node) const { return &node.input; }
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

std::string ToString(const Constant& constant) {
  if (const auto* v = std::get_if<Int128>(&constant.value)) {
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
    switch (type) {
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
      case LogicalType::kDouble:
      case LogicalType::kVarchar:
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
