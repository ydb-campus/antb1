#include "antb1/plan/optimizer.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "antb1/common/check.h"
#include "antb1/common/narrow.h"
#include "antb1/plan/logical_plan.h"

// Every std::visit below uses a visitor with one overload per node type, so a new node type fails
// to compile until each rule handles it.

namespace antb1::plan {
namespace {

LogicalNodePtr Make(LogicalNode node) {
  return std::make_shared<const LogicalNode>(std::move(node));
}

// A copy of a node over another input (leaves are copied unchanged).
struct WithInput {
  const LogicalNodePtr& input;

  template <class Node>
  LogicalNodePtr Replace(Node node) const {
    node.input = input;
    return Make(std::move(node));
  }

  LogicalNodePtr operator()(const ScanNode& node) const { return Make(node); }
  LogicalNodePtr operator()(const FilterNode& node) const { return Replace(node); }
  LogicalNodePtr operator()(const ComputeNode& node) const { return Replace(node); }
  LogicalNodePtr operator()(const ProjectNode& node) const { return Replace(node); }
  LogicalNodePtr operator()(const AggregateNode& node) const { return Replace(node); }
  LogicalNodePtr operator()(const GroupAggregateNode& node) const { return Replace(node); }
  LogicalNodePtr operator()(const SortNode& node) const { return Replace(node); }
  LogicalNodePtr operator()(const LimitNode& node) const { return Replace(node); }
  LogicalNodePtr operator()(const RowCountNode& node) const { return Make(node); }
};

std::size_t OutputWidth(const LogicalNode& node);

struct OutputWidthOf {
  std::size_t operator()(const ScanNode& node) const { return node.fields.size(); }
  std::size_t operator()(const FilterNode& node) const { return OutputWidth(*node.input); }
  std::size_t operator()(const ComputeNode& node) const {
    return OutputWidth(*node.input) + node.exprs.size();
  }
  std::size_t operator()(const ProjectNode& node) const { return node.columns.size(); }
  std::size_t operator()(const AggregateNode& node) const { return node.aggregates.size(); }
  std::size_t operator()(const GroupAggregateNode& node) const {
    return node.keys.size() + node.aggregates.size();
  }
  std::size_t operator()(const SortNode& node) const { return OutputWidth(*node.input); }
  std::size_t operator()(const LimitNode& node) const { return OutputWidth(*node.input); }
  std::size_t operator()(const RowCountNode& /*node*/) const { return 1; }
};

std::size_t OutputWidth(const LogicalNode& node) { return std::visit(OutputWidthOf{}, node); }

// ---- rule 1: COUNT(*) without WHERE -> RowCount ----

LogicalNodePtr CountStarToRowCount(const LogicalNodePtr& node) {
  if (const auto* agg = std::get_if<AggregateNode>(node.get())) {
    const auto* scan = std::get_if<ScanNode>(agg->input.get());
    if (scan != nullptr && agg->aggregates.size() == 1 &&
        agg->aggregates.front().kind == AggKind::kCountStar &&
        scan->table->exact_row_count().has_value()) {
      return Make(RowCountNode{.table = scan->table,
                               .table_name = scan->table_name,
                               .id = agg->aggregates.front().id,
                               .span = agg->aggregates.front().span});
    }
  }
  const LogicalNodePtr* input = InputOf(*node);
  if (input == nullptr || *input == nullptr) {
    return node;
  }
  LogicalNodePtr rewritten = CountStarToRowCount(*input);
  return rewritten == *input ? node : std::visit(WithInput{.input = rewritten}, *node);
}

// ---- rule 2: GROUP BY keys that are functions of other keys ----

// A key computed from other keys alone (docs/adr/0018-dependent-group-keys.md) cannot split or
// merge groups: GroupAggregate(k, f(k)) over Compute becomes
// Project(Compute(f)(GroupAggregate(k))), which gives the same rows in the same output order,
// hashes fewer keys and computes f once per group. A key is dependent when it is an expression of
// the Compute right below the GroupAggregate that reads at least one column, and every column it
// reads is a key passed through that Compute and not DOUBLE (a DOUBLE key groups -0.0 with 0.0 and
// every NaN together, which a function of it could tell apart). Nothing sits between the Compute
// and the GroupAggregate, so the expression sees the same values either way: an error (an overflow)
// happens for the same input.
// The name of an aggregate's output column, as EXPLAIN shows the call: COUNT(*), SUM(x).
std::string CallName(const AggregateCall& call) {
  if (!call.arg.has_value()) {
    return "COUNT(*)";
  }
  return std::format("{}({}{})", ToString(call.kind),
                     call.kind == AggKind::kCountDistinct ? "DISTINCT " : "", call.arg->name);
}

LogicalNodePtr GroupByDeterminingKeys(const GroupAggregateNode& group) {
  const auto* compute = std::get_if<ComputeNode>(group.input.get());
  if (compute == nullptr) {
    return nullptr;
  }
  const auto width = Narrow<int>(OutputWidth(*compute->input));
  // The keys each key position reads, and whether it is dependent.
  std::vector<bool> dependent(group.keys.size(), false);
  for (std::size_t k = 0; k < group.keys.size(); ++k) {
    const int index = group.keys[k].index;
    if (index < width) {
      continue;
    }
    const Expr& expr = *compute->exprs.at(Narrow<std::size_t>(index - width));
    std::vector<int> reads;
    CollectColumns(expr, reads);
    dependent[k] = !reads.empty() && std::ranges::all_of(reads, [&](int column) {
      return column < width && std::ranges::any_of(
                                   group.keys,
                                   [&](const BoundColumn& key) {
                                     return key.index == column && key.type != LogicalType::kDouble;
                                   });
    });
  }
  if (std::ranges::none_of(dependent, [](bool d) { return d; })) {
    return nullptr;
  }
  // The GroupAggregate by the determining keys; `position` and `id` map an input column that is a
  // kept key to its output position and column.
  GroupAggregateNode kept = group;
  kept.keys.clear();
  kept.key_ids.clear();
  std::vector<int> position(Narrow<std::size_t>(width), -1);
  std::vector<ColumnId> id(Narrow<std::size_t>(width), kNoColumnId);
  for (std::size_t k = 0; k < group.keys.size(); ++k) {
    if (!dependent[k]) {
      if (group.keys[k].index < width) {
        position[Narrow<std::size_t>(group.keys[k].index)] = Narrow<int>(kept.keys.size());
        id[Narrow<std::size_t>(group.keys[k].index)] = group.key_ids.at(k);
      }
      kept.keys.push_back(group.keys[k]);
      kept.key_ids.push_back(group.key_ids.at(k));
    }
  }
  const std::size_t kept_width = kept.keys.size() + kept.aggregates.size();
  // The dependent keys over its output, in key order, each the column it was as a key; the Project
  // passes every column through, so the nodes above read the same columns as before.
  ComputeNode above{.input = Make(std::move(kept)), .exprs = {}, .ids = {}, .span = compute->span};
  ProjectNode out{.input = nullptr, .columns = {}, .constants = {}, .ids = {}, .span = group.span};
  int next_kept = 0;
  for (std::size_t k = 0; k < group.keys.size(); ++k) {
    BoundColumn column = group.keys[k];
    column.id = group.key_ids.at(k);
    if (dependent[k]) {
      const ExprPtr& expr = compute->exprs.at(Narrow<std::size_t>(group.keys[k].index - width));
      column.index = Narrow<int>(kept_width + above.exprs.size());
      above.exprs.push_back(MapColumns(expr, [&](const ColumnExpr& read) {
        const auto at = Narrow<std::size_t>(read.index);
        ANTB1_CHECK(position.at(at) >= 0);
        return ColumnExpr{.index = position.at(at), .id = id.at(at)};
      }));
      above.ids.push_back(column.id);
    } else {
      column.index = next_kept++;
    }
    out.ids.push_back(column.id);
    out.columns.push_back(std::move(column));
  }
  for (std::size_t i = 0; i < group.aggregates.size(); ++i) {
    const AggregateCall& call = group.aggregates[i];
    out.columns.push_back(BoundColumn{.index = Narrow<int>(static_cast<std::size_t>(next_kept) + i),
                                      .id = call.id,
                                      .name = CallName(call),
                                      .type = call.type});
    out.ids.push_back(call.id);
  }
  out.input = Make(std::move(above));
  return Make(std::move(out));
}

// Rewrites every GroupAggregate whose whole output is read. Under a Limit with no Sort in between,
// the nodes above stop reading after the rows they need: a key computed above the GroupAggregate
// would then be computed for those groups only, and an error (an overflow) in another group would
// no longer fail the query, as it does when every row computes it. A Sort reads everything.
LogicalNodePtr DependentKeys(const LogicalNodePtr& node, bool limited) {
  if (std::holds_alternative<LimitNode>(*node)) {
    limited = true;
  } else if (std::holds_alternative<SortNode>(*node)) {
    limited = false;
  }
  const bool group = std::holds_alternative<GroupAggregateNode>(*node);
  const LogicalNodePtr* input = InputOf(*node);
  LogicalNodePtr current = node;
  if (input != nullptr && *input != nullptr) {
    LogicalNodePtr rewritten = DependentKeys(*input, limited && !group);
    if (rewritten != *input) {
      current = std::visit(WithInput{.input = rewritten}, *node);
    }
  }
  if (const auto* aggregate = std::get_if<GroupAggregateNode>(current.get());
      aggregate != nullptr && !limited) {
    if (LogicalNodePtr rewritten = GroupByDeterminingKeys(*aggregate); rewritten != nullptr) {
      return rewritten;
    }
  }
  return current;
}

// ---- rule 3: Limit below Project and Compute ----

// Limit(Project(x)) -> Project(Limit(x)), and likewise for Compute: both keep every row, so
// limiting first gives the same rows, copies or computes only the rows kept, and puts the Limit
// right above a Sort (top-N).
LogicalNodePtr LimitBelowProject(const LogicalNodePtr& node) {
  if (const auto* limit = std::get_if<LimitNode>(node.get())) {
    if (const auto* project = std::get_if<ProjectNode>(limit->input.get())) {
      LimitNode below = *limit;
      below.input = project->input;
      ProjectNode above = *project;
      above.input = LimitBelowProject(Make(std::move(below)));
      return Make(std::move(above));
    }
    if (const auto* compute = std::get_if<ComputeNode>(limit->input.get())) {
      LimitNode below = *limit;
      below.input = compute->input;
      ComputeNode above = *compute;
      above.input = LimitBelowProject(Make(std::move(below)));
      return Make(std::move(above));
    }
  }
  const LogicalNodePtr* input = InputOf(*node);
  if (input == nullptr || *input == nullptr) {
    return node;
  }
  LogicalNodePtr rewritten = LimitBelowProject(*input);
  return rewritten == *input ? node : std::visit(WithInput{.input = rewritten}, *node);
}

// ---- rule 4: projection pruning ----

// Old output position -> new output position of a rewritten node; -1 for a dropped column.
using Remap = std::vector<int>;

struct Pruned {
  LogicalNodePtr node;
  Remap remap;
};

Remap Identity(std::size_t width) {
  Remap remap(width);
  for (std::size_t i = 0; i < width; ++i) {
    remap[i] = Narrow<int>(i);
  }
  return remap;
}

void Need(std::vector<bool>& needed, const BoundColumn& column) {
  needed.at(Narrow<std::size_t>(column.index)) = true;
}

void Renumber(BoundColumn& column, const Remap& remap) {
  const int to = remap.at(Narrow<std::size_t>(column.index));
  ANTB1_CHECK(to >= 0);
  column.index = to;
}

Pruned Prune(const LogicalNodePtr& node, std::vector<bool> needed);

// Rewrites a node so that its output keeps at least the positions marked in `needed` (one flag per
// current output column). Only a Scan drops columns; Filter, Sort and Limit pass the request
// through (with the columns they reference), Project, Aggregate and GroupAggregate ask their input
// for exactly what they reference.
struct Pruner {
  const LogicalNodePtr& node;
  std::vector<bool>& needed;

  Pruned operator()(const ScanNode& scan) const {
    ScanNode out = scan;
    out.fields.clear();
    out.ids.clear();
    Remap remap(scan.fields.size(), -1);
    for (std::size_t i = 0; i < scan.fields.size(); ++i) {
      if (needed.at(i)) {
        remap[i] = Narrow<int>(out.fields.size());
        out.fields.push_back(scan.fields[i]);
        out.ids.push_back(scan.ids.at(i));
      }
    }
    return Pruned{.node = Make(std::move(out)), .remap = std::move(remap)};
  }

  Pruned operator()(const FilterNode& filter) const {
    for (const Predicate& p : filter.predicates) {
      if (p.column.has_value()) {
        Need(needed, *p.column);
      }
      if (p.other.has_value()) {
        Need(needed, *p.other);
      }
    }
    Pruned in = Prune(filter.input, std::move(needed));
    FilterNode out = filter;
    out.input = in.node;
    for (Predicate& p : out.predicates) {
      if (p.column.has_value()) {
        Renumber(*p.column, in.remap);
      }
      if (p.other.has_value()) {
        Renumber(*p.other, in.remap);
      }
    }
    return Pruned{.node = Make(std::move(out)), .remap = std::move(in.remap)};
  }

  // Keeps the needed input columns and the needed expressions (and what they read).
  Pruned operator()(const ComputeNode& compute) const {
    const std::size_t width = OutputWidth(*compute.input);
    std::vector<bool> below(needed.begin(), needed.begin() + static_cast<std::ptrdiff_t>(width));
    std::vector<std::size_t> kept;
    for (std::size_t k = 0; k < compute.exprs.size(); ++k) {
      if (!needed.at(width + k)) {
        continue;
      }
      kept.push_back(k);
      std::vector<int> reads;
      CollectColumns(*compute.exprs[k], reads);
      for (const int column : reads) {
        below.at(Narrow<std::size_t>(column)) = true;
      }
    }
    Pruned in = Prune(compute.input, std::move(below));
    Remap remap = in.remap;
    remap.resize(width + compute.exprs.size(), -1);
    if (kept.empty()) {
      return Pruned{.node = in.node, .remap = std::move(remap)};
    }
    ComputeNode out{.input = in.node, .exprs = {}, .ids = {}, .span = compute.span};
    const std::size_t new_width = OutputWidth(*in.node);
    for (const std::size_t k : kept) {
      remap[width + k] = Narrow<int>(new_width + out.exprs.size());
      out.exprs.push_back(plan::Renumber(compute.exprs[k], in.remap));
      out.ids.push_back(compute.ids.at(k));
    }
    return Pruned{.node = Make(std::move(out)), .remap = std::move(remap)};
  }

  Pruned operator()(const ProjectNode& project) const {
    const auto is_constant = [&](std::size_t i) {
      return !project.constants.empty() && project.constants[i].has_value();
    };
    std::vector<bool> below(OutputWidth(*project.input), false);
    for (std::size_t i = 0; i < project.columns.size(); ++i) {
      if (!is_constant(i)) {
        Need(below, project.columns[i]);
      }
    }
    const Pruned in = Prune(project.input, std::move(below));
    ProjectNode out = project;
    out.input = in.node;
    for (std::size_t i = 0; i < out.columns.size(); ++i) {
      if (!is_constant(i)) {
        Renumber(out.columns[i], in.remap);
      }
    }
    return Pruned{.node = Make(std::move(out)), .remap = Identity(project.columns.size())};
  }

  Pruned operator()(const AggregateNode& aggregate) const {
    std::vector<bool> below(OutputWidth(*aggregate.input), false);
    for (const AggregateCall& call : aggregate.aggregates) {
      if (call.arg.has_value()) {
        Need(below, *call.arg);
      }
    }
    const Pruned in = Prune(aggregate.input, std::move(below));
    AggregateNode out = aggregate;
    out.input = in.node;
    for (AggregateCall& call : out.aggregates) {
      if (call.arg.has_value()) {
        Renumber(*call.arg, in.remap);
      }
    }
    return Pruned{.node = Make(std::move(out)), .remap = Identity(aggregate.aggregates.size())};
  }

  Pruned operator()(const GroupAggregateNode& group) const {
    std::vector<bool> below(OutputWidth(*group.input), false);
    for (const BoundColumn& key : group.keys) {
      Need(below, key);
    }
    for (const AggregateCall& call : group.aggregates) {
      if (call.arg.has_value()) {
        Need(below, *call.arg);
      }
    }
    const Pruned in = Prune(group.input, std::move(below));
    GroupAggregateNode out = group;
    out.input = in.node;
    for (BoundColumn& key : out.keys) {
      Renumber(key, in.remap);
    }
    for (AggregateCall& call : out.aggregates) {
      if (call.arg.has_value()) {
        Renumber(*call.arg, in.remap);
      }
    }
    return Pruned{.node = Make(std::move(out)),
                  .remap = Identity(group.keys.size() + group.aggregates.size())};
  }

  Pruned operator()(const SortNode& sort) const {
    for (const SortKey& key : sort.keys) {
      Need(needed, key.column);
    }
    Pruned in = Prune(sort.input, std::move(needed));
    SortNode out = sort;
    out.input = in.node;
    for (SortKey& key : out.keys) {
      Renumber(key.column, in.remap);
    }
    return Pruned{.node = Make(std::move(out)), .remap = std::move(in.remap)};
  }

  Pruned operator()(const LimitNode& limit) const {
    Pruned in = Prune(limit.input, std::move(needed));
    LimitNode out = limit;
    out.input = in.node;
    return Pruned{.node = Make(std::move(out)), .remap = std::move(in.remap)};
  }

  Pruned operator()(const RowCountNode& /*rows*/) const {
    return Pruned{.node = node, .remap = Identity(1)};
  }
};

Pruned Prune(const LogicalNodePtr& node, std::vector<bool> needed) {
  return std::visit(Pruner{.node = node, .needed = needed}, *node);
}

}  // namespace

LogicalPlan Optimize(const LogicalPlan& plan) {
  if (plan.root == nullptr) {
    return plan;
  }
  LogicalNodePtr root =
      LimitBelowProject(DependentKeys(CountStarToRowCount(plan.root), /*limited=*/false));
  const std::size_t width = OutputWidth(*root);
  Pruned pruned = Prune(root, std::vector<bool>(width, true));
  const LogicalPlan optimized{.root = std::move(pruned.node), .output = plan.output};
  CheckPositions(optimized);  // the rules' positions are the positions of the ids (ADR 0022, P1)
  return ResolvePositions(optimized);
}

}  // namespace antb1::plan
