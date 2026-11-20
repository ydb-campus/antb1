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
#include "antb1/plan/shared_sub_plans.h"
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

// Equality conditions on grouping keys can filter the input before it is grouped. Carry the
// same restriction to an equijoin's other input where every match must satisfy it too. Only
// integer-valued equality is moved here: no expression is inverted and no cast is approximated.
bool KeyEquality(const Predicate& predicate) {
  return predicate.kind == Predicate::Kind::kCompare && predicate.op == CompareOp::kEq &&
         predicate.column.has_value() && !predicate.other.has_value() &&
         (IsInteger(predicate.column->type) || predicate.column->type == LogicalType::kDate) &&
         predicate.constant.type == predicate.column->type &&
         std::holds_alternative<Int128>(predicate.constant.value);
}

bool SameEquality(const Predicate& a, const Predicate& b) {
  return KeyEquality(a) && KeyEquality(b) && a.column.has_value() && b.column.has_value() &&
         a.column->id == b.column->id && a.constant.type == b.constant.type &&
         a.constant.value == b.constant.value;
}

std::optional<Predicate> ThroughProject(const ProjectNode& project, const Predicate& predicate) {
  ANTB1_CHECK(predicate.column.has_value());  // called only for a KeyEquality
  const auto at = std::ranges::find(project.ids, predicate.column->id);
  if (at == project.ids.end()) {
    return std::nullopt;
  }
  const auto index = Narrow<std::size_t>(at - project.ids.begin());
  if ((!project.constants.empty() && project.constants[index].has_value()) ||
      project.columns[index].type != predicate.column->type) {
    return std::nullopt;
  }
  Predicate below = predicate;
  below.column = project.columns[index];
  return below;
}

struct PushdownState {
  std::set<const LogicalNode*> preserve;
  // A checked aggregate or an entire UNION ALL input can fail on rows an early filter removes.
  // An inferred filter can depend on other consumers, so preserve the trial for this pass.
  bool keep_filtered_plan = false;
};

LogicalNodePtr PushGroupKeyEqualities(const LogicalNodePtr& node, PushdownState& state);

struct GroupEqualities {
  std::vector<Predicate> values;
  bool grouped = false;
  // An inferred empty build can skip this entire source. Keep barriers separate from facts:
  // a filter may establish a new fact above a barrier without making its source safe to skip.
  bool safe = false;
};

GroupEqualities KnownGroupEqualities(const LogicalNodePtr& node);

bool OnlyColumnsAndConstants(const ComputeNode& compute) {
  return std::ranges::all_of(compute.exprs, [](const ExprPtr& expr) {
    return std::holds_alternative<ColumnExpr>(expr->node) ||
           std::holds_alternative<ConstantExpr>(expr->node);
  });
}

// Wide sums check the accumulator and AVG(DATE) checks the conversion to timestamp. Groups
// removed by a consumer's pushed filter must stay unevaluated regardless of the sharing gate.
bool CanFailOnGroup(const AggregateCall& call) {
  if (!call.arg.has_value() || (call.kind != AggKind::kSum && call.kind != AggKind::kAvg)) {
    return false;
  }
  const LogicalType type = call.arg->type;
  return type == LogicalType::kHugeInt ||
         (type == LogicalType::kDecimal && type.width() > LogicalType::kMaxNarrowDecimalWidth) ||
         (call.kind == AggKind::kAvg && type == LogicalType::kDate);
}

LogicalNodePtr PushEquality(const LogicalNodePtr& node, const Predicate& predicate,
                            PushdownState& state) {
  if (state.preserve.contains(node.get())) {
    return Make(FilterNode{.input = node, .predicates = {predicate}, .span = predicate.span});
  }
  ANTB1_CHECK(predicate.column.has_value());  // KeyEquality or its remapped copy
  if (const auto* filter = std::get_if<FilterNode>(node.get())) {
    if (std::ranges::any_of(filter->predicates,
                            [&](const Predicate& p) { return SameEquality(p, predicate); })) {
      return node;
    }
    LogicalNodePtr below = PushEquality(filter->input, predicate, state);
    if (const auto* placed = std::get_if<FilterNode>(below.get());
        placed != nullptr && placed->input == filter->input) {
      FilterNode out = *filter;
      out.predicates.push_back(predicate);
      return Make(std::move(out));
    }
    return WithInputs(*node, {std::move(below)});
  }
  if (const auto* project = std::get_if<ProjectNode>(node.get())) {
    if (const auto below = ThroughProject(*project, predicate)) {
      return WithInputs(*node, {PushEquality(project->input, *below, state)});
    }
  }
  if (const auto* group = std::get_if<GroupAggregateNode>(node.get())) {
    const auto at = std::ranges::find(group->key_ids, predicate.column->id);
    if (at != group->key_ids.end()) {
      if (std::ranges::any_of(group->aggregates, CanFailOnGroup)) {
        state.keep_filtered_plan = true;
      }
      Predicate below = predicate;
      below.column = group->keys[Narrow<std::size_t>(at - group->key_ids.begin())];
      return WithInputs(*node, {PushEquality(group->input, below, state)});
    }
  }
  // Do not move a filter ahead of expressions which may fail, or through a computed key.
  if (const auto* compute = std::get_if<ComputeNode>(node.get());
      compute != nullptr &&
      std::ranges::find(compute->ids, predicate.column->id) == compute->ids.end() &&
      OnlyColumnsAndConstants(*compute)) {
    return WithInputs(*node, {PushEquality(compute->input, predicate, state)});
  }
  if (const auto* join = std::get_if<JoinNode>(node.get());
      join != nullptr && join->residual.empty() && KnownGroupEqualities(join->left).safe &&
      KnownGroupEqualities(join->right).safe) {
    const auto left_ids = OutputIds(*join->left);
    const auto right_ids = OutputIds(*join->right);
    const bool left = std::ranges::find(left_ids, predicate.column->id) != left_ids.end();
    const bool right = std::ranges::find(right_ids, predicate.column->id) != right_ids.end();
    // A condition on a LEFT join's preserved input may move; one on its nullable input may not.
    if ((left && (join->kind == JoinKind::kInner || join->kind == JoinKind::kLeft)) ||
        (right && join->kind == JoinKind::kInner)) {
      JoinNode out = *join;
      auto& input = left ? out.left : out.right;
      input = PushEquality(input, predicate, state);
      for (const JoinKey& key : join->keys) {
        const BoundColumn& source = left ? key.left : key.right;
        const BoundColumn& target = left ? key.right : key.left;
        if (source.id == predicate.column->id && source.type == target.type) {
          Predicate inferred = predicate;
          inferred.column = target;
          auto& other = left ? out.right : out.left;
          other = PushEquality(other, inferred, state);
        }
      }
      return Make(std::move(out));
    }
  }
  LogicalNodePtr placed =
      Make(FilterNode{.input = node, .predicates = {predicate}, .span = predicate.span});
  if (std::holds_alternative<UnionAllNode>(*node)) {
    // A new condition above UNION ALL must reach its branches in this Optimize, too. Their
    // projections or aggregates may then admit it farther down. FALSE filters are left alone.
    LogicalNodePtr branches = FilterIntoUnionBranches(placed, &state.keep_filtered_plan);
    return branches != placed ? PushGroupKeyEqualities(branches, state) : placed;
  }
  // Windows, limits, global aggregates, grouping sets and distinct set operations are barriers.
  return placed;
}

// Conditions true for every returned row, expressed in this node's output ids. `grouped`
// restricts cross-input inference to grouped relations, whose predicates the binder cannot
// route into their already-bound bodies. A nullable side contributes no guaranteed equality.
GroupEqualities KnownGroupEqualities(const LogicalNodePtr& node) {
  if (const auto* filter = std::get_if<FilterNode>(node.get())) {
    GroupEqualities out = KnownGroupEqualities(filter->input);
    if (!out.safe) {
      return out;
    }
    for (const Predicate& predicate : filter->predicates) {
      if (KeyEquality(predicate)) {
        out.values.push_back(predicate);
      }
    }
    return out;
  }
  if (const auto* project = std::get_if<ProjectNode>(node.get())) {
    GroupEqualities out = KnownGroupEqualities(project->input);
    if (!out.safe) {
      return out;
    }
    std::vector<Predicate> mapped;
    for (std::size_t i = 0; i < project->columns.size(); ++i) {
      if (!project->constants.empty() && project->constants[i].has_value()) {
        continue;
      }
      for (const Predicate& predicate : out.values) {
        ANTB1_CHECK(predicate.column.has_value());  // every recorded fact is a KeyEquality
        if (predicate.column->id == project->columns[i].id) {
          Predicate above = predicate;
          BoundColumn column = *predicate.column;
          column.id = project->ids[i];
          above.column = std::move(column);
          mapped.push_back(std::move(above));
        }
      }
    }
    out.values = std::move(mapped);
    return out;
  }
  if (const auto* group = std::get_if<GroupAggregateNode>(node.get())) {
    // An inferred key can empty the opposite build, skipping this aggregate entirely, or
    // remove one of its failing groups when this is the target. A consumer's own filter
    // can still cross a checked group in PushEquality.
    if (std::ranges::any_of(group->aggregates, CanFailOnGroup)) {
      return {};
    }
    GroupEqualities out = KnownGroupEqualities(group->input);
    if (!out.safe) {
      return out;
    }
    std::vector<Predicate> mapped;
    for (std::size_t i = 0; i < group->keys.size(); ++i) {
      for (const Predicate& predicate : out.values) {
        ANTB1_CHECK(predicate.column.has_value());
        if (predicate.column->id == group->keys[i].id) {
          Predicate above = predicate;
          BoundColumn column = *predicate.column;
          column.id = group->key_ids[i];
          above.column = std::move(column);
          mapped.push_back(std::move(above));
        }
      }
    }
    return {.values = std::move(mapped), .grouped = true, .safe = true};
  }
  // Inferred conditions can empty the opposite build and prevent this input from opening at
  // all. Therefore collecting facts observes the same expression-error barriers as moving them.
  if (const auto* compute = std::get_if<ComputeNode>(node.get());
      compute != nullptr && OnlyColumnsAndConstants(*compute)) {
    return KnownGroupEqualities(compute->input);
  }
  if (const auto* join = std::get_if<JoinNode>(node.get());
      join != nullptr && join->residual.empty() &&
      (join->kind == JoinKind::kLeft || join->kind == JoinKind::kInner)) {
    GroupEqualities left = KnownGroupEqualities(join->left);
    GroupEqualities right = KnownGroupEqualities(join->right);
    if (!left.safe || !right.safe) {
      return {};
    }
    if (join->kind == JoinKind::kLeft) {
      return left;
    }
    left.grouped = left.grouped || right.grouped;
    left.values.insert(left.values.end(), right.values.begin(), right.values.end());
    return left;
  }
  if (std::holds_alternative<ScanNode>(*node) || std::holds_alternative<RowCountNode>(*node)) {
    return {.safe = true};
  }
  return {};
}

LogicalNodePtr PushGroupKeyEqualities(const LogicalNodePtr& node, PushdownState& state) {
  if (state.preserve.contains(node.get())) {
    return node;
  }
  LogicalNodePtr current = MapInputs(
      node, [&](const LogicalNodePtr& input) { return PushGroupKeyEqualities(input, state); });
  if (const auto* filter = std::get_if<FilterNode>(current.get())) {
    LogicalNodePtr below = filter->input;
    std::vector<Predicate> kept;
    for (const Predicate& predicate : filter->predicates) {
      if (KeyEquality(predicate)) {
        LogicalNodePtr pushed = PushEquality(below, predicate, state);
        const auto* placed = std::get_if<FilterNode>(pushed.get());
        // Keep the original predicate order and filter span when nothing moved. In particular,
        // ordinary scan filters and repeated EXISTS checks need no rewrite here.
        if (pushed != below && (placed == nullptr || placed->input != below)) {
          below = std::move(pushed);
          continue;
        }
      }
      kept.push_back(predicate);
    }
    if (kept.size() == filter->predicates.size()) {
      return current;
    }
    if (kept.empty()) {
      return below;
    }
    FilterNode out = *filter;
    out.input = std::move(below);
    out.predicates = std::move(kept);
    return Make(std::move(out));
  }
  if (const auto* join = std::get_if<JoinNode>(current.get());
      join != nullptr && (join->kind == JoinKind::kInner || join->kind == JoinKind::kLeft)) {
    JoinNode out = *join;
    const GroupEqualities left = KnownGroupEqualities(out.left);
    const GroupEqualities right = KnownGroupEqualities(out.right);
    if (!left.safe || !right.safe) {
      return current;
    }
    const auto infer = [&](const GroupEqualities& known, bool from_left) {
      if (!known.grouped) {
        return;
      }
      for (const Predicate& predicate : known.values) {
        ANTB1_CHECK(predicate.column.has_value());
        for (const JoinKey& key : join->keys) {
          const BoundColumn& source = from_left ? key.left : key.right;
          const BoundColumn& target = from_left ? key.right : key.left;
          if (predicate.column->id == source.id && source.type == target.type) {
            Predicate inferred = predicate;
            inferred.column = target;
            auto& input = from_left ? out.right : out.left;
            input = PushEquality(input, inferred, state);
          }
        }
      }
    };
    infer(left, true);
    if (join->kind == JoinKind::kInner) {
      infer(out.right == join->right ? right : KnownGroupEqualities(out.right), false);
    }
    return out.left == join->left && out.right == join->right ? current : Make(std::move(out));
  }
  return current;
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
  LogicalNodePtr operator()(const WindowNode& window) const {
    // Keep every call, including unused results, and all dependencies. W1 windows are barriers.
    for (const WindowCall& call : window.calls) {
      needed.erase(call.id);
      for (const BoundColumn& arg : call.arguments) {
        Need(needed, arg);
      }
      for (const BoundColumn& key : call.partition_by) {
        Need(needed, key);
      }
      for (const SortKey& key : call.order_by) {
        Need(needed, key.column);
      }
    }
    return Over(std::move(needed));
  }

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

// Aggregate output ids belong to one reference and survive filter movement and the switch to
// partial/final aggregation. Keep the nested aggregates too: a consumer-specific outer group
// can become cheap to recompute if every consumer still reads the same materialized partial.
void CollectAggregateIds(const LogicalNodePtr& node, std::vector<ColumnId>& ids) {
  if (std::holds_alternative<AggregateNode>(*node) ||
      std::holds_alternative<GroupAggregateNode>(*node)) {
    const auto output = OutputIds(*node);
    ids.push_back(output.empty() ? kNoColumnId : output.front());
  }
  for (const LogicalNodePtr& input : InputsOf(*node)) {
    CollectAggregateIds(input, ids);
  }
}

struct ReusableAggregate {
  std::vector<LogicalNodePtr> members;
  // One set per aggregate in the common subtree, one id per consumer of that aggregate.
  std::vector<std::set<ColumnId>> consumers;
};

std::vector<ReusableAggregate> ReusableAggregates(const LogicalNodePtr& root) {
  std::vector<ReusableAggregate> result;
  for (SharedSubPlan& group : SharedSubPlans({.root = root, .output = {}})) {
    if (WorthSharing(group)) {
      ReusableAggregate reuse{.members = std::move(group.members), .consumers = {}};
      for (const LogicalNodePtr& member : reuse.members) {
        std::vector<ColumnId> ids;
        CollectAggregateIds(member, ids);
        // SharedSubPlans compares inputs in order, so the same traversal of each member
        // visits corresponding aggregates in the same order, even across different aliases.
        reuse.consumers.resize(ids.size());
        for (std::size_t i = 0; i < ids.size(); ++i) {
          reuse.consumers[i].insert(ids[i]);
        }
      }
      result.push_back(std::move(reuse));
    }
  }
  return result;
}

bool StillShared(const ReusableAggregate& before, const ReusableAggregate& after) {
  return std::ranges::any_of(before.consumers, [&](const auto& old_consumers) {
    return !old_consumers.contains(kNoColumnId) && old_consumers.size() == before.members.size() &&
           std::ranges::any_of(after.consumers, [&](const auto& new_consumers) {
             return std::ranges::includes(new_consumers, old_consumers);
           });
  });
}

// Try the ordinary rewrite first. All consumers can benefit when their inputs are rewritten
// alike. If a materializable aggregate loses a consumer, retry from the original tree with
// those producers as boundaries, so the consumer's restriction stays above the shared result.
template <class Rewrite>
LogicalNodePtr PreserveSharedAggregates(const LogicalNodePtr& root, const Rewrite& rewrite) {
  PushdownState state;
  LogicalNodePtr next = rewrite(state);
  if (next == root || state.keep_filtered_plan) {
    return next;
  }
  std::vector<ColumnId> aggregate_ids;
  CollectAggregateIds(root, aggregate_ids);
  if (aggregate_ids.size() < 2) {
    return next;
  }
  const auto before = ReusableAggregates(root);
  while (!before.empty()) {
    const auto after = ReusableAggregates(next);
    const auto previous = state.preserve.size();
    bool lost = false;
    for (const auto& group : before) {
      if (std::ranges::none_of(
              after, [&](const auto& candidate) { return StillShared(group, candidate); })) {
        lost = true;
        for (const LogicalNodePtr& member : group.members) {
          state.preserve.insert(member.get());
        }
      }
    }
    if (!lost) {
      break;
    }
    if (state.preserve.size() == previous) {
      return root;  // A change outside the protected members also obscured their sharing.
    }
    next = rewrite(state);
  }
  return next;
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
  const ColumnId last = MaxColumnId(*root);
  root = PreserveSharedAggregates(root, [&](const PushdownState& state) {
    ColumnId last_exchanged = last;
    return ExchangeAggregates(root, last_exchanged, state.preserve);
  });
  // Integer arithmetic as DuckDB computes it (rule 6): after every rule that reads expressions, so
  // that none meets a rewritten one, and before Prune, which then drops a column that a rewritten
  // expression no longer reads.
  root = RewriteArithmeticIn(root);
  const std::vector<ColumnId> output = OutputIds(*root);
  root = Prune(root, Needed(output.begin(), output.end()));
  // After limits, projections and unused computations reach their final shape: otherwise a
  // second Optimize could move a predicate farther than the first. Refresh pruning afterwards:
  // new filters and changed join inputs need their own needed_above sets for scan pushdown.
  root = PreserveSharedAggregates(
      root, [&](PushdownState& state) { return PushGroupKeyEqualities(root, state); });
  root = Prune(root, Needed(output.begin(), output.end()));
  // A dependent join that Unnest left behind is a bug in the pass, never an unsupported query: no
  // SqlErrorDetail, so it exits 70 and cannot pass for a refusal (ADR 0005, ADR 0023).
  if (FirstDependentJoin(root) != nullptr) {
    return arrow::Status::NotImplemented("the unnesting pass left a dependent join in the plan");
  }
  return ResolvePositions({.root = std::move(root), .output = plan.output});
}

}  // namespace antb1::plan
