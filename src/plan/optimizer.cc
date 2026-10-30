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
#include "join_order.h"
#include "key_type.h"

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
  const bool group = std::holds_alternative<GroupAggregateNode>(*node);
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

// The outermost dependent join of the plan, nullptr when it has none. Pre-order, so the first one
// found is the one whose span names the whole correlated subquery.
const DependentJoinNode* FirstDependentJoin(const LogicalNodePtr& node) {
  if (node == nullptr) {
    return nullptr;
  }
  if (const auto* dependent = std::get_if<DependentJoinNode>(node.get())) {
    return dependent;
  }
  for (const LogicalNodePtr& input : InputsOf(*node)) {
    if (const DependentJoinNode* found = FirstDependentJoin(input)) {
      return found;
    }
  }
  return nullptr;
}

// The unnesting pass (ADR 0023), the first and only fallible step of Optimize: it replaces every
// dependent join with ordinary joins, or rejects the query with exit code 4 at the subquery's span.
// Roadmap id U1 has neither rewrite, so it rejects every one; U2 adds Path 1 (correlated EXISTS and
// NOT EXISTS pulled up into semi and anti joins) and U3 Path 2 (correlated scalar aggregates by
// equivalence substitution). Outermost first, as the top-down algorithm goes.
// ---- ADR 0023's Path 1: correlated selections pulled up (EXISTS and NOT EXISTS, U2) ----

// Which side of a dependent join an expression reads.
struct Sides {
  bool inner = false;
  bool outer = false;

  [[nodiscard]] bool both() const { return inner && outer; }
  [[nodiscard]] bool outer_only() const { return outer && !inner; }
};

Sides SidesOf(const Expr& expr, const std::set<ColumnId>& outer_ids) {
  std::vector<ColumnId> reads;
  CollectColumnIds(expr, reads);
  Sides sides;
  for (const ColumnId id : reads) {
    if (id == kNoColumnId) {
      continue;
    }
    (outer_ids.contains(id) ? sides.outer : sides.inner) = true;
  }
  return sides;
}

// The block of a correlated subquery, as ADR 0023's Binding section leaves it: the nodes a semi or
// anti join reads none of (the Project of the select list, the Compute of its computed items and
// the Sort of an ORDER BY) over the one Filter that holds the block's correlated conjuncts, over
// the Compute that evaluates them, over the block's own joins. Path 1 strips the first group,
// moves the conjuncts into the join and keeps the rest.
struct Block {
  const FilterNode* filter = nullptr;     // the correlated conjuncts, as kIsTrue over `computed`
  const ComputeNode* computed = nullptr;  // their expressions
  LogicalNodePtr below;                   // the block's joins: what the new join's right input is
};

// Strips the select list and the sort, then matches the correlated filter and its Compute.
// std::nullopt when the block does not have that shape, which makes Path 1 decline rather than
// rewrite something it has not understood.
std::optional<Block> BlockOf(const LogicalNodePtr& root) {
  LogicalNodePtr node = root;
  while (node != nullptr &&
         (std::holds_alternative<ProjectNode>(*node) ||
          std::holds_alternative<ComputeNode>(*node) || std::holds_alternative<SortNode>(*node))) {
    const std::vector<LogicalNodePtr> inputs = InputsOf(*node);
    if (inputs.size() != 1) {
      return std::nullopt;
    }
    // A Compute here is the select list's; the correlated layer sits below the Filter, so a
    // Compute reached before any Filter is never it.
    node = inputs.front();
  }
  const auto* filter = node == nullptr ? nullptr : std::get_if<FilterNode>(node.get());
  if (filter == nullptr) {
    return std::nullopt;
  }
  const auto* computed = std::get_if<ComputeNode>(filter->input.get());
  if (computed == nullptr) {
    return std::nullopt;
  }
  return Block{.filter = filter, .computed = computed, .below = computed->input};
}

// The expression of `computed` that defines `id`, or nullptr.
const ExprPtr* DefinedBy(const ComputeNode& computed, ColumnId id) {
  const auto it = std::ranges::find(computed.ids, id);
  if (it == computed.ids.end()) {
    return nullptr;
  }
  return &computed.exprs.at(Narrow<std::size_t>(it - computed.ids.begin()));
}

// One conjunct of the block's top filter, as Path 1 classifies it.
struct Conjunct {
  const Expr* expr = nullptr;  // the PredicateExpr the Compute holds for it
  Sides sides;
};

// A key of the rewritten join, where the conjunct is an equality of a pure-inner and a pure-outer
// side whose common type is not DOUBLE (ADR 0023's Path 1). Both sides must be plain columns: an
// equality over an expression stays a residual, which is always correct and never wrong.
struct KeyParts {
  ColumnExpr inner;
  ColumnExpr outer;
  LogicalType inner_type = LogicalType::kBigInt;
  LogicalType outer_type = LogicalType::kBigInt;
  LogicalType key_type = LogicalType::kBigInt;
  // What the binder calls each side. BindBool's column_of puts the names in the PredicateExpr's
  // own predicate, positionally, while the ids live in the operands; EXPLAIN prints them, so a key
  // built without them reads `keys=[outer = inner]`.
  std::string inner_name;
  std::string outer_name;
};

std::optional<KeyParts> AsKey(const Expr& expr, const std::set<ColumnId>& outer_ids) {
  const auto* predicate = std::get_if<PredicateExpr>(&expr.node);
  if (predicate == nullptr || predicate->predicate.kind != Predicate::Kind::kCompareColumns ||
      predicate->predicate.op != CompareOp::kEq || predicate->operands.size() != 2) {
    return std::nullopt;
  }
  const auto* a = std::get_if<ColumnExpr>(&predicate->operands[0]->node);
  const auto* b = std::get_if<ColumnExpr>(&predicate->operands[1]->node);
  if (a == nullptr || b == nullptr) {
    return std::nullopt;
  }
  const bool a_outer = outer_ids.contains(a->id);
  const bool b_outer = outer_ids.contains(b->id);
  if (a_outer == b_outer) {
    return std::nullopt;  // both sides on one side of the join: no key
  }
  const Expr& inner_expr = *predicate->operands[a_outer ? 1 : 0];
  const Expr& outer_expr = *predicate->operands[a_outer ? 0 : 1];
  const std::optional<LogicalType> key = KeyType(inner_expr.type, outer_expr.type);
  if (!key.has_value()) {
    return std::nullopt;  // DOUBLE, or no type holds both: a residual
  }
  const auto name_of = [](const std::optional<BoundColumn>& column, const Expr& fallback) {
    return column.has_value() && !column->name.empty() ? column->name : fallback.name;
  };
  const std::optional<BoundColumn>& first = predicate->predicate.column;
  const std::optional<BoundColumn>& second = predicate->predicate.other;
  return KeyParts{.inner = a_outer ? *b : *a,
                  .outer = a_outer ? *a : *b,
                  .inner_type = inner_expr.type,
                  .outer_type = outer_expr.type,
                  .key_type = *key,
                  .inner_name = name_of(a_outer ? second : first, inner_expr),
                  .outer_name = name_of(a_outer ? first : second, outer_expr)};
}

// ---- ADR 0023's Path 2: equivalence substitution (scalar aggregates, U3) ----

// A union-find over column ids. ADR 0023 builds the classes from the equalities between two
// columns among the WHERE conjuncts of the subquery's block; this takes only the CORRELATED ones,
// the conjuncts of the block's top filter.
//
// That is narrower than the ADR and deliberately so. An inner-inner equality can only extend a
// class that already holds an inner member, so it never gives an outer column its first inner
// equivalent: which shapes are accepted is the same either way, and only the choice of
// representative could differ. What it buys is two whole classes of wrong answer that cannot then
// arise: a LEFT JOIN's ON key, which is not enforced on padded rows and would make the group key
// non-NULL exactly where the correlation was NULL, and a conjunct of a nested dependent join,
// which an anti join keeps the rows of where it is false. Both would be silent.
class Classes {
 public:
  void Merge(ColumnId a, ColumnId b) { parent_[Find(a)] = Find(b); }

  ColumnId Find(ColumnId id) {
    const auto it = parent_.find(id);
    if (it == parent_.end()) {
      parent_[id] = id;
      return id;
    }
    if (it->second == id) {
      return id;
    }
    const ColumnId root = Find(it->second);
    parent_[id] = root;  // path compression
    return root;
  }

  [[nodiscard]] bool Same(ColumnId a, ColumnId b) { return Find(a) == Find(b); }

 private:
  std::map<ColumnId, ColumnId> parent_;
};

// The subquery's block as Path 2 needs it: an ungrouped aggregate over the correlated filter.
//
//   [Project]           the select list, dropped: the join reads the aggregate's columns
//   [Compute]           the select item over the aggregate (0.2 * avg(x)), kept above the join
//   Aggregate           becomes a GroupAggregate keyed by the inner equivalents
//   [Compute]           the aggregate's own arguments, kept below it
//   Filter              the correlated conjuncts, each dropped once it reads inner columns only
//   Compute             their expressions
//   <below>             the block's joins
struct ScalarBlock {
  std::vector<LogicalNodePtr> above;  // between the dependent join and the Aggregate, top-down
  const AggregateNode* aggregate = nullptr;
  LogicalNodePtr args;  // the Compute of the aggregate's arguments, or nullptr
  const FilterNode* filter = nullptr;
  const ComputeNode* computed = nullptr;
  LogicalNodePtr below;
};

std::optional<ScalarBlock> ScalarBlockOf(const LogicalNodePtr& root) {
  ScalarBlock out;
  LogicalNodePtr node = root;
  while (node != nullptr && (std::holds_alternative<ProjectNode>(*node) ||
                             std::holds_alternative<ComputeNode>(*node))) {
    out.above.push_back(node);
    const std::vector<LogicalNodePtr> inputs = InputsOf(*node);
    if (inputs.size() != 1) {
      return std::nullopt;
    }
    node = inputs.front();
  }
  out.aggregate = node == nullptr ? nullptr : std::get_if<AggregateNode>(node.get());
  if (out.aggregate == nullptr) {
    return std::nullopt;  // the binder admits only an ungrouped aggregate here
  }
  LogicalNodePtr under = out.aggregate->input;
  if (under != nullptr && std::holds_alternative<ComputeNode>(*under)) {
    out.args = under;
    under = std::get<ComputeNode>(*under).input;
  }
  out.filter = under == nullptr ? nullptr : std::get_if<FilterNode>(under.get());
  if (out.filter == nullptr) {
    return std::nullopt;
  }
  out.computed = std::get_if<ComputeNode>(out.filter->input.get());
  if (out.computed == nullptr) {
    return std::nullopt;
  }
  out.below = out.computed->input;
  return out;
}

// Path 1's rewrite (ADR 0023, the formalization's Lemma 4.8 then Lemma 3.1): the block's select
// list is dropped, since the join reads none of its columns, and the conjuncts of the block's top
// filter move into the join, which becomes a semi join (EXISTS) or a plain anti join (NOT EXISTS,
// which has no NULL case, unlike NOT IN). Its build side is the subquery (ADR 0022).
//
//   DependentJoin SEMI                     Join SEMI  keys o.k = t.k  residual t.x < o.x
//     <outer>                                <outer>
//     Project ...                            Filter t.z = 1
//       Filter <correlated conjuncts>          Scan t
//         Compute <their expressions>
//           Filter t.z = 1
//             Scan t
arrow::Result<LogicalNodePtr> Path1(const DependentJoinNode& join, ColumnId& last_id,
                                    int64_t build_side_ratio) {
  const auto refuse = [&join](std::string_view what) {
    return UnsupportedError(
        std::format("a correlated {} {} is not supported",
                    join.kind == JoinKind::kSemi ? "EXISTS" : "NOT EXISTS", what),
        join.span);
  };
  const std::optional<Block> block = BlockOf(join.right);
  if (!block.has_value()) {
    // An aggregate, GROUP BY, HAVING, LIMIT or OFFSET leaves a node Path 1 does not know between
    // the join and the correlated filter. The binder refuses those statically, so this is the
    // backstop for a shape it has not met.
    return refuse("over a block that is not a plain FROM and WHERE");
  }
  const std::vector<ColumnId> left_ids = OutputIds(*join.left);
  const std::set<ColumnId> outer(left_ids.begin(), left_ids.end());

  std::vector<JoinKey> keys;
  std::vector<ExprPtr> residual;
  std::vector<ExprPtr> inner_casts;  // a key's inner side widened to the key type
  std::vector<ColumnId> inner_cast_ids;
  std::vector<ExprPtr> outer_casts;  // and its outer side
  std::vector<ColumnId> outer_cast_ids;

  for (const Predicate& predicate : block->filter->predicates) {
    if (predicate.kind != Predicate::Kind::kIsTrue || !predicate.column.has_value()) {
      return refuse("whose correlated conjunct the pass does not recognize");
    }
    const ExprPtr* defined = DefinedBy(*block->computed, predicate.column->id);
    if (defined == nullptr) {
      return refuse("whose correlated conjunct the pass does not recognize");
    }
    const ExprPtr& expr = *defined;
    const Sides sides = SidesOf(*expr, outer);
    if (!sides.outer) {
      // An inner-only conjunct stays where the binder put it; one can only reach the block's top
      // filter when it shares a computed column with a correlated one.
      residual.push_back(expr);
      continue;
    }
    if (sides.outer_only()) {
      // ADR 0023: an outer-only conjunct must not become a filter on the outer rows of an anti
      // join -- where it is false or NULL the subquery has no row, so NOT EXISTS keeps the outer
      // row that a filter would drop. A residual gets that right: the pair does not match, and
      // the anti join keeps the row.
      //
      // The ADR puts an EXISTS's outer-only conjunct in a filter below the semi join instead,
      // which is the same answer and drops the rows earlier. A residual is used for both here:
      // the filter would have to compute the conjunct on the probe side, which widens the semi
      // join's output and so breaks Optimize's contract that a plan keeps its output columns,
      // and it would need a Project to undo. Pushing it down is a performance refinement, and
      // the pass is correct without it.
      residual.push_back(expr);
      continue;
    }
    if (const std::optional<KeyParts> key = AsKey(*expr, outer)) {
      // Both sides cast to the common type, as ADR 0022's keys are, each in a layer of its own on
      // its side of the join: ResolvePositions requires a key's two sides to have one type, and
      // exec compares that type against the column's own.
      const auto side = [&last_id](const ColumnExpr& column, LogicalType from, LogicalType to,
                                   std::vector<ExprPtr>& casts, std::vector<ColumnId>& cast_ids,
                                   std::string name) {
        if (from == to) {
          return BoundColumn{.id = column.id, .name = std::move(name), .type = to};
        }
        casts.push_back(WidenTo(
            std::make_shared<const Expr>(Expr{.node = column, .type = from, .name = name}), to));
        cast_ids.push_back(ColumnId{std::to_underlying(last_id) + 1});
        last_id = cast_ids.back();
        return BoundColumn{.id = cast_ids.back(), .name = casts.back()->name, .type = to};
      };
      keys.push_back(JoinKey{.left = side(key->outer, key->outer_type, key->key_type, outer_casts,
                                          outer_cast_ids, key->outer_name),
                             .right = side(key->inner, key->inner_type, key->key_type, inner_casts,
                                           inner_cast_ids, key->inner_name)});
      continue;
    }
    residual.push_back(expr);  // reads both sides and is no key: a residual over candidate pairs
  }

  if (keys.empty()) {
    // ADR 0023: as a LEFT JOIN without one does in ADR 0022, since the hash table and the
    // candidate pairs need a key.
    return refuse("without a key");
  }

  LogicalNodePtr left = join.left;
  if (!outer_casts.empty()) {
    left = Make(ComputeNode{
        .input = std::move(left), .exprs = outer_casts, .ids = outer_cast_ids, .span = join.span});
  }
  LogicalNodePtr right = block->below;
  if (!inner_casts.empty()) {
    right = Make(ComputeNode{
        .input = std::move(right), .exprs = inner_casts, .ids = inner_cast_ids, .span = join.span});
  }
  // ADR 0026's C2b, C2c and C3: a semi or an anti join may build on its left input, the side it
  // preserves, when the subquery side is decisively larger -- the shape a correlated EXISTS or NOT
  // EXISTS over a large relation has, where ADR 0022's rule built the whole of that relation to
  // probe it with far fewer rows. A null-aware anti join (NOT IN) still cannot emit its build rows,
  // so it keeps building on the subquery side whatever the estimates say.
  const std::optional<int64_t> left_rows = EstimateRows(*left);
  const bool chooses = join.kind == JoinKind::kSemi || join.kind == JoinKind::kAnti;
  const BuildSide build = chooses
                              ? ChooseBuildSide(left_rows, EstimateRows(*right), build_side_ratio)
                              : BuildSide::kRight;
  LogicalNodePtr out = Make(JoinNode{.kind = join.kind,
                                     .left = std::move(left),
                                     .right = std::move(right),
                                     .keys = std::move(keys),
                                     .residual = std::move(residual),
                                     .build = build,
                                     // A semi or an anti join keeps a subset of its left input, so
                                     // that is an upper bound: without it every join above this
                                     // rewrite would read its own left input as unknown.
                                     .estimate = left_rows,
                                     .span = join.span});
  // Lemma 3.1: the rewrite is legal only when the new join's right input has no free variables
  // left. A bug in the classification above is the only way this fails, and it must not reach
  // ResolvePositions, which would abort the process rather than fail the query.
  if (!FreeVariables(*std::get<JoinNode>(*out).right).empty()) {
    return arrow::Status::NotImplemented(
        "the unnesting pass left a free variable in a rewritten join's right input");
  }
  return out;
}

// Whether an expression over the aggregate's columns is STRICT: NULL whenever an operand is NULL.
// ADR 0023's Path 2 accepts only such a combination, and the reason is the COUNT bug (R. A. Ganski
// and H. K. T. Wong, SIGMOD 1987): the rewrite gives an outer row with no group no row at all, so
// the comparison must reject it, which it does exactly when the subquery's value is NULL there.
// SUM, AVG, MIN and MAX are NULL over no rows and a strict combination keeps that NULL; COUNT is
// 0, and CASE, COALESCE, OR, AND and some functions turn a NULL into a value, so the comparison
// could accept a row the inner join drops -- a silently wrong answer, not a refusal.
bool IsStrict(const Expr& expr) {
  return std::visit(
      [](const auto& node) {
        using Node = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<Node, ColumnExpr> || std::is_same_v<Node, ConstantExpr>) {
          return true;  // a column of the aggregation, or a literal
        } else if constexpr (std::is_same_v<Node, ArithExpr>) {
          return IsStrict(*node.left) && IsStrict(*node.right);
        } else if constexpr (std::is_same_v<Node, NegateExpr> || std::is_same_v<Node, CastExpr>) {
          // Unary minus, and the implicit casts of an operator's operands.
          return IsStrict(*node.operand);
        } else {
          // FunctionExpr, PredicateExpr, BoolExpr, CaseExpr: none is strict in general.
          return false;
        }
      },
      expr.node);
}

// Whether the aggregation's calls are the four ADR 0023's Path 2 admits. COUNT in any spelling is
// 0 over no rows, which is what breaks the rewrite.
bool StrictAggregates(const std::vector<AggregateCall>& calls) {
  return std::ranges::all_of(calls, [](const AggregateCall& call) {
    switch (call.kind) {
      case AggKind::kSum:
      case AggKind::kAvg:
      case AggKind::kMin:
      case AggKind::kMax:
        return true;
      case AggKind::kCountStar:
      case AggKind::kCount:
      case AggKind::kCountDistinct:
      // A sum of counts is a count for this rule's purpose, and for its stated reason: it answers
      // 0 over no rows, by design (ADR 0032's G2c). Unreachable today, since the unnesting runs
      // before the exchange that makes one, and correct if that ever changes.
      case AggKind::kSumCounts:
        return false;
    }
    return false;
  });
}

// How many times the aggregation's own input must exceed the outer side before restricting it to
// the keys that side binds is worth a build and a probe (RestrictToTheOuterKeys below).
//
// Four, which is low on purpose, because the outer number is an **upper bound** and can be far
// loose: on the query this gains most it is a semi join's 80,000,000 standing for the 868,888 rows
// the join really yields. A loose bound makes the test refuse where it would have paid, never fire
// where it would not, which is the safe direction for a rewrite that has to pay for itself.
constexpr int64_t kLeastRestriction = 4;

// Whether to restrict: both sides estimated, and the aggregation's input at least
// kLeastRestriction times the outer side. An unknown estimate refuses, as ADR 0032's gate does and
// for its reason -- a small outer side is what argues *for* the rewrite, so an unknown one has to
// argue against. The groups the restriction would avoid building are the quantity that would
// actually decide this, and nothing estimates a group count, so the rows standing in for them are
// the proxy and this is the whole of the evidence.
bool RestrictionIsWorth(const std::optional<int64_t>& outer, const std::optional<int64_t>& inner) {
  if (!outer.has_value() || !inner.has_value() || *outer <= 0 || *inner <= 0) {
    return false;
  }
  return Int128{*inner} >= Int128{kLeastRestriction} * Int128{*outer};
}

// Whether `node` reads a join or a union: the aggregation's own input holding one is what tells
// this pass that ADR 0029's filter cannot reach its scan, because the physical planner's offer
// survives neither on the way down -- a join forwards none to its probe, and a union's inputs are
// pipelines of their own (ADR 0036). A structural fact about the plan, which is the only kind
// available here -- and a total one, since a subtree with neither is exactly the chain an offer
// could travel.
bool ReadsAJoinOrUnion(const LogicalNodePtr& node) {
  if (node == nullptr) {
    return false;
  }
  if (std::holds_alternative<JoinNode>(*node) || std::holds_alternative<UnionAllNode>(*node)) {
    return true;
  }
  return std::ranges::any_of(InputsOf(*node), ReadsAJoinOrUnion);
}

// ADR 0023's **semi-join reduction**: `input` restricted to the rows whose correlated key the outer
// side binds, or `input` itself where the gate refuses or the pass cannot name a key.
//
// Path 2's substitution drops this restriction. The formalization's Lemma 4.2 puts a map where the
// domain D was, which keeps the answer and loses what D carried, "because a map restricts nothing"
// -- so the aggregation below builds a group for every key of its own table while the join above
// reads a thousandth of them. This puts the restriction back, as the ADR's own deferred item asks.
//
// **A semi join rather than an inner join to a de-duplicated domain**, and each reason earned its
// place in the measurements: it cannot multiply its probe's rows, so duplicate-freeness needs
// neither an argument nor a GROUP BY to enforce it; ADR 0022 fixes its build side to its right
// input, so the outer side is built whatever the estimates say, which is the difference between
// 179 MB and 5.5 GB on the query this is for; and it outputs its probe's columns alone, so the
// grouping above it reads exactly what it read before.
//
// The domain is a **clone** of the outer side, not the outer side: a plan is a tree, and the same
// node under both the join and this restriction is a column defined twice, which aborts
// (CloneWithFreshIds). So the outer side is computed twice, which the measurement includes.
//
// It removes only rows whose group no outer row reads, so no surviving group's value moves; and the
// join above matches each outer row to at most one group, so no row count above it moves either.
LogicalNodePtr RestrictToTheOuterKeys(LogicalNodePtr input, const DependentJoinNode& join,
                                      const std::map<ColumnId, ColumnId>& equivalent,
                                      const std::map<ColumnId, BoundColumn>& described,
                                      ColumnId& last_id) {
  // **One correlated column over a bare scan is ADR 0029's D5 to restrict, not this.** D5 publishes
  // the join's key set to the aggregation's own scan, which costs a filter rather than a build and
  // a probe, and it takes exactly one key. Where it can apply, this rewrite can only repeat it:
  // measured at SF 100 on the query whose correlation is a single key over a scan, restricting it
  // as well cost **1.420x** -- the aggregate's rows and groups did not move at all, because D5 had
  // already removed the same rows, while the clone re-read the 600-million-row join its outer side
  // is.
  //
  // **A join below the aggregation is the case where D5 cannot apply either**, and so is this
  // rewrite's as much as more than one key is. The reason is not the scan's one filter slot: a join
  // whose key is not publishable (CanPublishKeyFilter) leaves that slot free and D5 still cannot
  // reach the scan. What happens is that **the offer does not survive a join** --
  // physical_planner.cc's Builder threads an AggregateFilter down only through the nodes that keep
  // every row, and its JoinNode arm forwards no inherited offer to its probe, passing the join's
  // own instead. Past a pipeline breaker there is no offer at all, the aggregate then having no
  // part pipeline for one to travel.
  //
  // So the test is total rather than incidental: LogicalNode's multi-input alternatives are the two
  // joins and the union, and the offer crosses none of them (a union's arm drops it, its inputs
  // being pipelines of their own), so a subtree with no join and no union *is* the chain an offer
  // could travel, and ReadsAJoinOrUnion is false exactly where one could arrive.
  // src/exec/tests/aggregate_filter_test.cc pins that invariant, because threading the offer
  // through a join or a union is a natural change to make and would silently leave this gate paying
  // for a reduction D5 had already made.
  //
  // Measured on the query of the 22 shaped this way: **0.931x** and a peak of 483 MiB against
  // 1,011, its aggregation building 47,107 groups over 64,000 rows where it built 11,829,402 over
  // 16,042,800 -- with every other query of the 22 flat.
  //
  // What a plan may *not* read off is the filter itself: ADR 0029's filters are attached by the
  // physical planner, long after this pass runs, which is why these two structural facts stand in
  // for it. ADR 0030 states the hazard they guard against -- "before costing a shared sub-plan,
  // check whether a consumer is already pruned by one" -- and a wrong guess here is not a wrong
  // answer but a rewrite that pays twice for one reduction.
  if (equivalent.size() < 2 && !ReadsAJoinOrUnion(input)) {
    return input;
  }
  // **An outer side holding a dependent join is not cloned, and that is what bounds the copying.**
  // The pass is top-down, so a dependent join still in this one's left input is one this rewrite
  // has not reached: cloning it would duplicate a rewrite that clones in turn, and k stacked
  // correlated subqueries would cost 2^k nodes. Measured before this condition, on a query of k
  // conjuncts over one correlation: 45 plan lines at k=1, 1,533 at k=8 and 24,573 at k=12, where
  // planning alone took 1.3 s. With it, the outermost such join declines, then the next, down to
  // the innermost, whose left input holds none and which clones once -- so a chain costs one clone
  // and a plan's size stays linear in the rewrites, which is what ADR 0022's relation limit needs
  // in order to go on bounding every walk of a plan.
  //
  // It also makes the Lemma 3.1 check below unreachable by construction: a free variable can only
  // enter `join.left` through a dependent join, and the clone lands in the new join's right input,
  // where a free variable would fail the query with an internal error rather than decline.
  if (FirstDependentJoin(join.left) != nullptr || !FreeVariables(*join.left).empty()) {
    return input;
  }
  if (!RestrictionIsWorth(EstimateRows(*join.left), EstimateRows(*input))) {
    return input;
  }
  const std::vector<ColumnId> outer_ids = OutputIds(*join.left);
  const LogicalNodePtr domain = CloneWithFreshIds(join.left, last_id);
  const std::vector<ColumnId> domain_ids = OutputIds(*domain);
  if (domain_ids.size() != outer_ids.size()) {
    return input;  // a clone outputs what it cloned: nothing to key on if it does not
  }
  std::vector<JoinKey> keys;
  for (const auto& [outer_id, inner_id] : equivalent) {
    const auto at = std::ranges::find(outer_ids, outer_id);
    const auto inner = described.find(inner_id);
    const auto outer = described.find(outer_id);
    if (at == outer_ids.end() || inner == described.end() || outer == described.end() ||
        inner->second.type != outer->second.type) {
      return input;  // declining costs only speed: Path 2 without the restriction is the plan
    }
    BoundColumn key = outer->second;
    key.id = domain_ids[Narrow<std::size_t>(at - outer_ids.begin())];
    keys.push_back(JoinKey{.left = inner->second, .right = std::move(key)});
  }
  if (keys.empty()) {
    return input;
  }
  const std::optional<int64_t> rows = EstimateRows(*input);
  return Make(JoinNode{.kind = JoinKind::kSemi,
                       .left = std::move(input),
                       .right = domain,
                       .keys = std::move(keys),
                       .residual = {},
                       .build = BuildSide::kRight,
                       .estimate = rows,  // a semi join keeps a subset of its probe
                       .span = join.span});
}

// Path 2's rewrite (ADR 0023, the formalization's Lemmas 4.14 and 4.2): each outer column is
// replaced by its inner equivalent, so the correlated conjuncts read inner columns only and stay
// below the aggregation; an equality that became c = c is dropped. The aggregation groups by the
// inner equivalents, and the one-row dependent join becomes an inner join with one key per outer
// column the subquery reads.
//
//   Filter o.v <= s                      Filter o.v <= s
//     DependentJoin one-row                Join inner  keys o.k = <key of t.k>
//       Scan o                               Scan o
//       Aggregate m := max(t.x)              GroupAggregate keys t.k  m := max(t.x)
//         Filter t.k = o.k                     Scan t
//           Scan t
arrow::Result<LogicalNodePtr> Path2(const DependentJoinNode& join, ColumnId& last_id,
                                    int64_t build_side_ratio) {
  const auto refuse = [&join](std::string_view what) {
    return UnsupportedError(std::format("a correlated scalar subquery {} is not supported", what),
                            join.span);
  };
  const std::optional<ScalarBlock> block = ScalarBlockOf(join.right);
  if (!block.has_value()) {
    return refuse("over a block that is not an ungrouped aggregate over its correlation");
  }
  if (!StrictAggregates(block->aggregate->aggregates)) {
    return refuse("whose aggregate is not SUM, AVG, MIN or MAX");
  }
  for (const LogicalNodePtr& node : block->above) {
    const auto* compute = std::get_if<ComputeNode>(node.get());
    if (compute == nullptr) {
      continue;  // a Project passes columns through, which is strict
    }
    if (!std::ranges::all_of(compute->exprs, [](const ExprPtr& expr) { return IsStrict(*expr); })) {
      return refuse("whose select item does not combine its aggregate strictly");
    }
  }
  const std::vector<ColumnId> left_ids = OutputIds(*join.left);
  const std::set<ColumnId> outer(left_ids.begin(), left_ids.end());
  const std::vector<ColumnId> inner_ids = OutputIds(*block->below);
  const std::set<ColumnId> inner(inner_ids.begin(), inner_ids.end());

  // The classes, and the conjuncts that survive the substitution. An equality of two plain columns
  // of one type feeds a class; anything else is a conjunct that has to keep reading what it reads.
  Classes classes;
  std::vector<const Expr*> kept;
  // The name and type of each column a correlated conjunct names: BindBool's column_of stores them
  // in the PredicateExpr's own predicate, positionally, while the ids live in the operands. It is
  // the only place the pass can learn what to call a column.
  std::map<ColumnId, BoundColumn> described;
  for (const Predicate& predicate : block->filter->predicates) {
    if (predicate.kind != Predicate::Kind::kIsTrue || !predicate.column.has_value()) {
      return refuse("whose correlated conjunct the pass does not recognize");
    }
    const ExprPtr* defined = DefinedBy(*block->computed, predicate.column->id);
    if (defined == nullptr) {
      return refuse("whose correlated conjunct the pass does not recognize");
    }
    const Expr& expr = **defined;
    const auto* compare = std::get_if<PredicateExpr>(&expr.node);
    const ColumnExpr* a = compare == nullptr || compare->operands.size() != 2
                              ? nullptr
                              : std::get_if<ColumnExpr>(&compare->operands[0]->node);
    const ColumnExpr* b =
        a == nullptr ? nullptr : std::get_if<ColumnExpr>(&compare->operands[1]->node);
    const bool equality = b != nullptr &&
                          compare->predicate.kind == Predicate::Kind::kCompareColumns &&
                          compare->predicate.op == CompareOp::kEq;
    // One type on both ends, which KeyType answers for: it is the same question, and it excludes
    // DOUBLE, whose comparison is not exact.
    const bool one_type =
        equality && compare->operands[0]->type == compare->operands[1]->type &&
        KeyType(compare->operands[0]->type, compare->operands[1]->type).has_value();
    if (one_type) {
      const auto describe = [&described](ColumnId id, const std::optional<BoundColumn>& from,
                                         LogicalType type) {
        BoundColumn column = from.value_or(BoundColumn{});
        column.id = id;
        column.type = type;
        described.insert_or_assign(id, std::move(column));
      };
      describe(a->id, compare->predicate.column, compare->operands[0]->type);
      describe(b->id, compare->predicate.other, compare->operands[1]->type);
      classes.Merge(a->id, b->id);
      continue;  // becomes c = c after the substitution and is dropped
    }
    kept.push_back(&expr);
  }

  // Every outer column the subquery reads needs an inner equivalent, and gets its OWN key: one key
  // per class would drop a second outer column's constraint and keep rows SQL rejects.
  const std::vector<ColumnId> free = FreeVariables(*join.right);
  std::map<ColumnId, ColumnId> equivalent;  // outer id -> the inner column it is replaced by
  for (const ColumnId id : free) {
    ColumnId found = kNoColumnId;
    for (const ColumnId candidate : inner_ids) {
      if (classes.Same(id, candidate)) {
        found = candidate;
        break;
      }
    }
    if (found == kNoColumnId) {
      return refuse("with an outer column that has no inner equivalent");
    }
    equivalent.emplace(id, found);
  }
  if (equivalent.empty()) {
    return refuse("that reads no column of its enclosing block");
  }

  // The aggregation groups by the inner equivalents, each output as a new column that the join's
  // key then reads. Distinct, in a stable order.
  std::vector<ColumnId> group;
  for (const auto& [id, equiv] : equivalent) {
    if (std::ranges::find(group, equiv) == group.end()) {
      group.push_back(equiv);
    }
  }
  GroupAggregateNode grouped{.input = nullptr,
                             .keys = {},
                             .key_ids = {},
                             .aggregates = block->aggregate->aggregates,
                             .span = block->aggregate->span};
  std::map<ColumnId, ColumnId> key_of;  // the inner column -> the GroupAggregate's output for it
  for (const ColumnId id : group) {
    const auto column = described.find(id);
    if (column == described.end()) {
      return refuse("whose correlated key the pass cannot name");
    }
    grouped.keys.push_back(column->second);
    grouped.key_ids.push_back(ColumnId{std::to_underlying(last_id) + 1});
    last_id = grouped.key_ids.back();
    key_of.emplace(id, grouped.key_ids.back());
  }

  // The substitution: every outer id an expression reads becomes its inner equivalent's.
  const auto substitute = [&equivalent](const ExprPtr& expr) {
    return MapColumns(expr, [&equivalent](ColumnExpr column) {
      const auto it = equivalent.find(column.id);
      if (it != equivalent.end()) {
        column.id = it->second;
      }
      return column;
    });
  };
  LogicalNodePtr right = block->below;
  if (!kept.empty()) {
    std::vector<ExprPtr> exprs;
    std::vector<ColumnId> ids;
    std::vector<Predicate> predicates;
    for (const Expr* expr : kept) {
      exprs.push_back(substitute(std::make_shared<const Expr>(*expr)));
      ids.push_back(ColumnId{std::to_underlying(last_id) + 1});
      last_id = ids.back();
      predicates.push_back(Predicate{.kind = Predicate::Kind::kIsTrue,
                                     .column = BoundColumn{.id = ids.back(),
                                                           .name = exprs.back()->name,
                                                           .type = LogicalType::kBoolean},
                                     .span = block->filter->span});
    }
    right = Make(ComputeNode{
        .input = std::move(right), .exprs = exprs, .ids = ids, .span = block->computed->span});
    right = Make(FilterNode{
        .input = std::move(right), .predicates = predicates, .span = block->filter->span});
  }
  if (block->args != nullptr) {
    ComputeNode args = std::get<ComputeNode>(*block->args);
    for (ExprPtr& expr : args.exprs) {
      expr = substitute(expr);
    }
    args.input = std::move(right);
    right = Make(std::move(args));
  }
  // The keys as the join will name them, taken before `grouped` is moved into its node.
  const std::vector<BoundColumn> group_keys = grouped.keys;
  grouped.input = RestrictToTheOuterKeys(std::move(right), join, equivalent, described, last_id);
  right = Make(std::move(grouped));
  // The select item over the aggregate, and the select list, rebuilt above it. A Compute passes
  // its input's columns through, but a Project outputs only its own, so the group's key columns
  // are added to it: the join above reads them, and without them its key would name a column its
  // input does not output, which ResolvePositions turns into an abort.
  for (const LogicalNodePtr& node : std::ranges::reverse_view(block->above)) {
    if (const auto* project = std::get_if<ProjectNode>(node.get())) {
      ProjectNode out = *project;
      out.input = std::move(right);
      for (const ColumnId id : group) {
        BoundColumn column =
            group_keys[Narrow<std::size_t>(std::ranges::find(group, id) - group.begin())];
        column.id = key_of.at(id);
        out.ids.push_back(column.id);
        out.columns.push_back(std::move(column));
        if (!out.constants.empty()) {
          out.constants.emplace_back(std::nullopt);
        }
      }
      right = Make(std::move(out));
      continue;
    }
    right = WithInputs(*node, {std::move(right)});
  }

  // One key per outer column, the outer column equal to the group's output for its equivalent.
  std::vector<JoinKey> keys;
  for (const auto& [id, equiv] : equivalent) {
    const auto outer_column = described.find(id);
    if (outer_column == described.end()) {
      return refuse("whose correlated key the pass cannot name");
    }
    BoundColumn key = outer_column->second;
    BoundColumn group_key =
        group_keys[Narrow<std::size_t>(std::ranges::find(group, equiv) - group.begin())];
    group_key.id = key_of.at(equiv);
    if (key.type != group_key.type) {
      return refuse("whose correlated key has two types");
    }
    keys.push_back(JoinKey{.left = std::move(key), .right = std::move(group_key)});
  }
  // Path 2's join is an inner join, so either side may build: the aggregate it puts on the right
  // is often far larger than the outer rows that probe it (ADR 0026). Its estimate is its left
  // input's rows: the keys above are exactly the aggregate's group keys, which a group by makes
  // unique, so each outer row matches at most one group and the join cannot multiply its left
  // input. That is an upper bound rather than a count of the groups, which nothing estimates --
  // but leaving it unknown left every join above this rewrite reading its own left input as
  // unknown, which is what kept a lopsided join building on the wrong side (ADR 0026).
  const std::optional<int64_t> left_rows = EstimateRows(*join.left);
  const std::optional<int64_t> right_rows = EstimateRows(*right);
  LogicalNodePtr out =
      Make(JoinNode{.kind = JoinKind::kInner,
                    .left = join.left,
                    .right = std::move(right),
                    .keys = std::move(keys),
                    .residual = {},
                    .build = ChooseBuildSide(left_rows, right_rows, build_side_ratio),
                    .estimate = left_rows,
                    .span = join.span});
  if (!FreeVariables(*std::get<JoinNode>(*out).right).empty()) {
    return arrow::Status::NotImplemented(
        "the unnesting pass left a free variable in a rewritten join's right input");
  }
  return out;
}

// The pass, outermost first as the top-down algorithm goes: a dependent join is rewritten before
// anything inside it, and the rewrite's own inputs are then walked, so a dependent join nested in
// a correlated block is rewritten too (or refused at its own span, which is more useful than a
// blanket refusal at the outer one).
arrow::Result<LogicalNodePtr> UnnestNode(const LogicalNodePtr& node, ColumnId& last_id,
                                         int64_t build_side_ratio) {
  if (node == nullptr) {
    return node;
  }
  if (const auto* dependent = std::get_if<DependentJoinNode>(node.get())) {
    switch (dependent->kind) {
      case JoinKind::kSemi:
      case JoinKind::kAnti: {
        ARROW_ASSIGN_OR_RAISE(const LogicalNodePtr rewritten,
                              Path1(*dependent, last_id, build_side_ratio));
        return UnnestNode(rewritten, last_id, build_side_ratio);  // its inputs may hold more
      }
      case JoinKind::kOneRow: {
        ARROW_ASSIGN_OR_RAISE(const LogicalNodePtr rewritten,
                              Path2(*dependent, last_id, build_side_ratio));
        return UnnestNode(rewritten, last_id, build_side_ratio);
      }
      case JoinKind::kInner:
      case JoinKind::kLeft:
      case JoinKind::kNullAwareAnti:
        return UnsupportedError("a correlated subquery is not supported", dependent->span);
    }
  }
  std::vector<LogicalNodePtr> inputs = InputsOf(*node);
  bool changed = false;
  for (LogicalNodePtr& input : inputs) {
    if (input == nullptr) {
      continue;
    }
    ARROW_ASSIGN_OR_RAISE(LogicalNodePtr rewritten, UnnestNode(input, last_id, build_side_ratio));
    if (rewritten != input) {
      input = std::move(rewritten);
      changed = true;
    }
  }
  return changed ? WithInputs(*node, std::move(inputs)) : node;
}

arrow::Result<LogicalNodePtr> Unnest(const LogicalNodePtr& root, int64_t build_side_ratio) {
  if (FirstDependentJoin(root) == nullptr) {
    return root;
  }
  ColumnId last_id = MaxColumnId(*root);
  return UnnestNode(root, last_id, build_side_ratio);
}

LogicalNodePtr Prune(const LogicalNodePtr& node, Needed needed) {
  return std::visit(Pruner{.node = node, .needed = needed}, *node);
}

}  // namespace

arrow::Result<LogicalPlan> Optimize(const LogicalPlan& plan, const BindOptions& options) {
  if (plan.root == nullptr) {
    return plan;
  }
  ARROW_ASSIGN_OR_RAISE(LogicalNodePtr root, Unnest(plan.root, options.build_side_ratio));
  root = LimitBelowProject(DependentKeys(CountStarToRowCount(root), /*limited=*/false));
  // A grouped aggregate computed on one input of the join it reads, where the gate admits it
  // (docs/adr/0032-aggregate-placement.md's G2b). After the unnesting, so that a correlated
  // subquery's own aggregate is a candidate too, and before Prune, which then drops the columns
  // the aggregated side no longer has to carry.
  ColumnId last_exchanged = MaxColumnId(*root);
  root = ExchangeAggregates(root, last_exchanged);
  const std::vector<ColumnId> output = OutputIds(*root);
  root = Prune(root, Needed(output.begin(), output.end()));
  // A dependent join that Unnest left behind is a bug in the pass, never an unsupported query: no
  // SqlErrorDetail, so it exits 70 and cannot pass for a refusal (ADR 0005, ADR 0023). Unreachable
  // while Unnest rejects every dependent join; U2 and U3, which rewrite them, make it live.
  if (FirstDependentJoin(root) != nullptr) {
    return arrow::Status::NotImplemented("the unnesting pass left a dependent join in the plan");
  }
  return ResolvePositions({.root = std::move(root), .output = plan.output});
}

}  // namespace antb1::plan
