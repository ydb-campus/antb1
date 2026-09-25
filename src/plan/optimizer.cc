#include "antb1/plan/optimizer.h"

#include <cstddef>
#include <memory>
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
  LogicalNodePtr operator()(const ProjectNode& node) const { return Replace(node); }
  LogicalNodePtr operator()(const AggregateNode& node) const { return Replace(node); }
  LogicalNodePtr operator()(const LimitNode& node) const { return Replace(node); }
  LogicalNodePtr operator()(const RowCountNode& node) const { return Make(node); }
};

std::size_t OutputWidth(const LogicalNode& node);

struct OutputWidthOf {
  std::size_t operator()(const ScanNode& node) const { return node.fields.size(); }
  std::size_t operator()(const FilterNode& node) const { return OutputWidth(*node.input); }
  std::size_t operator()(const ProjectNode& node) const { return node.columns.size(); }
  std::size_t operator()(const AggregateNode& node) const { return node.aggregates.size(); }
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

// ---- rule 2: projection pruning ----

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
// current output column). Only a Scan drops columns; Filter and Limit pass the request through,
// Project and Aggregate ask their input for exactly what they reference.
struct Pruner {
  const LogicalNodePtr& node;
  std::vector<bool>& needed;

  Pruned operator()(const ScanNode& scan) const {
    ScanNode out = scan;
    out.fields.clear();
    Remap remap(scan.fields.size(), -1);
    for (std::size_t i = 0; i < scan.fields.size(); ++i) {
      if (needed.at(i)) {
        remap[i] = Narrow<int>(out.fields.size());
        out.fields.push_back(scan.fields[i]);
      }
    }
    return Pruned{.node = Make(std::move(out)), .remap = std::move(remap)};
  }

  Pruned operator()(const FilterNode& filter) const {
    for (const Predicate& p : filter.predicates) {
      if (p.column.has_value()) {
        Need(needed, *p.column);
      }
    }
    Pruned in = Prune(filter.input, std::move(needed));
    FilterNode out = filter;
    out.input = in.node;
    for (Predicate& p : out.predicates) {
      if (p.column.has_value()) {
        Renumber(*p.column, in.remap);
      }
    }
    return Pruned{.node = Make(std::move(out)), .remap = std::move(in.remap)};
  }

  Pruned operator()(const ProjectNode& project) const {
    std::vector<bool> below(OutputWidth(*project.input), false);
    for (const BoundColumn& c : project.columns) {
      Need(below, c);
    }
    const Pruned in = Prune(project.input, std::move(below));
    ProjectNode out = project;
    out.input = in.node;
    for (BoundColumn& c : out.columns) {
      Renumber(c, in.remap);
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
  LogicalNodePtr root = CountStarToRowCount(plan.root);
  const std::size_t width = OutputWidth(*root);
  Pruned pruned = Prune(root, std::vector<bool>(width, true));
  return LogicalPlan{.root = std::move(pruned.node), .output = plan.output};
}

}  // namespace antb1::plan
