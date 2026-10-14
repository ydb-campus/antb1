#include "antb1/plan/optimizer.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "antb1/common/check.h"
#include "antb1/common/narrow.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/sql_status.h"

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
// GroupAggregate ask their input for exactly what they read, and a Join asks both inputs for the
// request plus its keys and residual columns (each input keeps the ones it outputs).
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
    for (const Predicate& p : filter.predicates) {
      if (p.column.has_value()) {
        Need(needed, *p.column);
      }
      if (p.other.has_value()) {
        Need(needed, *p.other);
      }
    }
    return Over(std::move(needed));
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
    for (const JoinKey& key : join.keys) {
      Need(needed, key.left);
      Need(needed, key.right);
    }
    for (const ExprPtr& conjunct : join.residual) {
      std::vector<ColumnId> reads;
      CollectColumnIds(*conjunct, reads);
      needed.insert(reads.begin(), reads.end());
    }
    return WithInputs(*node, {Prune(join.left, needed), Prune(join.right, needed)});
  }
  LogicalNodePtr operator()(const DependentJoinNode& /*join*/) const {
    // Prune nothing through a dependent join: its right input reads columns of its left input as
    // free variables, which no walk of the right input can see, so pruning the left input by what
    // the nodes above it need would drop them. Unreachable in a plan that Optimize returns, since
    // the unnesting pass runs first and replaces every dependent join or rejects the query; a plan
    // test that prunes a hand-built one gets this rule.
    return node;
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
  return KeyParts{.inner = a_outer ? *b : *a,
                  .outer = a_outer ? *a : *b,
                  .inner_type = inner_expr.type,
                  .outer_type = outer_expr.type,
                  .key_type = *key};
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
arrow::Result<LogicalNodePtr> Path1(const DependentJoinNode& join, ColumnId& last_id) {
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
  std::vector<Predicate> probe_filter;  // an EXISTS's outer-only conjuncts, below the semi join
  std::vector<ExprPtr> inner_casts;     // a key's inner side widened to the key type
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
      // ADR 0023: an outer-only conjunct stays a residual of an anti join -- where it is false or
      // NULL the subquery has no row, and NOT EXISTS keeps the outer row, which a filter on the
      // outer rows would drop. In an EXISTS the same reasoning makes it a filter on the probe
      // side, below the join.
      if (join.kind == JoinKind::kAnti) {
        residual.push_back(expr);
      } else {
        probe_filter.push_back(predicate);
      }
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
                                          outer_cast_ids, "outer"),
                             .right = side(key->inner, key->inner_type, key->key_type, inner_casts,
                                           inner_cast_ids, "inner")});
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
  if (!probe_filter.empty()) {
    left =
        Make(FilterNode{.input = std::move(left), .predicates = probe_filter, .span = join.span});
  }
  if (!outer_casts.empty()) {
    left = Make(ComputeNode{
        .input = std::move(left), .exprs = outer_casts, .ids = outer_cast_ids, .span = join.span});
  }
  LogicalNodePtr right = block->below;
  if (!inner_casts.empty()) {
    right = Make(ComputeNode{
        .input = std::move(right), .exprs = inner_casts, .ids = inner_cast_ids, .span = join.span});
  }
  LogicalNodePtr out = Make(JoinNode{.kind = join.kind,
                                     .left = std::move(left),
                                     .right = std::move(right),
                                     .keys = std::move(keys),
                                     .residual = std::move(residual),
                                     .build = BuildSide::kRight,
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

// The pass, outermost first as the top-down algorithm goes: a dependent join is rewritten before
// anything inside it, and the rewrite's own inputs are then walked, so a dependent join nested in
// a correlated block is rewritten too (or refused at its own span, which is more useful than a
// blanket refusal at the outer one).
arrow::Result<LogicalNodePtr> UnnestNode(const LogicalNodePtr& node, ColumnId& last_id) {
  if (node == nullptr) {
    return node;
  }
  if (const auto* dependent = std::get_if<DependentJoinNode>(node.get())) {
    switch (dependent->kind) {
      case JoinKind::kSemi:
      case JoinKind::kAnti: {
        ARROW_ASSIGN_OR_RAISE(const LogicalNodePtr rewritten, Path1(*dependent, last_id));
        return UnnestNode(rewritten, last_id);  // its inputs may hold more
      }
      case JoinKind::kOneRow:  // Path 2, roadmap id U3
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
    ARROW_ASSIGN_OR_RAISE(LogicalNodePtr rewritten, UnnestNode(input, last_id));
    if (rewritten != input) {
      input = std::move(rewritten);
      changed = true;
    }
  }
  return changed ? WithInputs(*node, std::move(inputs)) : node;
}

arrow::Result<LogicalNodePtr> Unnest(const LogicalNodePtr& root) {
  if (FirstDependentJoin(root) == nullptr) {
    return root;
  }
  ColumnId last_id = MaxColumnId(*root);
  return UnnestNode(root, last_id);
}

LogicalNodePtr Prune(const LogicalNodePtr& node, Needed needed) {
  return std::visit(Pruner{.node = node, .needed = needed}, *node);
}

}  // namespace

arrow::Result<LogicalPlan> Optimize(const LogicalPlan& plan) {
  if (plan.root == nullptr) {
    return plan;
  }
  ARROW_ASSIGN_OR_RAISE(LogicalNodePtr root, Unnest(plan.root));
  root = LimitBelowProject(DependentKeys(CountStarToRowCount(root), /*limited=*/false));
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
