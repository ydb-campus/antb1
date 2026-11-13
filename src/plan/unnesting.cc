#include "unnesting.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
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

#include "join_order.h"
#include "key_type.h"

// The unnesting pass (ADR 0023; ADR 0041's decisions 16 to 20), the first and only fallible step of
// Optimize: it replaces every dependent join with ordinary joins, or rejects the query with exit
// code 4 at the subquery's span. Outermost first, as the top-down algorithm goes. The rules find
// columns by their ids and never read or set a position: plan::ResolvePositions sets every
// position at the end (ADR 0022).

namespace antb1::plan {
namespace {

LogicalNodePtr Make(LogicalNode node) {
  return std::make_shared<const LogicalNode>(std::move(node));
}

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

// A key of the rewritten join, where the conjunct is an equality of a pure-inner and a pure-outer
// side whose common type is not DOUBLE (ADR 0023's Path 1), or an IS NOT DISTINCT FROM of them,
// whose key is NULL-equal (ADR 0037's OJ5b). Both sides must be plain columns: an equality over an
// expression stays a residual, which is always correct and never wrong.
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
  bool null_equal = false;  // IS NOT DISTINCT FROM: a NULL matches a NULL
};

std::optional<KeyParts> AsKey(const Expr& expr, const std::set<ColumnId>& outer_ids) {
  const auto* predicate = std::get_if<PredicateExpr>(&expr.node);
  if (predicate == nullptr || predicate->operands.size() != 2) {
    return std::nullopt;
  }
  const bool null_equal = predicate->predicate.kind == Predicate::Kind::kIsNotDistinct;
  if (!null_equal && (predicate->predicate.kind != Predicate::Kind::kCompareColumns ||
                      predicate->predicate.op != CompareOp::kEq)) {
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
                  .outer_name = name_of(a_outer ? first : second, outer_expr),
                  .null_equal = null_equal};
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
                                           inner_cast_ids, key->inner_name),
                             .null_equal = key->null_equal});
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
  const bool chooses = Traits(join.kind).may_build_left;
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
      // The variance family is NULL over no rows too, but stays out of the rewrite until a query
      // needs it there (ADR 0037's AG2).
      case AggKind::kStddevSamp:
      case AggKind::kStddevPop:
      case AggKind::kVarSamp:
      case AggKind::kVarPop:
      // The binder wraps only an uncorrelated subquery in it, so no dependent join holds one; and
      // its check of the row count belongs to each binding, which grouping by the correlated keys
      // would lose (ADR 0041's decision 22).
      case AggKind::kSingleValue:
        return false;
    }
    return false;
  });
}

// Whether every column of `values` -- what the subquery outputs, which the comparison above the
// dependent join reads -- is NULL when the aggregation has no row to aggregate: an aggregate call
// (each SUM, AVG, MIN or MAX, StrictAggregates), or a strict combination (IsStrict) that reads at
// least one such column, through the Computes and Projects above the aggregation. The inner join
// gives an outer row with no group no row, which is SQL's answer only where the subquery's value is
// NULL there, so that the comparison rejects the row. IsStrict alone does not say it: a constant is
// strict and reads no call, and so is the select item of `SELECT 1 ... ORDER BY COUNT(*)`, whose
// aggregation is left with no call at all -- 1 over no rows, not NULL.
bool NullOverNoRows(const ScalarBlock& block, const std::vector<ColumnId>& values) {
  std::set<ColumnId> nulls;
  for (const AggregateCall& call : block.aggregate->aggregates) {
    nulls.insert(call.id);
  }
  for (const LogicalNodePtr& node : std::ranges::reverse_view(block.above)) {
    if (const auto* compute = std::get_if<ComputeNode>(node.get())) {
      for (std::size_t i = 0; i < compute->exprs.size(); ++i) {
        std::vector<ColumnId> reads;
        CollectColumnIds(*compute->exprs[i], reads);
        if (IsStrict(*compute->exprs[i]) &&
            std::ranges::any_of(reads, [&nulls](ColumnId id) { return nulls.contains(id); })) {
          nulls.insert(compute->ids[i]);
        }
      }
      continue;
    }
    const auto* project = std::get_if<ProjectNode>(node.get());
    if (project == nullptr) {
      return false;  // ScalarBlockOf admits Computes and Projects only
    }
    for (std::size_t i = 0; i < project->columns.size() && i < project->ids.size(); ++i) {
      const bool constant = i < project->constants.size() && project->constants[i].has_value();
      if (!constant && nulls.contains(project->columns[i].id)) {
        nulls.insert(project->ids[i]);
      }
    }
  }
  return !values.empty() &&
         std::ranges::all_of(values, [&nulls](ColumnId id) { return nulls.contains(id); });
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
  if (std::holds_alternative<JoinNode>(*node) || std::holds_alternative<UnionAllNode>(*node) ||
      std::holds_alternative<SetOperationNode>(*node)) {
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
                                      const std::set<ColumnId>& null_equal,
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
  // joins, the union and the set operation (ADR 0039), and the offer crosses none of them (a
  // union's and a set operation's arms drop it, their inputs being pipelines of their own), so a
  // subtree with no join and no union *is* the chain an offer
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
    // The key the join above takes, NULL-equal where that one is: a NULL group is read by an outer
    // row whose column is NULL, so it must not be restricted away.
    keys.push_back(JoinKey{.left = inner->second,
                           .right = std::move(key),
                           .null_equal = null_equal.contains(outer_id)});
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
  if (!NullOverNoRows(*block, OutputIds(*join.right))) {
    return refuse("whose select item does not combine its aggregate strictly");
  }
  const std::vector<ColumnId> left_ids = OutputIds(*join.left);
  const std::set<ColumnId> outer(left_ids.begin(), left_ids.end());
  const std::vector<ColumnId> inner_ids = OutputIds(*block->below);
  const std::set<ColumnId> inner(inner_ids.begin(), inner_ids.end());

  // The classes, and the conjuncts that survive the substitution. An equality of two plain columns
  // of one type feeds a class; anything else is a conjunct that has to keep reading what it reads.
  Classes classes;
  std::vector<const Expr*> kept;
  // The equalities that fed a class, with their two columns: each is dropped below only where the
  // substitution makes it c = c, and kept as an inner condition otherwise.
  struct Equality {
    const Expr* expr = nullptr;
    ColumnId a = kNoColumnId;
    ColumnId b = kNoColumnId;
  };
  std::vector<Equality> equalities;
  // The correlated IS NOT DISTINCT FROM conjuncts between an outer and an inner column of one type
  // (ADR 0037's OJ5b). They feed no class, since the classes' members are equal values and these
  // also match NULL to NULL; an outer column with no equivalent in a class takes its inner column
  // from one instead, and its key is then NULL-equal. The substitution stays exact: a NULL is
  // replaced by a NULL, and a value by the same value.
  struct NullEqualLink {
    const Expr* expr = nullptr;
    ColumnId outer = kNoColumnId;
    ColumnId inner = kNoColumnId;
  };
  std::vector<NullEqualLink> links;
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
    const bool not_distinct = b != nullptr &&
                              compare->predicate.kind == Predicate::Kind::kIsNotDistinct &&
                              outer.contains(a->id) != outer.contains(b->id);
    // One type on both ends, which KeyType answers for: it is the same question, and it excludes
    // DOUBLE, whose comparison is not exact.
    const bool one_type =
        (equality || not_distinct) && compare->operands[0]->type == compare->operands[1]->type &&
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
      if (not_distinct) {
        const bool a_outer = outer.contains(a->id);
        links.push_back(NullEqualLink{
            .expr = &expr, .outer = a_outer ? a->id : b->id, .inner = a_outer ? b->id : a->id});
        continue;
      }
      classes.Merge(a->id, b->id);
      equalities.push_back(Equality{.expr = &expr, .a = a->id, .b = b->id});
      continue;
    }
    kept.push_back(&expr);
  }

  // Every outer column the subquery reads needs an inner equivalent, and gets its OWN key: one key
  // per class would drop a second outer column's constraint and keep rows SQL rejects.
  const std::vector<ColumnId> free = FreeVariables(*join.right);
  std::map<ColumnId, ColumnId> equivalent;  // outer id -> the inner column it is replaced by
  std::set<ColumnId> null_equal;            // the outer ids whose key is NULL-equal
  std::set<const NullEqualLink*> used_links;
  for (const ColumnId id : free) {
    ColumnId found = kNoColumnId;
    for (const ColumnId candidate : inner_ids) {
      if (classes.Same(id, candidate)) {
        found = candidate;
        break;
      }
    }
    if (found == kNoColumnId) {
      const NullEqualLink* used = nullptr;
      for (const NullEqualLink& link : links) {
        if (link.outer == id && inner.contains(link.inner)) {
          used = &link;
          break;
        }
      }
      if (used != nullptr) {
        found = used->inner;
        null_equal.insert(id);
        used_links.insert(used);
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
  // An equality the substitution turns into c = c says no more than the key that replaces it: it is
  // dropped. Any other one is a condition between inner columns -- two inner columns equal to one
  // outer column, of which only the first is its equivalent, must still equal each other -- and is
  // kept, substituted like every kept conjunct.
  //
  // So is one over an outer column whose key is NULL-equal. Dropping c = c is right only where c
  // cannot be NULL in a group an outer row reads (NK15 p. 395), which an `=` key ensures: it never
  // matches a NULL. A NULL-equal key does match the NULL group, so `o.x = o.y` (or `o.x = o.x`)
  // over two outer columns that only IS NOT DISTINCT FROM links to an inner column would let an
  // outer row whose columns are NULL read that group, where the equality is NULL in SQL and the
  // subquery sees no row. Kept, `c = c` leaves the NULL rows out of the aggregation.
  const auto substituted = [&equivalent](ColumnId id) {
    const auto it = equivalent.find(id);
    return it == equivalent.end() ? id : it->second;
  };
  for (const Equality& equality : equalities) {
    if (substituted(equality.a) != substituted(equality.b) || null_equal.contains(equality.a) ||
        null_equal.contains(equality.b)) {
      kept.push_back(equality.expr);
    }
  }
  // A link that gave its outer column the equivalent says no more than the NULL-equal key that
  // replaces it; any other is a condition between inner columns once substituted, and is kept.
  for (const NullEqualLink& link : links) {
    if (!used_links.contains(&link)) {
      kept.push_back(link.expr);
    }
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
  grouped.input =
      RestrictToTheOuterKeys(std::move(right), join, equivalent, null_equal, described, last_id);
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
    keys.push_back(JoinKey{.left = std::move(key),
                           .right = std::move(group_key),
                           .null_equal = null_equal.contains(id)});
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
      case JoinKind::kFull:
      case JoinKind::kNullAwareMark:
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

}  // namespace

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

arrow::Result<LogicalNodePtr> Unnest(const LogicalNodePtr& root, int64_t build_side_ratio) {
  if (FirstDependentJoin(root) == nullptr) {
    return root;
  }
  ColumnId last_id = MaxColumnId(*root);
  return UnnestNode(root, last_id, build_side_ratio);
}

}  // namespace antb1::plan
