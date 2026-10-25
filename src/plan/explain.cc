#include "antb1/plan/explain.h"

#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include <arrow/type.h>

#include "antb1/plan/catalog.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/shared_sub_plans.h"
#include "antb1/plan/types.h"

#include "aggregate_placement.h"

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

// A column as qualifier.name, or its name alone without a qualifier.
std::string ColumnName(const BoundColumn& column) {
  if (column.qualifier.empty()) {
    return Name(column.name);
  }
  return Name(column.qualifier) + "." + Name(column.name);
}

// A DECIMAL column compared with a DOUBLE constant (or IN list) compares in DOUBLE (ADR 0021 rule
// 11), which the column name alone would not show: `CAST(p AS DOUBLE) > 10`.
bool ComparedInDouble(const Predicate& p) {
  if (!p.column.has_value() || p.column->type != LogicalType::kDecimal) {
    return false;
  }
  if (p.kind == Predicate::Kind::kCompare) {
    return p.constant.type == LogicalType::kDouble;
  }
  return (p.kind == Predicate::Kind::kIn || p.kind == Predicate::Kind::kNotIn) &&
         !p.values.empty() && p.values.front().type == LogicalType::kDouble;
}

std::string PredicateText(const Predicate& p) {
  std::string column = p.column.has_value() ? ColumnName(*p.column) : "?";
  if (ComparedInDouble(p)) {
    column = "CAST(" + column + " AS DOUBLE)";
  }
  switch (p.kind) {
    case Predicate::Kind::kCompare:
      return std::format("{} {} {}", column, ToString(p.op), ToString(p.constant));
    case Predicate::Kind::kCompareColumns:
      return std::format("{} {} {}", column, ToString(p.op),
                         p.other.has_value() ? ColumnName(*p.other) : "?");
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
                     call.kind == AggKind::kCountDistinct ? "DISTINCT " : "",
                     ColumnName(*call.arg));
}

std::string SortKeyText(const SortKey& key) {
  return std::format("{} {} {}", ColumnName(key.column), key.descending ? "DESC" : "ASC",
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
  std::string operator()(const JoinNode& node) const {
    std::string line = std::format(
        "Join {} build={} keys=[{}]", ToString(node.kind), ToString(node.build),
        Join(
            node.keys,
            [](const JoinKey& key) { return ColumnName(key.left) + " = " + ColumnName(key.right); },
            ", "));
    if (!node.residual.empty()) {
      line += " residual=[" +
              Join(
                  node.residual, [](const ExprPtr& e) { return EscapeText(e->name, '\0'); }, ", ") +
              "]";
    }
    // The columns this join carries that nothing above it reads -- its inputs produce them, it
    // gathers them into every output batch, and a build on that side holds them for its lifetime.
    // Reported rather than acted on: `needed_above` is Prune's record of what is read above, so an
    // empty one means Prune has not run (a hand-built plan, or one rendered before the pass) and
    // the count would be the whole output rather than nothing.
    if (!node.needed_above.empty()) {
      const std::vector<ColumnId> out = OutputIds(LogicalNode(node));
      const auto dead =
          static_cast<std::size_t>(std::ranges::count_if(out, [&node](const ColumnId id) {
            return !std::ranges::binary_search(node.needed_above, id);
          }));
      if (dead > 0) {
        line += std::format(" carries {} of {} unread above", dead, out.size());
      }
    }
    return line;
  }
  std::string operator()(const DependentJoinNode& node) const {
    // No build= and no keys=: a dependent join has neither (ADR 0023). Only the plan tests see
    // this line, since Optimize replaces every dependent join or rejects the query.
    std::string line = std::format("DependentJoin {}", ToString(node.kind));
    if (node.mark != kNoColumnId) {
      line += std::format(" mark=#{}", std::to_underlying(node.mark));
    }
    return line;
  }
};

// Per node that heads a group of equivalent sub-plans, the suffix its line carries.
using SharedNotes = std::unordered_map<const LogicalNode*, std::string>;

// A note on every member of every group, numbered by the order the groups appear, so that a reader
// can see that two subtrees compute the same rows and what one shared sub-plan would read
// (docs/adr/0030-shared-sub-plans.md, E1). Nothing shares anything yet: this reports.
SharedNotes NotesOf(const LogicalPlan& plan) {
  SharedNotes notes;
  int group = 0;
  for (const SharedSubPlan& shared : SharedSubPlans(plan)) {
    ++group;
    for (const LogicalNodePtr& member : shared.members) {
      notes[member.get()] =
          std::format("  [shared #{} of {}, {} field{}]", group, shared.members.size(),
                      shared.fields, shared.fields == 1 ? "" : "s");
    }
  }
  // A note on every grouped aggregate that could be computed on one input of the join it reads
  // (docs/adr/0032-aggregate-placement.md). `Optimize` performs the ones the gate calls worth, so
  // on an optimized plan this reports what is LEFT -- which is why a note with a refusing verdict
  // can sit above a partial the exchange already built: it says that nothing more is worth doing
  // there, and the verdict says why.
  for (const AggregatePlacement& placement : AggregatePlacements(plan)) {
    std::string note =
        std::format("  [groupable below the {} join, {} key{}",
                    placement.left_is_aggregated ? "left of" : "right of",
                    placement.pre_grouping.size(), placement.pre_grouping.size() == 1 ? "" : "s");
    if (placement.grouping_from_aggregated_side > 0) {
      note += std::format(", {} from the grouping set", placement.grouping_from_aggregated_side);
    }
    if (placement.joins_down > 1) {
      // Only below the nearest join, so output moves only where the chosen placement is deeper.
      note += std::format(", {} joins down", placement.joins_down);
    }
    if (placement.needs_identity) {
      note += ", padding";
    }
    note += std::format(", {}", ToString(placement.verdict));
    if (placement.rows_below.has_value() && placement.groups.has_value()) {
      note += std::format(" {}->{}", *placement.rows_below, *placement.groups);
    }
    // Appended, not assigned: a node can be both a member of a shared group and a placement, and
    // ADR 0030's E1 promises a note on every member of every group.
    notes[placement.aggregate.get()] += note + "]";
  }
  return notes;
}

void Render(const LogicalNode& node, std::size_t depth, const SharedNotes& notes,
            std::string& out) {
  out += std::string(2 * depth, ' ') + std::visit(NodeLine{}, node);
  if (const auto note = notes.find(&node); note != notes.end()) {
    out += note->second;
  }
  out += '\n';
  for (const LogicalNodePtr& input : InputsOf(node)) {  // a Join's left input, then its right one
    if (input != nullptr) {
      Render(*input, depth + 1, notes, out);
    }
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
    Render(*plan.root, 0, NotesOf(plan), out);
  }
  return out;
}

}  // namespace antb1::plan
