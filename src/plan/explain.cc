#include "antb1/plan/explain.h"

#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <arrow/type.h>

#include "antb1/plan/catalog.h"
#include "antb1/plan/logical_plan.h"

namespace antb1::plan {
namespace {

// A column name as is when it is a plain identifier, otherwise "double-quoted".
std::string Name(std::string_view name) {
  if (IsPlainIdentifier(name)) {
    return std::string(name);
  }
  return "\"" + EscapeText(name, '"') + "\"";
}

template <class T, class F>
std::string Join(const std::vector<T>& items, const F& render, std::string_view separator) {
  std::string out;
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i > 0) {
      out += separator;
    }
    out += render(items[i]);
  }
  return out;
}

std::string PredicateText(const Predicate& p) {
  std::string column = p.column.has_value() ? Name(p.column->name) : "?";
  switch (p.kind) {
    case Predicate::Kind::kCompare:
      return std::format("{} {} {}", column, ToString(p.op), ToString(p.constant));
    case Predicate::Kind::kCompareColumns:
      return std::format("{} {} {}", column, ToString(p.op),
                         p.other.has_value() ? Name(p.other->name) : "?");
    case Predicate::Kind::kLike:
      return std::format("{} LIKE {}", column, ToString(p.constant));
    case Predicate::Kind::kNotLike:
      return std::format("{} NOT LIKE {}", column, ToString(p.constant));
    case Predicate::Kind::kIn:
    case Predicate::Kind::kNotIn: {
      std::string values;
      for (const Constant& value : p.values) {
        values += (values.empty() ? "" : ", ") + ToString(value);
      }
      return std::format("{} {}IN ({})", column, p.kind == Predicate::Kind::kNotIn ? "NOT " : "",
                         values);
    }
    case Predicate::Kind::kIsNotNull:
      return column + " IS NOT NULL";
    case Predicate::Kind::kIsTrue:
      return column;
    case Predicate::Kind::kFalse:
      break;
  }
  return "FALSE";
}

std::string AggregateText(const AggregateCall& call) {
  if (!call.arg.has_value()) {
    return "COUNT(*)";
  }
  return std::format("{}({}{})", ToString(call.kind),
                     call.kind == AggKind::kCountDistinct ? "DISTINCT " : "", Name(call.arg->name));
}

std::string ColumnName(const BoundColumn& column) { return Name(column.name); }

std::string SortKeyText(const SortKey& key) {
  return std::format("{} {} {}", Name(key.column.name), key.descending ? "DESC" : "ASC",
                     key.nulls_first ? "NULLS FIRST" : "NULLS LAST");
}

// The line of one node (without its input). One overload per node type: a node type without one
// fails to compile.
struct NodeLine {
  std::string operator()(const ScanNode& node) const {
    const auto& schema = *node.table->schema();
    const std::string columns =
        Join(node.fields, [&schema](int f) { return Name(schema.field(f)->name()); }, ", ");
    return std::format("Scan table={} source={} columns=[{}]", EscapeText(node.table_name, '\0'),
                       node.table->Describe(), columns);
  }
  std::string operator()(const FilterNode& node) const {
    return "Filter " + Join(node.predicates, PredicateText, " AND ");
  }
  std::string operator()(const ComputeNode& node) const {
    return "Compute " +
           Join(node.exprs, [](const ExprPtr& e) { return EscapeText(e->name, '\0'); }, ", ");
  }
  std::string operator()(const ProjectNode& node) const {
    std::string line = "Project ";
    for (std::size_t i = 0; i < node.columns.size(); ++i) {
      line += i == 0 ? "" : ", ";
      const std::optional<Constant> constant =
          node.constants.empty() ? std::nullopt : node.constants[i];
      line += constant.has_value() ? ToString(*constant) : ColumnName(node.columns[i]);
    }
    return line;
  }
  std::string operator()(const AggregateNode& node) const {
    return "Aggregate " + Join(node.aggregates, AggregateText, ", ");
  }
  std::string operator()(const GroupAggregateNode& node) const {
    std::string line = "GroupAggregate keys=[" + Join(node.keys, ColumnName, ", ") + "]";
    if (!node.aggregates.empty()) {
      line += " " + Join(node.aggregates, AggregateText, ", ");
    }
    return line;
  }
  std::string operator()(const SortNode& node) const {
    return "Sort " + Join(node.keys, SortKeyText, ", ");
  }
  std::string operator()(const LimitNode& node) const {
    std::string line =
        node.limit.has_value() ? std::format("Limit {}", *node.limit) : std::string("Limit ALL");
    if (node.offset > 0) {
      line += std::format(" OFFSET {}", node.offset);
    }
    return line;
  }
  std::string operator()(const RowCountNode& node) const {
    return std::format("RowCount table={} source={}", EscapeText(node.table_name, '\0'),
                       node.table->Describe());
  }
};

void Render(const LogicalNode& node, std::size_t depth, std::string& out) {
  out += std::string(2 * depth, ' ') + std::visit(NodeLine{}, node) + '\n';
  if (const LogicalNodePtr* input = InputOf(node); input != nullptr && *input != nullptr) {
    Render(**input, depth + 1, out);
  }
}

}  // namespace

std::string ExplainNode(const LogicalNode& node) { return std::visit(NodeLine{}, node); }

std::string ExplainOutput(const LogicalPlan& plan) {
  std::string out = "Output:";
  for (const auto& col : plan.output) {
    out += std::format(" {}:{}", EscapeText(col.name, '\0'), ToString(col.type));
  }
  return out;
}

std::string Explain(const LogicalPlan& plan) {
  std::string out = ExplainOutput(plan) + '\n';
  if (plan.root != nullptr) {
    Render(*plan.root, 0, out);
  }
  return out;
}

}  // namespace antb1::plan
