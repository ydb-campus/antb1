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
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "antb1/common/check.h"
#include "antb1/common/narrow.h"
#include "antb1/plan/branch_truth.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/sql_status.h"

#include "join_order.h"
#include "key_type.h"
#include "strictness.h"

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

// ---- the pieces the rules share ----

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

// ---- the rule table (ADR 0041's decisions 16 to 18) ----

// How a dependent join's kind is unnested, read from its traits, never from its name (decision 16).
enum class Mode : std::uint8_t {
  // A join that keeps or drops left rows and reads none of its right input's columns, and that can
  // evaluate the correlated conjuncts over candidate pairs (semi and anti): they move into it, as
  // ADR
  // 0023's Path 1 has it.
  kLift,
  // A join that appends its right input's one row (one-row): a static aggregate grouped by the
  // representatives of the outer columns, by substitution, as ADR 0023's Path 2 has it.
  kValue,
};

// std::nullopt where the pass answers no correlated subquery of the kind: a null-aware anti join
// and a mark take no residual, so their correlation cannot move into them (SQ8 and SQ12), and the
// binder makes no dependent inner, LEFT or FULL join.
std::optional<Mode> ModeOf(JoinKind kind) {
  const JoinKindTraits traits = Traits(kind);
  if (traits.one_row) {
    return Mode::kValue;
  }
  if (traits.filters_left && traits.residuals) {
    return Mode::kLift;
  }
  return std::nullopt;
}

// What every rule reads of the dependent join it unnests (decision 16).
struct Correlation {
  const DependentJoinNode& join;
  Mode mode;
  std::set<ColumnId> outer;  // the left input's output
  ColumnId& last_id;
  int64_t build_side_ratio;
  std::span<const LogicalNode* const> above;  // the plan's root down to the join's parent
  UnnestReport* report = nullptr;

  // The refusal of a shape, in the wording each mode has always had, at the subquery's span.
  [[nodiscard]] arrow::Status Refuse(std::string_view what) const {
    if (mode == Mode::kValue) {
      return UnsupportedError(std::format("a correlated scalar subquery {} is not supported", what),
                              join.span);
    }
    return UnsupportedError(
        std::format("a correlated {} {} is not supported",
                    Traits(join.kind).drops_unmatched_left ? "EXISTS" : "NOT EXISTS", what),
        join.span);
  }
  // A node on the way to the correlated filter that the mode has no rule for. An aggregate, GROUP
  // BY, HAVING, LIMIT or OFFSET leaves one; the binder refuses those statically, so this is the
  // backstop for a shape it has not met.
  [[nodiscard]] arrow::Status RefuseShape() const {
    return Refuse(mode == Mode::kValue
                      ? "over a block that is not an ungrouped aggregate over its correlation"
                      : "over a block that is not a plain FROM and WHERE");
  }
};

// What a node between the dependent join and its correlated filter is to the rewrite (decision 17).
enum class Role : std::uint8_t {
  kDropped,  // lift: the select list, its computed items or its sort, which the join reads none of
  kMap,      // value: a Project or Compute over the static aggregate, rebuilt above it
  kStatic,   // value: the ungrouped Aggregate, grouped by the representatives
  kArguments,  // value: the Compute of the aggregate's arguments, substituted
};

struct Step {
  LogicalNodePtr node;
  Role role = Role::kDropped;
};

// The way from the dependent join down to its correlated filter, as the descent found it.
struct Spine {
  std::vector<Step> steps;                // top-down
  const FilterNode* filter = nullptr;     // the correlated conjuncts, as kIsTrue over `computed`
  const ComputeNode* computed = nullptr;  // their expressions
  LogicalNodePtr below;                   // the block's own joins, kept whole
};

// The descent: one rule per node type, and no other overload, so that a new node type does not
// compile until it has its rule (decision 17). Each rule records the node and goes on into its
// input, ends the descent at the correlated filter, or declines, which refuses the shape. The first
// Filter over a Compute is the correlated one: the binder pulls every correlated conjunct of a
// block up into it, so nothing below it reads an outer column, which Lemma 3.1's check after the
// rewrite verifies.
struct Descent {
  enum class Stage : std::uint8_t { kAbove, kUnderStatic, kUnderArguments };
  enum class Next : std::uint8_t { kInput, kFound, kDeclined };

  Mode mode;
  Stage stage = Stage::kAbove;
  Spine spine;
  LogicalNodePtr node;  // the node the rule is called on

  Next Record(Role role) {
    spine.steps.push_back(Step{.node = node, .role = role});
    return Next::kInput;
  }
  // A correlated filter where the stage admits one: lift mode above everything, value mode under
  // the static aggregate.
  Next Filter(const FilterNode& filter) {
    const bool admitted = mode == Mode::kLift ? stage == Stage::kAbove : stage != Stage::kAbove;
    const auto* computed = std::get_if<ComputeNode>(filter.input.get());
    if (!admitted || computed == nullptr) {
      return Next::kDeclined;
    }
    spine.filter = &filter;
    spine.computed = computed;
    spine.below = computed->input;
    return Next::kFound;
  }

  Next operator()(const FilterNode& filter) { return Filter(filter); }
  Next operator()(const ProjectNode& /*project*/) {
    if (stage != Stage::kAbove) {
      return Next::kDeclined;
    }
    return Record(mode == Mode::kLift ? Role::kDropped : Role::kMap);
  }
  Next operator()(const ComputeNode& /*compute*/) {
    switch (stage) {
      case Stage::kAbove:
        return Record(mode == Mode::kLift ? Role::kDropped : Role::kMap);
      case Stage::kUnderStatic:
        stage = Stage::kUnderArguments;
        return Record(Role::kArguments);
      case Stage::kUnderArguments:
        return Next::kDeclined;
    }
    return Next::kDeclined;
  }
  // An ORDER BY nothing limits orders rows no semi or anti join reads in order. A scalar block's
  // ORDER BY over one aggregated row is one the binder never leaves.
  Next operator()(const SortNode& /*sort*/) {
    return mode == Mode::kLift && stage == Stage::kAbove ? Record(Role::kDropped) : Next::kDeclined;
  }
  // The static aggregate of a scalar subquery (decision 17's "Aggregate (static)" row, INNER until
  // SQ6); an EXISTS over one is SQ8's.
  Next operator()(const AggregateNode& /*aggregate*/) {
    if (mode != Mode::kValue || stage != Stage::kAbove) {
      return Next::kDeclined;
    }
    stage = Stage::kUnderStatic;
    return Record(Role::kStatic);
  }
  // SQ8 and SQ14 (LIMIT), SQ8 and SQ4b (GROUP BY), SQ16 (grouping sets), SQ15 (set operations),
  // SQ8 and SQ10 (joins on the way, decision 17's join rule), and the leaves and a nested dependent
  // join, which stand below a correlated filter, never above one.
  Next operator()(const LimitNode& /*limit*/) { return Next::kDeclined; }
  Next operator()(const GroupAggregateNode& /*group*/) { return Next::kDeclined; }
  Next operator()(const GroupingSetsNode& /*sets*/) { return Next::kDeclined; }
  Next operator()(const UnionAllNode& /*union_all*/) { return Next::kDeclined; }
  Next operator()(const SetOperationNode& /*set_operation*/) { return Next::kDeclined; }
  Next operator()(const JoinNode& /*join*/) { return Next::kDeclined; }
  Next operator()(const DependentJoinNode& /*join*/) { return Next::kDeclined; }
  Next operator()(const ScanNode& /*scan*/) { return Next::kDeclined; }
  Next operator()(const RowCountNode& /*row_count*/) { return Next::kDeclined; }
};

// The spine of `root`, a dependent join's right input, or std::nullopt where a rule declines.
std::optional<Spine> Descend(Mode mode, const LogicalNodePtr& root) {
  Descent descent{.mode = mode, .stage = Descent::Stage::kAbove, .spine = {}, .node = root};
  while (descent.node != nullptr) {
    switch (std::visit(descent, *descent.node)) {
      case Descent::Next::kFound:
        return std::move(descent.spine);
      case Descent::Next::kDeclined:
        return std::nullopt;
      case Descent::Next::kInput:
        break;
    }
    const std::vector<LogicalNodePtr> inputs = InputsOf(*descent.node);
    if (inputs.size() != 1) {
      return std::nullopt;
    }
    descent.node = inputs.front();
  }
  return std::nullopt;
}

// ---- the static aggregate's rule: its calls and their empty values (decisions 17 and 19) ----

// Whether the rule answers a static aggregate's call. COUNT in any spelling is 0 over no rows,
// which the INNER join back loses (the COUNT bug, R. A. Ganski and H. K. T. Wong, SIGMOD 1987); the
// LEFT join back that answers it is SQ6's.
bool AnsweredCall(AggKind kind) {
  switch (kind) {
    case AggKind::kSum:
    case AggKind::kAvg:
    case AggKind::kMin:
    case AggKind::kMax:
      return true;
    case AggKind::kCountStar:
    case AggKind::kCount:
    case AggKind::kCountDistinct:
    // A sum of counts is a count for this rule's purpose, and for its stated reason: it answers 0
    // over no rows, by design (ADR 0032's G2c). Unreachable today, since the unnesting runs before
    // the exchange that makes one, and correct if that ever changes.
    case AggKind::kSumCounts:
    // The variance family is NULL over no rows too, but stays out of the rewrite until a query
    // needs it there (ADR 0037's AG2).
    case AggKind::kStddevSamp:
    case AggKind::kStddevPop:
    case AggKind::kVarSamp:
    case AggKind::kVarPop:
    // The binder wraps only an uncorrelated subquery in it, so no dependent join holds one; and its
    // check of the row count belongs to each binding, which grouping by the correlated keys would
    // lose (ADR 0041's decision 22).
    case AggKind::kSingleValue:
      return false;
  }
  return false;
}

bool IsNull(const EmptyValue& value) {
  return value.has_value() && std::holds_alternative<std::monostate>(value->value);
}

// The empty value of each column the static aggregate and the maps above it define (decision 19):
// each call's, then, bottom-up through the maps, NULL for a strict combination (IsStrict) that
// reads at least one NULL, a Project's column its source's, and not known otherwise. IsStrict alone
// does not make a NULL: a constant is strict and reads no call, and so is the select item of
// `SELECT 1
// ... ORDER BY COUNT(*)`, whose aggregation is left with no call at all -- 1 over no rows, not
// NULL (ADR 0023's update of 2026-11-08). A constant item keeps its constant, a NULL one excepted,
// which no select list the binder leaves here holds.
std::map<ColumnId, EmptyValue> EmptyValues(const AggregateNode& aggregate, const Spine& spine) {
  std::map<ColumnId, EmptyValue> values;
  for (const AggregateCall& call : aggregate.aggregates) {
    values.insert_or_assign(call.id, EmptyValueOf(call));
  }
  for (const Step& step : std::ranges::reverse_view(spine.steps)) {
    if (step.role != Role::kMap) {
      continue;
    }
    if (const auto* compute = std::get_if<ComputeNode>(step.node.get())) {
      for (std::size_t i = 0; i < compute->exprs.size(); ++i) {
        std::vector<ColumnId> reads;
        CollectColumnIds(*compute->exprs[i], reads);
        const bool null =
            IsStrict(*compute->exprs[i]) && std::ranges::any_of(reads, [&](ColumnId id) {
              const auto it = values.find(id);
              return it != values.end() && IsNull(it->second);
            });
        values.insert_or_assign(
            compute->ids[i],
            null ? EmptyValue(Constant{.type = compute->exprs[i]->type, .value = std::monostate{}})
                 : EmptyValue());
      }
      continue;
    }
    const auto& project = std::get<ProjectNode>(*step.node);
    for (std::size_t i = 0; i < project.columns.size() && i < project.ids.size(); ++i) {
      const std::optional<Constant>* entry =
          i < project.constants.size() ? &project.constants[i] : nullptr;
      if (const auto* constant = entry != nullptr && entry->has_value() ? &**entry : nullptr;
          constant != nullptr) {
        values.insert_or_assign(project.ids[i],
                                std::holds_alternative<std::monostate>(constant->value)
                                    ? EmptyValue()
                                    : EmptyValue(*constant));
        continue;
      }
      const auto it = values.find(project.columns[i].id);
      values.insert_or_assign(project.ids[i], it == values.end() ? EmptyValue() : it->second);
    }
  }
  return values;
}

// The static aggregate's rule in value mode: its calls, then the maps over it, then its value. The
// INNER join back gives an outer row with no group no row, which is SQL's answer only where the
// subquery's value is NULL there, so that the comparison rejects the row.
arrow::Status CheckStatic(const Correlation& correlation, const Spine& spine,
                          std::map<ColumnId, EmptyValue>& values) {
  const AggregateNode* aggregate = nullptr;
  for (const Step& step : spine.steps) {
    if (step.role == Role::kStatic) {
      aggregate = &std::get<AggregateNode>(*step.node);
    }
  }
  ANTB1_CHECK(aggregate != nullptr);  // the descent finds the filter in value mode only under one
  if (!std::ranges::all_of(aggregate->aggregates,
                           [](const AggregateCall& call) { return AnsweredCall(call.kind); })) {
    return correlation.Refuse("whose aggregate is not SUM, AVG, MIN or MAX");
  }
  // Only a strict combination (strictness.h) keeps the NULL of SUM, AVG, MIN and MAX over no rows:
  // CASE, COALESCE, OR, AND and some functions turn a NULL into a value, so the comparison could
  // accept a row the inner join drops -- a silently wrong answer, not a refusal (the COUNT bug,
  // R. A. Ganski and H. K. T. Wong, SIGMOD 1987).
  for (const Step& step : spine.steps) {
    const auto* compute = std::get_if<ComputeNode>(step.node.get());
    if (step.role == Role::kMap && compute != nullptr &&
        !std::ranges::all_of(compute->exprs, [](const ExprPtr& expr) { return IsStrict(*expr); })) {
      return correlation.Refuse("whose select item does not combine its aggregate strictly");
    }
  }
  values = EmptyValues(*aggregate, spine);
  const std::vector<ColumnId> outputs = OutputIds(*correlation.join.right);
  if (outputs.empty() || !std::ranges::all_of(outputs, [&values](ColumnId id) {
        const auto it = values.find(id);
        return it != values.end() && IsNull(it->second);
      })) {
    return correlation.Refuse("whose select item does not combine its aggregate strictly");
  }
  return arrow::Status::OK();
}

// ---- the correlated filter's rule (decision 17's first row) ----

// Each conjunct of the correlated filter, as its Compute defines it, in filter order; a conjunct
// that is no kIsTrue over a computed column is refused.
arrow::Result<std::vector<const ExprPtr*>> ConjunctsOf(const Correlation& correlation,
                                                       const Spine& spine) {
  std::vector<const ExprPtr*> conjuncts;
  for (const Predicate& predicate : spine.filter->predicates) {
    const ExprPtr* defined =
        predicate.kind == Predicate::Kind::kIsTrue && predicate.column.has_value()
            ? DefinedBy(*spine.computed, predicate.column->id)
            : nullptr;
    if (defined == nullptr) {
      return correlation.Refuse("whose correlated conjunct the pass does not recognize");
    }
    conjuncts.push_back(defined);
  }
  return conjuncts;
}

// Lift mode's join (ADR 0023's Path 1; the formalization's Lemma 4.8 then Lemma 3.1): the steps are
// dropped, since the join reads none of their columns, and the conjuncts of the correlated filter
// move into the join, which becomes a semi join (EXISTS) or a plain anti join (NOT EXISTS, which
// has no NULL case, unlike NOT IN). Every outer reference moves with them, so substitution has
// nothing left to replace. Its build side is chosen by the estimates where the kind may build on
// its left.
//
//   DependentJoin SEMI                     Join SEMI  keys o.k = t.k  residual t.x < o.x
//     <outer>                                <outer>
//     Project ...                            Filter t.z = 1
//       Filter <correlated conjuncts>          Scan t
//         Compute <their expressions>
//           Filter t.z = 1
//             Scan t
arrow::Result<LogicalNodePtr> LiftJoin(const Correlation& correlation, const Spine& spine) {
  const DependentJoinNode& join = correlation.join;
  ColumnId& last_id = correlation.last_id;
  std::vector<JoinKey> keys;
  std::vector<ExprPtr> residual;
  std::vector<ExprPtr> inner_casts;  // a key's inner side widened to the key type
  std::vector<ColumnId> inner_cast_ids;
  std::vector<ExprPtr> outer_casts;  // and its outer side
  std::vector<ColumnId> outer_cast_ids;

  ARROW_ASSIGN_OR_RAISE(const std::vector<const ExprPtr*> conjuncts,
                        ConjunctsOf(correlation, spine));
  for (const ExprPtr* defined : conjuncts) {
    const ExprPtr& expr = *defined;
    const Sides sides = SidesOf(*expr, correlation.outer);
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
    if (const std::optional<KeyParts> key = AsKey(*expr, correlation.outer)) {
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
    // candidate pairs need a key (ADR 0040's decision 3).
    return correlation.Refuse("without a key");
  }

  LogicalNodePtr left = join.left;
  if (!outer_casts.empty()) {
    left = Make(ComputeNode{
        .input = std::move(left), .exprs = outer_casts, .ids = outer_cast_ids, .span = join.span});
  }
  LogicalNodePtr right = spine.below;
  if (!inner_casts.empty()) {
    right = Make(ComputeNode{
        .input = std::move(right), .exprs = inner_casts, .ids = inner_cast_ids, .span = join.span});
  }
  // ADR 0026's C2b, C2c and C3: a semi or an anti join may build on its left input, the side it
  // preserves, when the subquery side is decisively larger -- the shape a correlated EXISTS or NOT
  // EXISTS over a large relation has, where ADR 0022's rule built the whole of that relation to
  // probe it with far fewer rows.
  const std::optional<int64_t> left_rows = EstimateRows(*left);
  const BuildSide build =
      Traits(join.kind).may_build_left
          ? ChooseBuildSide(left_rows, EstimateRows(*right), correlation.build_side_ratio)
          : BuildSide::kRight;
  return Make(JoinNode{.kind = join.kind,
                       .left = std::move(left),
                       .right = std::move(right),
                       .keys = std::move(keys),
                       .residual = std::move(residual),
                       .build = build,
                       // A semi or an anti join keeps a subset of its left input, so that is an
                       // upper bound: without it every join above this rewrite would read its own
                       // left input as unknown.
                       .estimate = left_rows,
                       .span = join.span});
}

// The equalities that fed a class, with their two columns: each is dropped where the substitution
// makes it c = c, and kept as an inner condition otherwise.
struct Equality {
  const Expr* expr = nullptr;
  ColumnId a = kNoColumnId;
  ColumnId b = kNoColumnId;
};

// A correlated IS NOT DISTINCT FROM between an outer and an inner column of one type (ADR 0037's
// OJ5b). It feeds no class, since the classes' members are equal values and these also match NULL
// to NULL; an outer column with no equivalent in a class takes its inner column from one instead,
// and its key is then NULL-equal. The substitution stays exact: a NULL is replaced by a NULL, and a
// value by the same value.
struct NullEqualLink {
  const Expr* expr = nullptr;
  ColumnId outer = kNoColumnId;
  ColumnId inner = kNoColumnId;
};

// Value mode's reading of the correlated filter: the classes, and the conjuncts that survive the
// substitution. An equality of two plain columns of one type feeds a class; anything else is a
// conjunct that has to keep reading what it reads.
struct Equivalences {
  Classes classes;
  std::vector<Equality> equalities;
  std::vector<NullEqualLink> links;
  // The name and type of each column a correlated conjunct names: BindBool's column_of stores them
  // in the PredicateExpr's own predicate, positionally, while the ids live in the operands. It is
  // the only place the pass can learn what to call a column; the last conjunct to name one names
  // it.
  std::map<ColumnId, BoundColumn> described;
  std::vector<const Expr*> kept;
};

arrow::Result<Equivalences> EquivalencesOf(const Correlation& correlation, const Spine& spine) {
  Equivalences out;
  ARROW_ASSIGN_OR_RAISE(const std::vector<const ExprPtr*> conjuncts,
                        ConjunctsOf(correlation, spine));
  for (const ExprPtr* defined : conjuncts) {
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
    const bool not_distinct =
        b != nullptr && compare->predicate.kind == Predicate::Kind::kIsNotDistinct &&
        correlation.outer.contains(a->id) != correlation.outer.contains(b->id);
    // One type on both ends, which KeyType answers for: it is the same question, and it excludes
    // DOUBLE, whose comparison is not exact.
    const bool one_type =
        (equality || not_distinct) && compare->operands[0]->type == compare->operands[1]->type &&
        KeyType(compare->operands[0]->type, compare->operands[1]->type).has_value();
    if (!one_type) {
      out.kept.push_back(&expr);
      continue;
    }
    const auto describe = [&out](ColumnId id, const std::optional<BoundColumn>& from,
                                 LogicalType type) {
      BoundColumn column = from.value_or(BoundColumn{});
      column.id = id;
      column.type = type;
      out.described.insert_or_assign(id, std::move(column));
    };
    describe(a->id, compare->predicate.column, compare->operands[0]->type);
    describe(b->id, compare->predicate.other, compare->operands[1]->type);
    if (not_distinct) {
      const bool a_outer = correlation.outer.contains(a->id);
      out.links.push_back(NullEqualLink{
          .expr = &expr, .outer = a_outer ? a->id : b->id, .inner = a_outer ? b->id : a->id});
      continue;
    }
    out.classes.Merge(a->id, b->id);
    out.equalities.push_back(Equality{.expr = &expr, .a = a->id, .b = b->id});
  }
  return out;
}

// ---- the terminal step: substitution (decision 18) ----

// The representatives of the outer columns: per outer id, the inner column it is replaced by.
struct Representatives {
  std::map<ColumnId, ColumnId> of;  // outer id -> inner column; the map's order is the keys' order
  std::set<ColumnId> null_equal;    // the outer ids whose key is NULL-equal
  std::set<const NullEqualLink*> used_links;
};

// Every outer column the subquery reads needs a representative among the columns below the
// correlated filter, and gets its OWN key: one key per class would drop a second outer column's
// constraint and keep rows SQL rejects. Phase 8 has no other terminal step: an outer column without
// one is refused (the domain D is SQ9's).
arrow::Result<Representatives> RepresentativesOf(const Correlation& correlation, const Spine& spine,
                                                 Equivalences& equivalences) {
  const std::vector<ColumnId> inner_ids = OutputIds(*spine.below);
  const std::set<ColumnId> inner(inner_ids.begin(), inner_ids.end());
  Representatives out;
  for (const ColumnId id : FreeVariables(*correlation.join.right)) {
    ColumnId found = kNoColumnId;
    for (const ColumnId candidate : inner_ids) {
      if (equivalences.classes.Same(id, candidate)) {
        found = candidate;
        break;
      }
    }
    if (found == kNoColumnId) {
      const NullEqualLink* used = nullptr;
      for (const NullEqualLink& link : equivalences.links) {
        if (link.outer == id && inner.contains(link.inner)) {
          used = &link;
          break;
        }
      }
      if (used != nullptr) {
        found = used->inner;
        out.null_equal.insert(id);
        out.used_links.insert(used);
      }
    }
    if (found == kNoColumnId) {
      return correlation.Refuse("with an outer column that has no inner equivalent");
    }
    out.of.emplace(id, found);
  }
  if (out.of.empty()) {
    return correlation.Refuse("that reads no column of its enclosing block");
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
  const auto substituted = [&out](ColumnId id) {
    const auto it = out.of.find(id);
    return it == out.of.end() ? id : it->second;
  };
  for (const Equality& equality : equivalences.equalities) {
    if (substituted(equality.a) != substituted(equality.b) || out.null_equal.contains(equality.a) ||
        out.null_equal.contains(equality.b)) {
      equivalences.kept.push_back(equality.expr);
    }
  }
  // A link that gave its outer column the representative says no more than the NULL-equal key that
  // replaces it; any other is a condition between inner columns once substituted, and is kept.
  for (const NullEqualLink& link : equivalences.links) {
    if (!out.used_links.contains(&link)) {
      equivalences.kept.push_back(link.expr);
    }
  }
  return out;
}

// Value mode's rewrite (ADR 0023's Path 2; the formalization's Lemmas 4.14 and 4.2): each outer
// column is replaced by its representative, so the correlated conjuncts read inner columns only and
// stay below the aggregation; an equality that became c = c is dropped. The static aggregate groups
// by the representatives, the maps over it are rebuilt above the group, and the one-row dependent
// join becomes an INNER join with one key per outer column the subquery reads (decision 20's proof
// decides LEFT from SQ6 on).
//
//   Filter o.v <= s                      Filter o.v <= s
//     DependentJoin one-row                Join inner  keys o.k = <key of t.k>
//       Scan o                               Scan o
//       Aggregate m := max(t.x)              GroupAggregate keys t.k  m := max(t.x)
//         Filter t.k = o.k                     Scan t
//           Scan t
arrow::Result<LogicalNodePtr> ValueJoin(const Correlation& correlation, const Spine& spine) {
  const DependentJoinNode& join = correlation.join;
  ColumnId& last_id = correlation.last_id;
  std::map<ColumnId, EmptyValue> values;
  ARROW_RETURN_NOT_OK(CheckStatic(correlation, spine, values));
  ARROW_ASSIGN_OR_RAISE(Equivalences equivalences, EquivalencesOf(correlation, spine));
  ARROW_ASSIGN_OR_RAISE(const Representatives representatives,
                        RepresentativesOf(correlation, spine, equivalences));
  // Decision 20's proof, over the values the subquery appends. It decides nothing yet: the join
  // back is INNER, which every shape this rule answers proves (the binder puts the comparison that
  // reads the value directly above the join); SQ6 joins back LEFT where it fails.
  std::map<ColumnId, EmptyValue> appended;
  for (const ColumnId id : OutputIds(*join.right)) {
    appended.insert_or_assign(id, values.at(id));
  }
  const JoinBack join_back = ProveJoinBack(correlation.above, appended);
  if (correlation.report != nullptr) {
    correlation.report->join_backs.push_back({.span = join.span, .join_back = join_back});
  }
  const std::map<ColumnId, ColumnId>& equivalent = representatives.of;
  const std::map<ColumnId, BoundColumn>& described = equivalences.described;

  // The static aggregate's keys, allocated first: the representatives, distinct, in the map's
  // order, each output as a new column that the join's key then reads.
  const AggregateNode* aggregate = nullptr;
  for (const Step& step : spine.steps) {
    if (step.role == Role::kStatic) {
      aggregate = &std::get<AggregateNode>(*step.node);
    }
  }
  ANTB1_CHECK(aggregate != nullptr);
  std::vector<ColumnId> group;
  for (const auto& [id, equiv] : equivalent) {
    if (std::ranges::find(group, equiv) == group.end()) {
      group.push_back(equiv);
    }
  }
  GroupAggregateNode grouped{.input = nullptr,
                             .keys = {},
                             .key_ids = {},
                             .aggregates = aggregate->aggregates,
                             .span = aggregate->span};
  std::map<ColumnId, ColumnId> key_of;  // the inner column -> the GroupAggregate's output for it
  for (const ColumnId id : group) {
    const auto column = described.find(id);
    if (column == described.end()) {
      return correlation.Refuse("whose correlated key the pass cannot name");
    }
    grouped.keys.push_back(column->second);
    grouped.key_ids.push_back(ColumnId{std::to_underlying(last_id) + 1});
    last_id = grouped.key_ids.back();
    key_of.emplace(id, grouped.key_ids.back());
  }

  // The substitution: every outer id an expression reads becomes its representative's. MapColumns
  // keeps each expression's name, so the outer names stay printed.
  const auto substitute = [&equivalent](const ExprPtr& expr) {
    return MapColumns(expr, [&equivalent](ColumnExpr column) {
      const auto it = equivalent.find(column.id);
      if (it != equivalent.end()) {
        column.id = it->second;
      }
      return column;
    });
  };
  // Bottom-up: the kept conjuncts over the terminal, then the steps.
  LogicalNodePtr right = spine.below;
  if (!equivalences.kept.empty()) {
    std::vector<ExprPtr> exprs;
    std::vector<ColumnId> ids;
    std::vector<Predicate> predicates;
    for (const Expr* expr : equivalences.kept) {
      exprs.push_back(substitute(std::make_shared<const Expr>(*expr)));
      ids.push_back(ColumnId{std::to_underlying(last_id) + 1});
      last_id = ids.back();
      predicates.push_back(Predicate{.kind = Predicate::Kind::kIsTrue,
                                     .column = BoundColumn{.id = ids.back(),
                                                           .name = exprs.back()->name,
                                                           .type = LogicalType::kBoolean},
                                     .span = spine.filter->span});
    }
    right = Make(ComputeNode{
        .input = std::move(right), .exprs = exprs, .ids = ids, .span = spine.computed->span});
    right = Make(FilterNode{
        .input = std::move(right), .predicates = predicates, .span = spine.filter->span});
  }
  // The keys as the join will name them, taken before `grouped` is moved into its node.
  const std::vector<BoundColumn> group_keys = grouped.keys;
  for (const Step& step : std::ranges::reverse_view(spine.steps)) {
    switch (step.role) {
      case Role::kArguments: {
        ComputeNode args = std::get<ComputeNode>(*step.node);
        for (ExprPtr& expr : args.exprs) {
          expr = substitute(expr);
        }
        args.input = std::move(right);
        right = Make(std::move(args));
        break;
      }
      case Role::kStatic: {
        GroupAggregateNode node = grouped;  // the one static step
        node.input = RestrictToTheOuterKeys(std::move(right), join, equivalent,
                                            representatives.null_equal, described, last_id);
        right = Make(std::move(node));
        break;
      }
      case Role::kMap: {
        // A Compute passes its input's columns through, but a Project outputs only its own, so the
        // group's key columns are added to it: the join above reads them, and without them its key
        // would name a column its input does not output, which ResolvePositions turns into an
        // abort.
        const auto* project = std::get_if<ProjectNode>(step.node.get());
        if (project == nullptr) {
          right = WithInputs(*step.node, {std::move(right)});
          break;
        }
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
        break;
      }
      case Role::kDropped:
        ANTB1_CHECK(false);  // lift mode's only
        break;
    }
  }

  // One key per outer column, the outer column equal to the group's output for its representative.
  std::vector<JoinKey> keys;
  for (const auto& [id, equiv] : equivalent) {
    const auto outer_column = described.find(id);
    if (outer_column == described.end()) {
      return correlation.Refuse("whose correlated key the pass cannot name");
    }
    BoundColumn key = outer_column->second;
    BoundColumn group_key =
        group_keys[Narrow<std::size_t>(std::ranges::find(group, equiv) - group.begin())];
    group_key.id = key_of.at(equiv);
    if (key.type != group_key.type) {
      return correlation.Refuse("whose correlated key has two types");
    }
    keys.push_back(JoinKey{.left = std::move(key),
                           .right = std::move(group_key),
                           .null_equal = representatives.null_equal.contains(id)});
  }
  // The join is an inner join, so either side may build: the aggregate it puts on the right is
  // often far larger than the outer rows that probe it (ADR 0026). Its estimate is its left input's
  // rows: the keys above are exactly the aggregate's group keys, which a group by makes unique, so
  // each outer row matches at most one group and the join cannot multiply its left input. That is
  // an upper bound rather than a count of the groups, which nothing estimates -- but leaving it
  // unknown left every join above this rewrite reading its own left input as unknown, which is what
  // kept a lopsided join building on the wrong side (ADR 0026).
  const std::optional<int64_t> left_rows = EstimateRows(*join.left);
  const std::optional<int64_t> right_rows = EstimateRows(*right);
  return Make(
      JoinNode{.kind = JoinKind::kInner,
               .left = join.left,
               .right = std::move(right),
               .keys = std::move(keys),
               .residual = {},
               .build = ChooseBuildSide(left_rows, right_rows, correlation.build_side_ratio),
               .estimate = left_rows,
               .span = join.span});
}

// The dependent join's rule: its mode, the descent, the mode's rewrite, then Lemma 3.1's check.
arrow::Result<LogicalNodePtr> UnnestJoin(const DependentJoinNode& join, ColumnId& last_id,
                                         int64_t build_side_ratio,
                                         std::span<const LogicalNode* const> above,
                                         UnnestReport* report) {
  const std::optional<Mode> mode = ModeOf(join.kind);
  if (!mode.has_value()) {
    return UnsupportedError("a correlated subquery is not supported", join.span);
  }
  const std::vector<ColumnId> outer_ids = OutputIds(*join.left);
  const Correlation correlation{.join = join,
                                .mode = *mode,
                                .outer = std::set<ColumnId>(outer_ids.begin(), outer_ids.end()),
                                .last_id = last_id,
                                .build_side_ratio = build_side_ratio,
                                .above = above,
                                .report = report};
  const std::optional<Spine> spine = Descend(*mode, join.right);
  if (!spine.has_value()) {
    return correlation.RefuseShape();
  }
  ARROW_ASSIGN_OR_RAISE(LogicalNodePtr out, *mode == Mode::kLift ? LiftJoin(correlation, *spine)
                                                                 : ValueJoin(correlation, *spine));
  // Lemma 3.1: the rewrite is legal only when the new join's right input has no free variables
  // left. A bug in a rule is the only way this fails, and it must not reach ResolvePositions, which
  // would abort the process rather than fail the query.
  if (!FreeVariables(*std::get<JoinNode>(*out).right).empty()) {
    return arrow::Status::NotImplemented(
        "the unnesting pass left a free variable in a rewritten join's right input");
  }
  return out;
}

// The walk, outermost first as the top-down algorithm goes: a dependent join is rewritten before
// anything inside it, and the rewrite's own inputs are then walked, so a dependent join nested in
// a correlated block is rewritten too (or refused at its own span, which is more useful than a
// blanket refusal at the outer one). An unchanged subtree keeps its node. The walk keeps the nodes
// above the one it is at, root first, for the consumer proof: a rewritten join stands where its
// dependent join stood, under the same nodes.
class Walker {
 public:
  Walker(ColumnId& last_id, int64_t build_side_ratio, UnnestReport* report)
      : last_id_(last_id), build_side_ratio_(build_side_ratio), report_(report) {}

  arrow::Result<LogicalNodePtr> Walk(const LogicalNodePtr& node) {
    if (node == nullptr) {
      return node;
    }
    if (const auto* dependent = std::get_if<DependentJoinNode>(node.get())) {
      ARROW_ASSIGN_OR_RAISE(const LogicalNodePtr rewritten,
                            UnnestJoin(*dependent, last_id_, build_side_ratio_, above_, report_));
      return Walk(rewritten);  // its inputs may hold more
    }
    std::vector<LogicalNodePtr> inputs = InputsOf(*node);
    bool changed = false;
    above_.push_back(node.get());
    for (LogicalNodePtr& input : inputs) {
      if (input == nullptr) {
        continue;
      }
      arrow::Result<LogicalNodePtr> rewritten = Walk(input);
      if (!rewritten.ok()) {
        above_.pop_back();
        return rewritten.status();
      }
      if (*rewritten != input) {
        input = *std::move(rewritten);
        changed = true;
      }
    }
    above_.pop_back();
    return changed ? WithInputs(*node, std::move(inputs)) : node;
  }

 private:
  ColumnId& last_id_;
  int64_t build_side_ratio_;
  UnnestReport* report_;
  std::vector<const LogicalNode*> above_;
};
}  // namespace

EmptyValue EmptyValueOf(const AggregateCall& call) {
  switch (call.kind) {
    case AggKind::kCountStar:
    case AggKind::kCount:
    case AggKind::kCountDistinct:
    case AggKind::kSumCounts:
      return Constant{.type = call.type, .value = Int128{0}};
    case AggKind::kSum:
    case AggKind::kAvg:
    case AggKind::kMin:
    case AggKind::kMax:
    case AggKind::kStddevSamp:
    case AggKind::kStddevPop:
    case AggKind::kVarSamp:
    case AggKind::kVarPop:
    case AggKind::kSingleValue:
      return Constant{.type = call.type, .value = std::monostate{}};
  }
  return std::nullopt;
}

JoinBack ProveJoinBack(std::span<const LogicalNode* const> above,
                       const std::map<ColumnId, EmptyValue>& values) {
  std::map<ColumnId, EmptyValue> tracked = values;
  const auto value_of = [&tracked](const std::optional<BoundColumn>& column) -> EmptyValue {
    if (!column.has_value()) {
      return std::nullopt;
    }
    const auto it = tracked.find(column->id);
    return it == tracked.end() ? std::nullopt : it->second;
  };
  const auto reads = [&tracked](const std::optional<BoundColumn>& column) {
    return column.has_value() && tracked.contains(column->id);
  };
  for (const LogicalNode* node : std::ranges::reverse_view(above)) {
    if (const auto* compute = std::get_if<ComputeNode>(node)) {
      for (std::size_t i = 0; i < compute->exprs.size() && i < compute->ids.size(); ++i) {
        std::vector<ColumnId> ids;
        CollectColumnIds(*compute->exprs[i], ids);
        if (!std::ranges::any_of(ids, [&tracked](ColumnId id) { return tracked.contains(id); })) {
          continue;
        }
        const bool null =
            IsStrict(*compute->exprs[i]) && std::ranges::any_of(ids, [&](ColumnId id) {
              const auto it = tracked.find(id);
              return it != tracked.end() && IsNull(it->second);
            });
        tracked.insert_or_assign(
            compute->ids[i],
            null ? EmptyValue(Constant{.type = compute->exprs[i]->type, .value = std::monostate{}})
                 : EmptyValue());
      }
      continue;
    }
    const auto* filter = std::get_if<FilterNode>(node);
    if (filter == nullptr) {
      return JoinBack::kLeft;  // a node on the way that is not a Compute
    }
    for (const Predicate& predicate : filter->predicates) {
      if ((reads(predicate.column) || reads(predicate.other)) &&
          TruthOver(predicate, value_of(predicate.column), value_of(predicate.other)) ==
              BranchTruth::kNever) {
        return JoinBack::kInner;  // every row whose value is empty fails this conjunct
      }
    }
    return JoinBack::kLeft;  // the first Filter on the way rejects nothing for sure
  }
  return JoinBack::kLeft;  // nothing reads the value: an INNER join would drop outer rows
}

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

arrow::Result<LogicalNodePtr> Unnest(const LogicalNodePtr& root, int64_t build_side_ratio,
                                     UnnestReport* report) {
  if (FirstDependentJoin(root) == nullptr) {
    return root;
  }
  ColumnId last_id = MaxColumnId(*root);
  return Walker(last_id, build_side_ratio, report).Walk(root);
}

}  // namespace antb1::plan
