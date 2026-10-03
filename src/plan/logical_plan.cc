#include "antb1/plan/logical_plan.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <source_location>
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

// A HUGEINT, or a DECIMAL(p, s) from its unscaled value: a decimal128 scalar of the type's own
// precision and scale.
arrow::Result<std::shared_ptr<arrow::Scalar>> Decimal128Scalar(Int128 value, LogicalType type) {
  const IntegerRange range = RangeOf(type);
  if (value < range.min || value > range.max) {
    return arrow::Status::Invalid("constant ", Int128ToString(value), " is outside the range of ",
                                  ToString(type));
  }
  const auto bits = static_cast<UInt128>(value);
  const arrow::Decimal128 decimal(static_cast<int64_t>(bits >> 64U), static_cast<uint64_t>(bits));
  return std::make_shared<arrow::Decimal128Scalar>(decimal, ToArrow(type));
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
  std::string_view operator()(const JoinNode& /*node*/) const { return "Join"; }
};

struct InputsOfNode {
  std::vector<LogicalNodePtr> operator()(const ScanNode& /*node*/) const { return {}; }
  std::vector<LogicalNodePtr> operator()(const FilterNode& node) const { return {node.input}; }
  std::vector<LogicalNodePtr> operator()(const ComputeNode& node) const { return {node.input}; }
  std::vector<LogicalNodePtr> operator()(const ProjectNode& node) const { return {node.input}; }
  std::vector<LogicalNodePtr> operator()(const AggregateNode& node) const { return {node.input}; }
  std::vector<LogicalNodePtr> operator()(const GroupAggregateNode& node) const {
    return {node.input};
  }
  std::vector<LogicalNodePtr> operator()(const SortNode& node) const { return {node.input}; }
  std::vector<LogicalNodePtr> operator()(const LimitNode& node) const { return {node.input}; }
  std::vector<LogicalNodePtr> operator()(const RowCountNode& /*node*/) const { return {}; }
  std::vector<LogicalNodePtr> operator()(const JoinNode& node) const {
    return {node.left, node.right};
  }
};

// A copy of a node over `inputs` (as many as InputsOfNode returns).
struct WithInputsOfNode {
  std::vector<LogicalNodePtr>& inputs;

  template <class Node>
  LogicalNodePtr Single(Node node) const {
    node.input = std::move(inputs[0]);
    return std::make_shared<const LogicalNode>(std::move(node));
  }

  LogicalNodePtr operator()(const ScanNode& node) const {
    return std::make_shared<const LogicalNode>(node);
  }
  LogicalNodePtr operator()(const FilterNode& node) const { return Single(node); }
  LogicalNodePtr operator()(const ComputeNode& node) const { return Single(node); }
  LogicalNodePtr operator()(const ProjectNode& node) const { return Single(node); }
  LogicalNodePtr operator()(const AggregateNode& node) const { return Single(node); }
  LogicalNodePtr operator()(const GroupAggregateNode& node) const { return Single(node); }
  LogicalNodePtr operator()(const SortNode& node) const { return Single(node); }
  LogicalNodePtr operator()(const LimitNode& node) const { return Single(node); }
  LogicalNodePtr operator()(const RowCountNode& node) const {
    return std::make_shared<const LogicalNode>(node);
  }
  LogicalNodePtr operator()(const JoinNode& node) const {
    JoinNode out = node;
    out.left = std::move(inputs[0]);
    out.right = std::move(inputs[1]);
    return std::make_shared<const LogicalNode>(std::move(out));
  }
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
    const auto& b = std::get<ColumnExpr>(other.node);
    return a.id == b.id && (a.id != kNoColumnId || a.index == b.index);
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

ExprPtr MapColumns(const ExprPtr& expr, const std::function<ColumnExpr(const ColumnExpr&)>& f) {
  Expr out = *expr;
  bool changed = false;
  if (auto* column = std::get_if<ColumnExpr>(&out.node)) {
    const ColumnExpr mapped = f(*column);
    changed = mapped.index != column->index || mapped.id != column->id;
    *column = mapped;
  }
  for (ExprPtr* child : Children(out)) {
    if (*child == nullptr) {
      continue;
    }
    ExprPtr mapped = MapColumns(*child, f);
    if (mapped != *child) {
      *child = std::move(mapped);
      changed = true;
    }
  }
  return changed ? std::make_shared<const Expr>(std::move(out)) : expr;
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

void CollectColumnIds(const Expr& expr, std::vector<ColumnId>& out) {
  if (const auto* column = std::get_if<ColumnExpr>(&expr.node)) {
    out.push_back(column->id);
  }
  Expr copy = expr;  // Children takes a mutable node; the copy shares the children
  for (const ExprPtr* child : Children(copy)) {
    if (*child != nullptr) {
      CollectColumnIds(**child, out);
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

std::string_view ToString(JoinKind kind) {
  switch (kind) {
    case JoinKind::kInner:
      return "INNER";
    case JoinKind::kLeft:
      return "LEFT";
    case JoinKind::kSemi:
      return "SEMI";
    case JoinKind::kAnti:
      return "ANTI";
    case JoinKind::kNullAwareAnti:
      return "NULL-AWARE ANTI";
    case JoinKind::kOneRow:
      return "ONE-ROW";
  }
  return "?";
}

std::string_view ToString(BuildSide side) {
  switch (side) {
    case BuildSide::kLeft:
      return "left";
    case BuildSide::kRight:
      return "right";
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
    if (constant.type == LogicalType::kDecimal) {
      return FormatDecimal(*v, constant.type.width(), constant.type.scale());
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
      case LogicalType::kDecimal:
        return Decimal128Scalar(*v, type);
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

std::vector<LogicalNodePtr> InputsOf(const LogicalNode& node) {
  return std::visit(InputsOfNode{}, node);
}

LogicalNodePtr WithInputs(const LogicalNode& node, std::vector<LogicalNodePtr> inputs) {
  ANTB1_CHECK(inputs.size() == InputsOf(node).size());
  return std::visit(WithInputsOfNode{.inputs = inputs}, node);
}

namespace {

std::vector<ColumnId> CallIds(std::vector<ColumnId> ids, const std::vector<AggregateCall>& calls) {
  ids.reserve(ids.size() + calls.size());
  for (const AggregateCall& call : calls) {
    ids.push_back(call.id);
  }
  return ids;
}

// The output ids of a join of `kind` over inputs with these output ids.
std::vector<ColumnId> JoinIds(JoinKind kind, std::vector<ColumnId> left,
                              const std::vector<ColumnId>& right) {
  switch (kind) {
    case JoinKind::kInner:
    case JoinKind::kLeft:
    case JoinKind::kOneRow:
      left.insert(left.end(), right.begin(), right.end());
      return left;
    case JoinKind::kSemi:
    case JoinKind::kAnti:
    case JoinKind::kNullAwareAnti:
      return left;
  }
  return left;
}

struct OutputIdsOf {
  std::vector<ColumnId> operator()(const ScanNode& node) const { return node.ids; }
  std::vector<ColumnId> operator()(const FilterNode& node) const { return OutputIds(*node.input); }
  std::vector<ColumnId> operator()(const ComputeNode& node) const {
    std::vector<ColumnId> ids = OutputIds(*node.input);
    ids.insert(ids.end(), node.ids.begin(), node.ids.end());
    return ids;
  }
  std::vector<ColumnId> operator()(const ProjectNode& node) const { return node.ids; }
  std::vector<ColumnId> operator()(const AggregateNode& node) const {
    return CallIds({}, node.aggregates);
  }
  std::vector<ColumnId> operator()(const GroupAggregateNode& node) const {
    return CallIds(node.key_ids, node.aggregates);
  }
  std::vector<ColumnId> operator()(const SortNode& node) const { return OutputIds(*node.input); }
  std::vector<ColumnId> operator()(const LimitNode& node) const { return OutputIds(*node.input); }
  std::vector<ColumnId> operator()(const RowCountNode& node) const { return {node.id}; }
  std::vector<ColumnId> operator()(const JoinNode& node) const {
    return JoinIds(node.kind, OutputIds(*node.left), OutputIds(*node.right));
  }
};

std::string IdText(ColumnId id) { return std::format("#{}", std::to_underlying(id)); }

template <class Node>
LogicalNodePtr MakeNode(Node node) {
  return std::make_shared<const LogicalNode>(std::move(node));
}

// The walk of ResolvePositions and PositionMismatch: the plan rebuilt bottom-up with every index
// set from the ids, each node with its output ids. It remembers the first broken invariant and the
// first index that differs from its resolved position; its messages name node kinds, ids and
// positions only (they reach the logs of the data tests, which never show data).
class Resolver {
 public:
  struct Resolved {
    LogicalNodePtr node;        // the same pointer when nothing changed
    std::vector<ColumnId> ids;  // its output columns
  };

  // One overload of Visit per node type: a node type without one fails to compile.
  Resolved Walk(const LogicalNodePtr& node) {
    return std::visit([&](const auto& n) { return Visit(node, n); }, *node);
  }

  void CheckOutput(const std::vector<OutputColumn>& output, const std::vector<ColumnId>& ids) {
    if (output.size() != ids.size()) {
      Fail(std::format("output: {} columns, but the root outputs {}", output.size(), ids.size()));
      return;
    }
    for (std::size_t i = 0; i < output.size(); ++i) {
      if (output[i].id != ids[i]) {
        Fail(std::format("output: column {} is {}, but the root outputs {}", i,
                         IdText(output[i].id), IdText(ids[i])));
        return;
      }
    }
  }

  [[nodiscard]] const std::optional<std::string>& failure() const { return failure_; }

  // The first broken invariant, else the first index that differs from its resolved position.
  [[nodiscard]] std::optional<std::string> Problem() const {
    return failure_.has_value() ? failure_ : difference_;
  }

 private:
  void Fail(std::string message) {
    if (!failure_.has_value()) {
      failure_ = std::move(message);
    }
  }

  // A column created by `node`: it has an id that no other column of the plan has.
  void Define(std::string_view node, ColumnId id) {
    if (id == kNoColumnId) {
      Fail(std::format("{}: a column without an id", node));
    } else if (!defined_.insert(id).second) {
      Fail(std::format("{}: column {} is defined twice", node, IdText(id)));
    }
  }

  void CheckSize(std::string_view node, std::string_view what, std::size_t ids,
                 std::size_t columns) {
    if (ids != columns) {
      Fail(std::format("{}: {} ids for {} {}", node, ids, columns, what));
    }
  }

  // Sets `index` to the position of `id` among `ids`; true when it changed.
  bool Resolve(std::string_view node, int& index, ColumnId id, const std::vector<ColumnId>& ids) {
    if (id == kNoColumnId) {
      Fail(std::format("{}: a reference without an id", node));
      return false;
    }
    const auto first = std::ranges::find(ids, id);
    if (first == ids.end()) {
      Fail(std::format("{}: column {} is not in its input", node, IdText(id)));
      return false;
    }
    if (std::ranges::find(std::next(first), ids.end(), id) != ids.end()) {
      Fail(std::format("{}: column {} is in its input twice", node, IdText(id)));
      return false;
    }
    const auto position = Narrow<int>(first - ids.begin());
    if (position == index) {
      return false;
    }
    if (!difference_.has_value()) {
      difference_ =
          std::format("{}: column {} is at {}, not {}", node, IdText(id), position, index);
    }
    index = position;
    return true;
  }

  bool Resolve(std::string_view node, BoundColumn& column, const std::vector<ColumnId>& ids) {
    return Resolve(node, column.index, column.id, ids);
  }

  bool Resolve(std::string_view node, std::optional<BoundColumn>& column,
               const std::vector<ColumnId>& ids) {
    return column.has_value() && Resolve(node, *column, ids);
  }

  Resolved Visit(const LogicalNodePtr& self, const ScanNode& scan) {
    CheckSize("Scan", "fields", scan.ids.size(), scan.fields.size());
    for (const ColumnId id : scan.ids) {
      Define("Scan", id);
    }
    return {.node = self, .ids = scan.ids};
  }

  Resolved Visit(const LogicalNodePtr& self, const FilterNode& filter) {
    Resolved input = Walk(filter.input);
    FilterNode out = filter;
    bool changed = input.node != filter.input;
    out.input = input.node;
    for (Predicate& predicate : out.predicates) {
      if (Resolve("Filter", predicate.column, input.ids)) {
        changed = true;
      }
      if (Resolve("Filter", predicate.other, input.ids)) {
        changed = true;
      }
    }
    return {.node = changed ? MakeNode(std::move(out)) : self, .ids = std::move(input.ids)};
  }

  Resolved Visit(const LogicalNodePtr& self, const ComputeNode& compute) {
    Resolved input = Walk(compute.input);
    CheckSize("Compute", "expressions", compute.ids.size(), compute.exprs.size());
    ComputeNode out = compute;
    bool changed = input.node != compute.input;
    out.input = input.node;
    for (ExprPtr& expr : out.exprs) {
      ExprPtr resolved = MapColumns(expr, [&](const ColumnExpr& column) {
        ColumnExpr leaf = column;
        Resolve("Compute", leaf.index, leaf.id, input.ids);
        return leaf;
      });
      if (resolved != expr) {
        expr = std::move(resolved);
        changed = true;
      }
    }
    for (const ColumnId id : compute.ids) {
      Define("Compute", id);
    }
    std::vector<ColumnId> ids = std::move(input.ids);
    ids.insert(ids.end(), compute.ids.begin(), compute.ids.end());
    return {.node = changed ? MakeNode(std::move(out)) : self, .ids = std::move(ids)};
  }

  Resolved Visit(const LogicalNodePtr& self, const ProjectNode& project) {
    Resolved input = Walk(project.input);
    CheckSize("Project", "columns", project.ids.size(), project.columns.size());
    ProjectNode out = project;
    bool changed = input.node != project.input;
    out.input = input.node;
    for (std::size_t i = 0; i < out.columns.size(); ++i) {
      const bool constant = i < out.constants.size() && out.constants[i].has_value();
      if (!constant && Resolve("Project", out.columns[i], input.ids)) {
        changed = true;
      }
      // A column that keeps the id it reads passes it through; any other is a new column.
      if (i < project.ids.size() &&
          (project.ids[i] != project.columns[i].id || project.ids[i] == kNoColumnId)) {
        Define("Project", project.ids[i]);
      }
    }
    return {.node = changed ? MakeNode(std::move(out)) : self, .ids = project.ids};
  }

  Resolved Visit(const LogicalNodePtr& self, const AggregateNode& aggregate) {
    Resolved input = Walk(aggregate.input);
    AggregateNode out = aggregate;
    bool changed = input.node != aggregate.input;
    out.input = input.node;
    for (AggregateCall& call : out.aggregates) {
      if (Resolve("Aggregate", call.arg, input.ids)) {
        changed = true;
      }
      Define("Aggregate", call.id);
    }
    return {.node = changed ? MakeNode(std::move(out)) : self,
            .ids = CallIds({}, aggregate.aggregates)};
  }

  Resolved Visit(const LogicalNodePtr& self, const GroupAggregateNode& group) {
    Resolved input = Walk(group.input);
    CheckSize("GroupAggregate", "keys", group.key_ids.size(), group.keys.size());
    GroupAggregateNode out = group;
    bool changed = input.node != group.input;
    out.input = input.node;
    for (BoundColumn& key : out.keys) {
      if (Resolve("GroupAggregate", key, input.ids)) {
        changed = true;
      }
    }
    for (const ColumnId id : group.key_ids) {
      Define("GroupAggregate", id);
    }
    for (AggregateCall& call : out.aggregates) {
      if (Resolve("GroupAggregate", call.arg, input.ids)) {
        changed = true;
      }
      Define("GroupAggregate", call.id);
    }
    return {.node = changed ? MakeNode(std::move(out)) : self,
            .ids = CallIds(group.key_ids, group.aggregates)};
  }

  Resolved Visit(const LogicalNodePtr& self, const SortNode& sort) {
    Resolved input = Walk(sort.input);
    SortNode out = sort;
    bool changed = input.node != sort.input;
    out.input = input.node;
    for (SortKey& key : out.keys) {
      if (Resolve("Sort", key.column, input.ids)) {
        changed = true;
      }
    }
    return {.node = changed ? MakeNode(std::move(out)) : self, .ids = std::move(input.ids)};
  }

  Resolved Visit(const LogicalNodePtr& self, const LimitNode& limit) {
    Resolved input = Walk(limit.input);
    if (input.node == limit.input) {
      return {.node = self, .ids = std::move(input.ids)};
    }
    LimitNode out = limit;
    out.input = input.node;
    return {.node = MakeNode(std::move(out)), .ids = std::move(input.ids)};
  }

  Resolved Visit(const LogicalNodePtr& self, const RowCountNode& count) {
    Define("RowCount", count.id);
    return {.node = self, .ids = {count.id}};
  }

  // Keys resolve against their own input, residual columns against the left input's ids, then
  // the right input's.
  Resolved Visit(const LogicalNodePtr& self, const JoinNode& join) {
    Resolved left = Walk(join.left);
    Resolved right = Walk(join.right);
    const bool one_row = join.kind == JoinKind::kOneRow;
    if (one_row && !join.keys.empty()) {
      Fail(std::format("Join: {} join with {} keys", ToString(join.kind), join.keys.size()));
    } else if (!one_row && join.keys.empty()) {
      Fail(std::format("Join: {} join without keys", ToString(join.kind)));
    }
    if (join.build == BuildSide::kLeft && join.kind != JoinKind::kInner) {
      Fail(std::format("Join: {} join builds on its left input", ToString(join.kind)));
    }
    JoinNode out = join;
    bool changed = left.node != join.left || right.node != join.right;
    out.left = left.node;
    out.right = right.node;
    for (JoinKey& key : out.keys) {
      if (key.left.type != key.right.type) {
        Fail(std::format("Join: key {} = {} has two types, {} and {}", IdText(key.left.id),
                         IdText(key.right.id), ToString(key.left.type), ToString(key.right.type)));
      }
      if (Resolve("Join", key.left, left.ids)) {
        changed = true;
      }
      if (Resolve("Join", key.right, right.ids)) {
        changed = true;
      }
    }
    std::vector<ColumnId> both = left.ids;
    both.insert(both.end(), right.ids.begin(), right.ids.end());
    for (ExprPtr& conjunct : out.residual) {
      if (conjunct->type != LogicalType::kBoolean) {
        Fail(std::format("Join: a residual of type {}", ToString(conjunct->type)));
      }
      ExprPtr resolved = MapColumns(conjunct, [&](const ColumnExpr& column) {
        ColumnExpr leaf = column;
        Resolve("Join", leaf.index, leaf.id, both);
        return leaf;
      });
      if (resolved != conjunct) {
        conjunct = std::move(resolved);
        changed = true;
      }
    }
    return {.node = changed ? MakeNode(std::move(out)) : self,
            .ids = JoinIds(join.kind, std::move(left.ids), right.ids)};
  }

  std::set<ColumnId> defined_;
  std::optional<std::string> failure_;
  std::optional<std::string> difference_;
};

}  // namespace

std::vector<ColumnId> OutputIds(const LogicalNode& node) { return std::visit(OutputIdsOf{}, node); }

LogicalPlan ResolvePositions(const LogicalPlan& plan, std::source_location location) {
  if (plan.root == nullptr) {
    return plan;
  }
  Resolver resolver;
  Resolver::Resolved root = resolver.Walk(plan.root);
  resolver.CheckOutput(plan.output, root.ids);
  if (resolver.failure().has_value()) {
    // A programming error: see PositionMismatch.
    internal::CheckFailed(resolver.failure()->c_str(), location);
  }
  return LogicalPlan{.root = std::move(root.node), .output = plan.output};
}

std::optional<std::string> PositionMismatch(const LogicalPlan& plan) {
  if (plan.root == nullptr) {
    return std::nullopt;
  }
  Resolver resolver;
  const Resolver::Resolved root = resolver.Walk(plan.root);
  resolver.CheckOutput(plan.output, root.ids);
  return resolver.Problem();
}

}  // namespace antb1::plan
