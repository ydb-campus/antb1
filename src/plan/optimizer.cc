#include "antb1/plan/optimizer.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "antb1/common/check.h"
#include "antb1/common/narrow.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/sql_status.h"

#include "aggregate_placement.h"
#include "arithmetic_rewrite.h"
#include "union_branches.h"
#include "unnesting.h"

// Every std::visit below uses a visitor with one overload per node type, so a new node type fails
// to compile until each rule handles it. The rules find columns by their ids and never read or set
// a position: plan::ResolvePositions sets every position at the end (ADR 0022).

namespace antb1::plan {
namespace {

LogicalNodePtr Make(LogicalNode node) {
  return std::make_shared<const LogicalNode>(std::move(node));
}

// The node over its inputs rewritten by `f` (every input, in order); the node itself when no
// input changes.
template <class F>
LogicalNodePtr MapInputs(const LogicalNodePtr& node, const F& f) {
  std::vector<LogicalNodePtr> inputs = InputsOf(*node);
  bool changed = false;
  for (LogicalNodePtr& input : inputs) {
    if (input == nullptr) {
      continue;
    }
    LogicalNodePtr rewritten = f(input);
    if (rewritten != input) {
      input = std::move(rewritten);
      changed = true;
    }
  }
  return changed ? WithInputs(*node, std::move(inputs)) : node;
}

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
  return MapInputs(node, CountStarToRowCount);
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

// The expression of `compute` that defines column `id`; nullptr if the column passes through.
const ExprPtr* ComputedBy(const ComputeNode& compute, ColumnId id) {
  const auto it = std::ranges::find(compute.ids, id);
  if (it == compute.ids.end()) {
    return nullptr;
  }
  return &compute.exprs.at(Narrow<std::size_t>(it - compute.ids.begin()));
}

LogicalNodePtr GroupByDeterminingKeys(const GroupAggregateNode& group) {
  const auto* compute = std::get_if<ComputeNode>(group.input.get());
  if (compute == nullptr) {
    return nullptr;
  }
  // Whether each key is dependent: computed by the Compute from keys alone. An expression of the
  // Compute reads only the Compute's input, so a key it reads is passed through.
  std::vector<bool> dependent(group.keys.size(), false);
  for (std::size_t k = 0; k < group.keys.size(); ++k) {
    const ExprPtr* expr = ComputedBy(*compute, group.keys[k].id);
    if (expr == nullptr) {
      continue;
    }
    std::vector<ColumnId> reads;
    CollectColumnIds(**expr, reads);
    dependent[k] = !reads.empty() && std::ranges::all_of(reads, [&](ColumnId read) {
      return std::ranges::any_of(group.keys, [&](const BoundColumn& key) {
        return key.id == read && key.type != LogicalType::kDouble;
      });
    });
  }
  if (std::ranges::none_of(dependent, [](bool d) { return d; })) {
    return nullptr;
  }
  // The GroupAggregate by the determining keys; `output` maps the input column of a kept key to the
  // key's output column.
  GroupAggregateNode kept = group;
  kept.keys.clear();
  kept.key_ids.clear();
  std::map<ColumnId, ColumnId> output;
  for (std::size_t k = 0; k < group.keys.size(); ++k) {
    if (!dependent[k]) {
      output.emplace(group.keys[k].id, group.key_ids.at(k));
      kept.keys.push_back(group.keys[k]);
      kept.key_ids.push_back(group.key_ids.at(k));
    }
  }
  // The dependent keys over its output, in key order, each the column it was as a key; the Project
  // passes every column through, so the nodes above read the same columns as before.
  ComputeNode above{.input = Make(std::move(kept)), .exprs = {}, .ids = {}, .span = compute->span};
  ProjectNode out{.input = nullptr, .columns = {}, .constants = {}, .ids = {}, .span = group.span};
  for (std::size_t k = 0; k < group.keys.size(); ++k) {
    BoundColumn column = group.keys[k];
    column.id = group.key_ids.at(k);
    if (dependent[k]) {
      above.exprs.push_back(
          MapColumns(*ComputedBy(*compute, group.keys[k].id), [&](ColumnExpr read) {
            const auto key = output.find(read.id);
            ANTB1_CHECK(key != output.end());
            read.id = key->second;
            return read;
          }));
      above.ids.push_back(column.id);
    }
    out.ids.push_back(column.id);
    out.columns.push_back(std::move(column));
  }
  for (const AggregateCall& call : group.aggregates) {
    out.columns.push_back(BoundColumn{.id = call.id, .name = CallName(call), .type = call.type});
    out.ids.push_back(call.id);
  }
  out.input = Make(std::move(above));
  return Make(std::move(out));
}

// Rewrites every GroupAggregate whose whole output is read. Under a Limit with no Sort in between,
// the nodes above stop reading after the rows they need: a key computed above the GroupAggregate
// would then be computed for those groups only, and an error (an overflow) in another group would
// no longer fail the query, as it does when every row computes it. A Sort reads everything. Both
// inputs of a join count as limited under a Limit (a probe may stop early; conservative for the
// build).
LogicalNodePtr DependentKeys(const LogicalNodePtr& node, bool limited) {
  if (std::holds_alternative<LimitNode>(*node)) {
    limited = true;
  } else if (std::holds_alternative<SortNode>(*node)) {
    limited = false;
  }
  // A set operation reads every input in full (ADR 0039), as a GROUP BY does.
  const bool group = std::holds_alternative<GroupAggregateNode>(*node) ||
                     std::holds_alternative<SetOperationNode>(*node);
  LogicalNodePtr current = MapInputs(
      node, [&](const LogicalNodePtr& input) { return DependentKeys(input, limited && !group); });
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
// right above a Sort (top-N). A Limit never moves below a Join.
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
  return MapInputs(node, LimitBelowProject);
}

// ---- rule 4: projection pruning ----

// The columns a node's output must keep, by id. Ids of columns the node does not output match
// nothing (ids are unique in the plan).
using Needed = std::set<ColumnId>;

void Need(Needed& needed, const BoundColumn& column) { needed.insert(column.id); }

LogicalNodePtr Prune(const LogicalNodePtr& node, Needed needed);

// Rewrites a node so that its output keeps at least the columns in `needed`. Only a Scan drops
// columns (fields) and only a Compute drops expressions, keeping their order; Filter, Sort and
// Limit pass the request through (with the columns they read), Project, Aggregate and
// GroupAggregate ask their input for exactly what they read, a Join asks both inputs for the
// request plus its keys and residual columns (each input keeps the ones it outputs), and a UnionAll
// asks each input for the columns it reads of it.
struct Pruner {
  const LogicalNodePtr& node;
  Needed& needed;

  // `node` (of one input) over its input pruned to `below`.
  [[nodiscard]] LogicalNodePtr Over(Needed below) const {
    return WithInputs(*node, {Prune(InputsOf(*node).at(0), std::move(below))});
  }

  LogicalNodePtr operator()(const ScanNode& scan) const {
    ScanNode out = scan;
    out.fields.clear();
    out.ids.clear();
    for (std::size_t i = 0; i < scan.fields.size(); ++i) {
      if (needed.contains(scan.ids.at(i))) {
        out.fields.push_back(scan.fields[i]);
        out.ids.push_back(scan.ids.at(i));
      }
    }
    return Make(std::move(out));
  }

  LogicalNodePtr operator()(const FilterNode& filter) const {
    FilterNode out = filter;
    // `needed` as it arrives is the demand this Filter is under, recorded before its own
    // predicates join it below -- and **intersected with this node's own outputs**, because the
    // same set is carried down both branches of every join above, so under one of those it also
    // holds the ids of an ancestor's other branch. Replaced rather than appended to: `out` is a
    // copy of a node that may have been pruned before, and `Optimize` is idempotent. Both of those
    // were review findings on JoinNode::needed_above, the first of which silently disabled that
    // item wherever it was worth the most. Sorted, so a pass is a function of its input and not of
    // a hash order.
    std::vector<ColumnId> above;
    for (const ColumnId id : OutputIds(*node)) {
      if (needed.contains(id)) {
        above.push_back(id);
      }
    }
    std::ranges::sort(above);
    out.needed_above = std::move(above);  // set, and empty where nothing above reads a column
    for (const Predicate& p : filter.predicates) {
      if (p.column.has_value()) {
        Need(needed, *p.column);
      }
      if (p.other.has_value()) {
        Need(needed, *p.other);
      }
    }
    const LogicalNodePtr with = Make(std::move(out));
    return WithInputs(*with, {Prune(filter.input, std::move(needed))});
  }

  // Keeps the needed expressions; the input keeps the needed columns and what they read.
  LogicalNodePtr operator()(const ComputeNode& compute) const {
    ComputeNode out{.input = nullptr, .exprs = {}, .ids = {}, .span = compute.span};
    for (std::size_t k = 0; k < compute.exprs.size(); ++k) {
      if (needed.contains(compute.ids.at(k))) {
        std::vector<ColumnId> reads;
        CollectColumnIds(*compute.exprs[k], reads);
        needed.insert(reads.begin(), reads.end());
        out.exprs.push_back(compute.exprs[k]);
        out.ids.push_back(compute.ids.at(k));
      }
    }
    out.input = Prune(compute.input, std::move(needed));
    return out.exprs.empty() ? out.input : Make(std::move(out));
  }

  LogicalNodePtr operator()(const ProjectNode& project) const {
    Needed below;
    for (std::size_t i = 0; i < project.columns.size(); ++i) {
      if (project.constants.empty() || !project.constants[i].has_value()) {
        Need(below, project.columns[i]);
      }
    }
    return Over(std::move(below));
  }

  LogicalNodePtr operator()(const AggregateNode& aggregate) const {
    Needed below;
    for (const AggregateCall& call : aggregate.aggregates) {
      if (call.arg.has_value()) {
        Need(below, *call.arg);
      }
    }
    return Over(std::move(below));
  }

  LogicalNodePtr operator()(const GroupAggregateNode& group) const {
    Needed below;
    for (const BoundColumn& key : group.keys) {
      Need(below, key);
    }
    for (const AggregateCall& call : group.aggregates) {
      if (call.arg.has_value()) {
        Need(below, *call.arg);
      }
    }
    return Over(std::move(below));
  }

  LogicalNodePtr operator()(const SortNode& sort) const {
    for (const SortKey& key : sort.keys) {
      Need(needed, key.column);
    }
    return Over(std::move(needed));
  }

  LogicalNodePtr operator()(const LimitNode& /*limit*/) const { return Over(std::move(needed)); }

  LogicalNodePtr operator()(const RowCountNode& /*rows*/) const { return node; }

  LogicalNodePtr operator()(const JoinNode& join) const {
    JoinNode out = join;
    // `needed` as it arrives is the demand this join is under, recorded before the join's own keys
    // and residual join it below -- but it is **not** a set of this join's columns. The same set is
    // carried down both branches of every join above, so at a nested join it also holds the ids of
    // an ancestor's other branch. Intersecting with this join's own outputs is what makes the field
    // mean what its name says, and it is load-bearing rather than tidy: the consumer refuses the
    // whole set when an id in it is not one the join outputs, so leaving a sibling's id in it
    // disabled the pruning on exactly the nested joins it is worth the most on.
    //
    // Sorted, and **replacing** whatever the field held: `out` is a copy of a node that may have
    // been pruned before (`Optimize` is idempotent and the tests assert it), and appending to an
    // already-recorded set would leave every id twice. Nothing reads the field by position, so that
    // would answer the same -- and go unnoticed, since EXPLAIN prints only what is *not* in it.
    out.needed_above.clear();
    for (const ColumnId id : OutputIds(*node)) {
      if (needed.contains(id)) {
        out.needed_above.push_back(id);
      }
    }
    std::ranges::sort(out.needed_above);
    for (const JoinKey& key : join.keys) {
      Need(needed, key.left);
      Need(needed, key.right);
    }
    for (const ExprPtr& conjunct : join.residual) {
      std::vector<ColumnId> reads;
      CollectColumnIds(*conjunct, reads);
      needed.insert(reads.begin(), reads.end());
    }
    const LogicalNodePtr with = Make(std::move(out));
    return WithInputs(*with, {Prune(join.left, needed), Prune(join.right, needed)});
  }
  LogicalNodePtr operator()(const DependentJoinNode& /*join*/) const {
    // Prune nothing through a dependent join: its right input reads columns of its left input as
    // free variables, which no walk of the right input can see, so pruning the left input by what
    // the nodes above it need would drop them. Unreachable in a plan that Optimize returns, since
    // the unnesting pass runs first and replaces every dependent join or rejects the query; a plan
    // test that prunes a hand-built one gets this rule.
    return node;
  }
  // Needs its keys and its calls' arguments, as a GroupAggregate does.
  // Keeps every output and every input column: each is a key of the deduplication, so no column can
  // go unread (ADR 0039), unlike a union's.
  LogicalNodePtr operator()(const SetOperationNode& set) const {
    std::vector<LogicalNodePtr> inputs;
    inputs.reserve(set.inputs.size());
    for (std::size_t k = 0; k < set.inputs.size(); ++k) {
      Needed below;
      for (const BoundColumn& column : set.columns.at(k)) {
        Need(below, column);
      }
      inputs.push_back(Prune(set.inputs[k], std::move(below)));
    }
    return WithInputs(*node, std::move(inputs));
  }
  LogicalNodePtr operator()(const GroupingSetsNode& group) const {
    Needed below;
    for (const BoundColumn& key : group.keys) {
      Need(below, key);
    }
    for (const AggregateCall& call : group.aggregates) {
      if (call.arg.has_value()) {
        Need(below, *call.arg);
      }
    }
    return Over(std::move(below));
  }
  // Keeps every output, as a Project does: a column nothing above reads is still computed in every
  // branch, as a derived table's is (narrowing both is a later item of ADR 0036).
  LogicalNodePtr operator()(const UnionAllNode& union_all) const {
    std::vector<LogicalNodePtr> inputs;
    inputs.reserve(union_all.inputs.size());
    for (std::size_t k = 0; k < union_all.inputs.size(); ++k) {
      Needed below;
      for (const BoundColumn& column : union_all.columns.at(k)) {
        Need(below, column);
      }
      inputs.push_back(Prune(union_all.inputs[k], std::move(below)));
    }
    return WithInputs(*node, std::move(inputs));
  }
};

LogicalNodePtr Prune(const LogicalNodePtr& node, Needed needed) {
  return std::visit(Pruner{.node = node, .needed = needed}, *node);
}

// Rule 6: every expression a Compute or a join's residual computes, rewritten by
// RewriteArithmetic (arithmetic_rewrite.h). Reads no column id and adds none.
LogicalNodePtr RewriteArithmeticIn(const LogicalNodePtr& node) {
  LogicalNodePtr out = MapInputs(node, RewriteArithmeticIn);
  const auto rewrite = [](std::vector<ExprPtr>& exprs) {
    bool changed = false;
    for (ExprPtr& expr : exprs) {
      ExprPtr rewritten = RewriteArithmetic(expr);
      if (rewritten != expr) {
        expr = std::move(rewritten);
        changed = true;
      }
    }
    return changed;
  };
  if (const auto* compute = std::get_if<ComputeNode>(out.get())) {
    ComputeNode rewritten = *compute;
    return rewrite(rewritten.exprs) ? Make(std::move(rewritten)) : out;
  }
  if (const auto* join = std::get_if<JoinNode>(out.get())) {
    JoinNode rewritten = *join;
    return rewrite(rewritten.residual) ? Make(std::move(rewritten)) : out;
  }
  return out;
}

}  // namespace

arrow::Result<LogicalPlan> Optimize(const LogicalPlan& plan, const BindOptions& options) {
  if (plan.root == nullptr) {
    return plan;
  }
  ARROW_ASSIGN_OR_RAISE(LogicalNodePtr root, Unnest(plan.root, options.build_side_ratio));
  // WHERE conditions into the branches of the unions they sit on, and the branches they rule out
  // pruned (ADR 0036's X7a); before ExchangeAggregates, whose gate reads the unions' estimates.
  root = FilterIntoUnionBranches(root);
  root = LimitBelowProject(DependentKeys(CountStarToRowCount(root), /*limited=*/false));
  // A grouped aggregate computed on one input of the join it reads, where the gate admits it
  // (docs/adr/0032-aggregate-placement.md's G2b). After the unnesting, so that a correlated
  // subquery's own aggregate is a candidate too, and before Prune, which then drops the columns
  // the aggregated side no longer has to carry.
  ColumnId last_exchanged = MaxColumnId(*root);
  root = ExchangeAggregates(root, last_exchanged);
  // Integer arithmetic as DuckDB computes it (rule 6): after every rule that reads expressions, so
  // that none meets a rewritten one, and before Prune, which then drops a column that a rewritten
  // expression no longer reads.
  root = RewriteArithmeticIn(root);
  const std::vector<ColumnId> output = OutputIds(*root);
  root = Prune(root, Needed(output.begin(), output.end()));
  // A dependent join that Unnest left behind is a bug in the pass, never an unsupported query: no
  // SqlErrorDetail, so it exits 70 and cannot pass for a refusal (ADR 0005, ADR 0023).
  if (FirstDependentJoin(root) != nullptr) {
    return arrow::Status::NotImplemented("the unnesting pass left a dependent join in the plan");
  }
  return ResolvePositions({.root = std::move(root), .output = plan.output});
}

}  // namespace antb1::plan
