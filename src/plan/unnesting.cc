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

#include "arithmetic_rewrite.h"
#include "column_classes.h"
#include "compute_safety.h"
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

std::optional<KeyParts> ComparedColumns(const Expr& expr, const std::set<ColumnId>& outer_ids) {
  const auto* predicate = std::get_if<PredicateExpr>(&expr.node);
  if (predicate == nullptr || predicate->operands.size() != 2) {
    return std::nullopt;
  }
  const bool null_equal = predicate->predicate.kind == Predicate::Kind::kIsNotDistinct;
  if (!null_equal && predicate->predicate.kind != Predicate::Kind::kIsDistinct &&
      predicate->predicate.kind != Predicate::Kind::kCompareColumns) {
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

bool IsEquality(const Expr& expr) {
  const auto* p = std::get_if<PredicateExpr>(&expr.node);
  return p != nullptr && (p->predicate.kind == Predicate::Kind::kIsNotDistinct ||
                          (p->predicate.kind == Predicate::Kind::kCompareColumns &&
                           p->predicate.op == CompareOp::kEq));
}

std::optional<KeyParts> AsKey(const Expr& expr, const std::set<ColumnId>& outer_ids) {
  return IsEquality(expr) ? ComparedColumns(expr, outer_ids) : std::nullopt;
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

// A join or set operation blocks D5. A computed key also has no scan column to filter,
// but for a small input the extra hash build can cost more than grouping the whole input.
// At 3,000 rows with 101 groups it cost 20%; at 4,096 distinct rows it already saved 15%.
constexpr int64_t kMinRestrictionRows = 4096;

bool NeedsKeyRestriction(const LogicalNodePtr& node, ColumnId key) {
  if (node == nullptr) {
    return false;
  }
  if (std::holds_alternative<JoinNode>(*node) || std::holds_alternative<UnionAllNode>(*node) ||
      std::holds_alternative<SetOperationNode>(*node)) {
    return true;
  }
  if (const auto* compute = std::get_if<ComputeNode>(node.get());
      compute != nullptr && std::ranges::contains(compute->ids, key) &&
      EstimateRows(*compute->input).value_or(0) >= kMinRestrictionRows) {
    return true;
  }
  if (const auto* project = std::get_if<ProjectNode>(node.get())) {
    const auto at = std::ranges::find(project->ids, key);
    if (at != project->ids.end()) {
      key = project->columns.at(Narrow<std::size_t>(at - project->ids.begin())).id;
    }
  }
  return std::ranges::any_of(InputsOf(*node), [key](const LogicalNodePtr& input) {
    return NeedsKeyRestriction(input, key);
  });
}

// A column and its exact widening casts are safe to evaluate on extra binding or probe rows.
std::optional<ColumnId> WidenedColumn(const Expr& expr) {
  if (const auto* column = std::get_if<ColumnExpr>(&expr.node)) {
    return column->id;
  }
  if (const auto* cast = std::get_if<CastExpr>(&expr.node);
      cast != nullptr && IsExactWidening(cast->operand->type, expr.type)) {
    return WidenedColumn(*cast->operand);
  }
  return std::nullopt;
}

// LIMIT and windows can choose different tied rows on a second evaluation; neither may be cloned
// into an independently evaluated decorrelation domain (ADRs 0030 and 0043).
bool UnsafeToClone(const LogicalNodePtr& node) {
  if (node == nullptr) {
    return false;
  }
  if (std::holds_alternative<LimitNode>(*node) || std::holds_alternative<WindowNode>(*node)) {
    return true;
  }
  return std::ranges::any_of(InputsOf(*node),
                             [](const LogicalNodePtr& input) { return UnsafeToClone(input); });
}

// The part of a dependent join's left input that the semi-join reduction clones for its domain, or
// nullptr where it declines: the first node down from `outer` that holds no dependent join and no
// free variable and outputs every column of `keys`, through the nodes that keep a binding's key
// values as they are and add no row -- filters, computes (replaying safe key expressions), the left
// input of a dependent join (stacked correlated subqueries, which the pass rewrites after this
// one), and the left input of a join that outputs no row of it padded. Every binding the outer side
// makes is then a row of the base, so its keys are a superset of the bound ones, which is all a
// domain needs to be: the restriction removes only groups no outer row reads.
LogicalNodePtr DomainBase(const LogicalNodePtr& outer, const std::set<ColumnId>& keys) {
  LogicalNodePtr node = outer;
  while (node != nullptr &&
         (FirstDependentJoin(node) != nullptr || !FreeVariables(*node).empty())) {
    if (const auto* filter = std::get_if<FilterNode>(node.get())) {
      node = filter->input;
    } else if (const auto* compute = std::get_if<ComputeNode>(node.get())) {
      ComputeNode keys_to_replay{.span = compute->span};
      std::set<ColumnId> sources;
      for (const ColumnId key : keys) {
        const ExprPtr* expr = DefinedBy(*compute, key);
        if (expr == nullptr) {
          sources.insert(key);
          continue;
        }
        std::vector<ColumnId> reads;
        CollectColumnIds(**expr, reads);
        if (reads.empty() || std::holds_alternative<ColumnExpr>((*expr)->node)) {
          return nullptr;
        }
        keys_to_replay.ids.push_back(key);
        keys_to_replay.exprs.push_back(*expr);
        sources.insert(reads.begin(), reads.end());
      }
      if (!keys_to_replay.ids.empty()) {
        keys_to_replay.input = DomainBase(compute->input, sources);
        // The base can hold more rows than the outer block. Recheck each expression there
        // before replaying it, including casts of other computed keys.
        if (keys_to_replay.input == nullptr ||
            !std::ranges::all_of(keys_to_replay.exprs, [&](const ExprPtr& expr) {
              return CanPrecomputeScalarKey(*expr, keys_to_replay.input);
            })) {
          return nullptr;
        }
        return Make(std::move(keys_to_replay));
      }
      node = compute->input;
    } else if (const auto* dependent = std::get_if<DependentJoinNode>(node.get())) {
      node = dependent->left;
    } else if (const auto* join = std::get_if<JoinNode>(node.get());
               join != nullptr && !Traits(join->kind).pads_left) {
      node = join->left;
    } else {
      return nullptr;
    }
  }
  if (node == nullptr || UnsafeToClone(node)) {
    return nullptr;
  }
  const std::vector<ColumnId> outputs = OutputIds(*node);
  if (!std::ranges::all_of(keys, [&outputs](ColumnId id) {
        return std::ranges::find(outputs, id) != outputs.end();
      })) {
    return nullptr;
  }
  return node;
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
// neither an argument nor a GROUP BY to enforce it; the restriction gate requires the outer
// domain to be smaller, so ChooseBuildSide keeps that domain on the build side; and it outputs
// only its preserved input's columns, so the
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
                                      JoinBack join_back, ColumnId& last_id,
                                      int64_t build_side_ratio) {
  // **One correlated column over a bare scan is ADR 0029's D5 to restrict, not this -- under an
  // INNER join back.** D5 publishes the join's key set to the aggregation's own scan, which costs a
  // filter rather than a build and a probe, and it takes exactly one key. Where it can apply, this
  // rewrite can only repeat it: measured at SF 100 on the query whose correlation is a single key
  // over a scan, restricting it as well cost **1.420x** -- the aggregate's rows and groups did not
  // move at all, because D5 had already removed the same rows, while the clone re-read the
  // 600-million-row join its outer side is.
  //
  // For a single key, follow the filter through the same column remaps as the physical planner.
  // It stops at a join, a union, or a Compute that defines the key (including SQ10's widening).
  // In those cases the costed semi join can still discard unbound keys before grouping.
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
  //
  // **A LEFT join back (ADR 0041's SQ6) gets no D5 at all**, so this rewrite is the only reduction
  // there: D5 offers a join's keys to the scan of the aggregation under its *probe*, and only where
  // the join drops the probe rows that match nothing (ProbeDropsUnmatchedRows), while a LEFT join
  // keeps every outer row and builds the aggregation. Without the restriction it would group and
  // build every inner key.
  if (join_back == JoinBack::kInner && equivalent.size() == 1 &&
      !NeedsKeyRestriction(input, equivalent.begin()->second)) {
    return input;
  }
  // **An outer side holding a dependent join is not cloned whole, and that is what bounds the
  // copying.** The pass is top-down, so a dependent join still in this one's left input is one this
  // rewrite has not reached: cloning it would duplicate a rewrite that clones in turn, and k
  // stacked correlated subqueries would cost 2^k nodes. Measured before this condition, on a query
  // of k conjuncts over one correlation: 45 plan lines at k=1, 1,533 at k=8 and 24,573 at k=12,
  // where planning alone took 1.3 s. So the clone is of the base below the stack (DomainBase): each
  // join of a chain clones that base once, and a plan's size stays linear in the rewrites, which is
  // what ADR 0022's relation limit needs in order to go on bounding every walk of a plan. Where no
  // such base outputs every key, the restriction declines, as it did for any stack before ADR
  // 0041's SQ6
  // -- where a LEFT join back, which D5 does not reach, would otherwise group every inner key.
  //
  // It also keeps the Lemma 3.1 check below unreachable by construction: a free variable can only
  // enter `join.left` through a dependent join, which the base holds none of, and the clone lands
  // in the new join's right input, where a free variable would fail the query with an internal
  // error rather than decline.
  std::set<ColumnId> wanted;  // the outer columns the restriction keys on
  for (const auto& [outer_id, inner_id] : equivalent) {
    wanted.insert(outer_id);
  }
  const LogicalNodePtr base = DomainBase(join.left, wanted);
  if (base == nullptr) {
    return input;
  }
  if (!RestrictionIsWorth(EstimateRows(*base), EstimateRows(*input))) {
    return input;
  }
  const std::vector<ColumnId> outer_ids = OutputIds(*base);
  const LogicalNodePtr domain = CloneWithFreshIds(base, last_id);
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
  const BuildSide build = ChooseBuildSide(rows, EstimateRows(*domain), build_side_ratio);
  return Make(JoinNode{.kind = JoinKind::kSemi,
                       .left = std::move(input),
                       .right = domain,
                       .keys = std::move(keys),
                       .residual = {},
                       .build = build,
                       .estimate = rows,  // a semi join keeps a subset of its probe
                       .span = join.span});
}

// ---- the rule table (ADR 0041's decisions 16 to 18) ----

// How a dependent join's kind is unnested, read from its traits, never from its name (decision 16).
enum class Mode : std::uint8_t {
  // A join that keeps, drops or marks left rows and reads none of its right input's columns, and
  // that can evaluate the correlated conjuncts over candidate pairs (semi, anti and mark joins):
  // they move into it, as ADR 0023's Path 1 has it.
  kLift,
  // A join that appends its right input's one row (one-row): a static aggregate grouped by the
  // representatives of the outer columns, by substitution, as ADR 0023's Path 2 has it.
  kValue,
  // Preserve maps and partitions for EXISTS, grouped membership and scalar selects.
  kPartitioned,
};

// std::nullopt where the pass answers no correlated subquery of the kind: a null-aware anti join
// and a null-aware mark take no residual, so their correlation cannot move into them (SQ12), and
// the binder makes no dependent inner, LEFT or FULL join.
std::optional<Mode> ModeOf(JoinKind kind) {
  const JoinKindTraits traits = Traits(kind);
  if (traits.single_match) {
    return Mode::kPartitioned;  // carry binding keys through maps, grouping and HAVING
  }
  if (traits.one_row) {
    return Mode::kValue;
  }
  if ((traits.filters_left || traits.appends_mark) && traits.residuals) {
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
  std::span<const Ancestor> above;  // the plan's root down to the join's parent
  UnnestReport* report = nullptr;

  // The refusal of a shape, in the wording each mode has always had, at the subquery's span: a
  // mark's is an EXISTS's, whose NOT, where it has one, stands above the mark.
  [[nodiscard]] arrow::Status Refuse(std::string_view what) const {
    if (join.membership.has_value()) {
      return UnsupportedError(std::format("a correlated {} {} is not supported",
                                          join.kind == JoinKind::kAnti ? "NOT IN" : "IN", what),
                              join.span);
    }
    if (mode == Mode::kValue || Traits(join.kind).single_match) {
      return UnsupportedError(std::format("a correlated scalar subquery {} is not supported", what),
                              join.span);
    }
    const JoinKindTraits traits = Traits(join.kind);
    return UnsupportedError(
        std::format("a correlated {} {} is not supported",
                    traits.drops_unmatched_left || traits.appends_mark ? "EXISTS" : "NOT EXISTS",
                    what),
        join.span);
  }
  // A node on the way to the correlated filter that this mode has no rule for.
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
  kHaving,     // value: a HAVING over the static aggregate, a CASE above it (decision 17)
  kJoin,       // a join with an independent right input; 1:1 above a static aggregate
  kGroup,      // EXISTS: append the representatives to the grouping keys
  kSets,       // EXISTS: append them to every nonempty grouping set
  kLimit,      // EXISTS: count surviving rows per binding for OFFSET
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
  const std::set<ColumnId>& outer;
  bool exists_value = false;
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
    const bool admitted = mode != Mode::kValue ? stage == Stage::kAbove : stage != Stage::kAbove;
    const auto* computed = std::get_if<ComputeNode>(filter.input.get());
    if (!admitted || computed == nullptr) {
      return Next::kDeclined;
    }
    spine.filter = &filter;
    spine.computed = computed;
    spine.below = computed->input;
    return Next::kFound;
  }

  // A Filter above the static aggregate in value mode is its HAVING, which becomes a CASE over the
  // maps above it, never a filter (decision 17's Filter row): a binding whose group it rejects
  // still reads the subquery's value, NULL.
  Next operator()(const FilterNode& filter) {
    if (mode != Mode::kValue) {
      const auto* computed = std::get_if<ComputeNode>(filter.input.get());
      const bool correlated =
          computed != nullptr &&
          std::ranges::any_of(filter.predicates, [&](const Predicate& predicate) {
            const ExprPtr* expr =
                predicate.column.has_value() ? DefinedBy(*computed, predicate.column->id) : nullptr;
            return expr != nullptr && SidesOf(**expr, outer).outer;
          });
      if (correlated) {
        return Filter(filter);
      }
      return mode == Mode::kPartitioned ? Record(Role::kHaving) : Next::kDeclined;
    }
    if (mode == Mode::kValue && stage == Stage::kAbove) {
      return Record(Role::kHaving);
    }
    return Filter(filter);
  }
  Next operator()(const ProjectNode& /*project*/) {
    if (mode == Mode::kPartitioned) {
      return Record(Role::kMap);
    }
    if (stage != Stage::kAbove) {
      return Next::kDeclined;
    }
    return Record(mode == Mode::kLift ? Role::kDropped : Role::kMap);
  }
  Next operator()(const WindowNode& /*window*/) { return Next::kDeclined; }
  Next operator()(const ComputeNode& /*compute*/) {
    if (mode == Mode::kPartitioned) {
      return Record(Role::kMap);
    }
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
    return mode != Mode::kValue && stage == Stage::kAbove ? Record(Role::kDropped)
                                                          : Next::kDeclined;
  }
  // The static aggregate of a scalar subquery (decision 17's "Aggregate (static)" row); an EXISTS
  // over one is SQ8's.
  Next operator()(const AggregateNode& /*aggregate*/) {
    if (mode == Mode::kPartitioned) {
      return Record(Role::kStatic);
    }
    if (mode != Mode::kValue || stage != Stage::kAbove) {
      return Next::kDeclined;
    }
    stage = Stage::kUnderStatic;
    return Record(Role::kStatic);
  }
  // SQ8 and SQ14 (LIMIT), SQ8 and SQ4b (GROUP BY), SQ16 (grouping sets), SQ15 (set operations),
  // SQ8 and SQ10 (joins on the way, decision 17's join rule), and the leaves and a nested dependent
  // join, which stand below a correlated filter, never above one.
  Next operator()(const LimitNode& /*limit*/) {
    return mode == Mode::kPartitioned ? Record(Role::kLimit) : Next::kDeclined;
  }
  Next operator()(const GroupAggregateNode& /*group*/) {
    return mode == Mode::kPartitioned ? Record(Role::kGroup) : Next::kDeclined;
  }
  Next operator()(const GroupingSetsNode& sets) {
    return mode == Mode::kPartitioned &&
                   std::ranges::none_of(sets.sets, [](const auto& set) { return set.empty(); })
               ? Record(Role::kSets)
               : Next::kDeclined;
  }
  Next operator()(const UnionAllNode& /*union_all*/) { return Next::kDeclined; }
  Next operator()(const SetOperationNode& /*set_operation*/) { return Next::kDeclined; }
  // Decision 17's Join rule, the one piece SQ6 needs: an uncorrelated value's one-row join over the
  // static aggregate, from the slot after the aggregation, moves up with the select list, so that a
  // binding without rows reads the empty value combined with the value. It appends to each row of
  // its left input and pads none, and its right input reads no outer column, so the descent goes on
  // into its left. SQ8b also carries EXISTS through left-preserving joins with an independent
  // right input, and moves HAVING marks above a static aggregate's empty-value join back.
  Next operator()(const JoinNode& join) {
    const JoinKindTraits traits = Traits(join.kind);
    const bool allowed = mode == Mode::kPartitioned
                             ? !traits.pads_left
                             : traits.one_row || (exists_value && traits.appends_mark);
    if (mode == Mode::kLift || stage != Stage::kAbove || !allowed ||
        !FreeVariables(*join.right).empty()) {
      return Next::kDeclined;
    }
    return Record(Role::kJoin);
  }
  Next operator()(const DependentJoinNode& /*join*/) { return Next::kDeclined; }
  Next operator()(const ScanNode& /*scan*/) { return Next::kDeclined; }
  Next operator()(const RowCountNode& /*row_count*/) { return Next::kDeclined; }
};

// The spine of `root`, a dependent join's right input, or std::nullopt where a rule declines, the
// node it declined at in `declined`.
std::optional<Spine> Descend(Mode mode, const LogicalNodePtr& root, LogicalNodePtr* declined,
                             const std::set<ColumnId>& outer, bool exists_value = false) {
  Descent descent{.mode = mode,
                  .outer = outer,
                  .exists_value = exists_value,
                  .stage = Descent::Stage::kAbove,
                  .spine = {},
                  .node = root};
  while (descent.node != nullptr) {
    switch (std::visit(descent, *descent.node)) {
      case Descent::Next::kFound:
        return std::move(descent.spine);
      case Descent::Next::kDeclined:
        *declined = descent.node;
        return std::nullopt;
      case Descent::Next::kInput:
        break;
    }
    if (const auto* join = std::get_if<JoinNode>(descent.node.get())) {
      descent.node = join->left;  // the admitted join's correlated input
      continue;
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

// Whether the rule answers a static aggregate's call: each has its empty value (decision 19), which
// the LEFT join back gives an outer row with no group (decision 21) -- COUNT's 0 through a
// COALESCE.
bool AnsweredCall(AggKind kind) {
  switch (kind) {
    case AggKind::kSum:
    case AggKind::kAvg:
    case AggKind::kMin:
    case AggKind::kMax:
    case AggKind::kCountStar:
    case AggKind::kCount:
    case AggKind::kCountDistinct:
    // A sum of counts is 0 over no rows, by design (ADR 0032's G2c). Unreachable today, since the
    // unnesting runs before the exchange that makes one, and correct if that ever changes.
    case AggKind::kSumCounts:
    case AggKind::kStddevSamp:
    case AggKind::kStddevPop:
    case AggKind::kVarSamp:
    case AggKind::kVarPop:
      return true;
    // The binder wraps only an uncorrelated subquery in it, so no dependent join holds one; its
    // check of the row count belongs to each binding, which grouping by the correlated keys would
    // lose (ADR 0041's decision 22), and the executor has no grouped state for it.
    case AggKind::kSingleValue:
      return false;
  }
  return false;
}

bool IsNull(const EmptyValue& value) {
  return value.has_value() && std::holds_alternative<std::monostate>(value->value);
}

// The ids the maps above a block's topmost HAVING define -- its select list's own columns -- or
// std::nullopt where the block has no HAVING. A value the dependent join appends that is not among
// them is defined below the HAVING, which decides it all the same.
std::optional<std::set<ColumnId>> IdsAboveHaving(const Spine& spine) {
  if (std::ranges::none_of(spine.steps,
                           [](const Step& step) { return step.role == Role::kHaving; })) {
    return std::nullopt;
  }
  std::set<ColumnId> above;
  for (const Step& step : spine.steps) {
    if (step.role == Role::kHaving) {
      break;
    }
    if (const auto* compute = std::get_if<ComputeNode>(step.node.get())) {
      above.insert(compute->ids.begin(), compute->ids.end());
    } else if (const auto* project = std::get_if<ProjectNode>(step.node.get())) {
      for (std::size_t i = 0; i < project->ids.size() && i < project->columns.size(); ++i) {
        const bool constant = i < project->constants.size() && project->constants[i].has_value();
        if (constant || project->columns[i].id != project->ids[i]) {
          above.insert(project->ids[i]);
        }
      }
    }
  }
  return above;
}

// The empty value of each column the static aggregate and the maps above it define (decision 19):
// each call's, then, bottom-up through the maps, NULL for a strict combination (IsStrict) that
// reads at least one NULL, a Project's column its source's, and not known otherwise. IsStrict alone
// does not make a NULL: a constant is strict and reads no call, and so is the select item of
// `SELECT 1 ... ORDER BY COUNT(*)`, whose aggregation is left with no call at all -- 1 over no
// rows, not NULL (ADR 0023's update of 2026-11-08). A constant item keeps its constant, a NULL one
// excepted, which no select list the binder leaves here holds. A HAVING decides what the maps above
// it give: NULL where it rejects the empty row, their value where it keeps it, and not known
// otherwise.
std::map<ColumnId, EmptyValue> EmptyValues(const AggregateNode& aggregate, const Spine& spine,
                                           const std::vector<ColumnId>& outputs) {
  std::map<ColumnId, EmptyValue> values;
  for (const AggregateCall& call : aggregate.aggregates) {
    values.insert_or_assign(call.id, EmptyValueOf(call));
  }
  const auto value_of = [&values](const std::optional<BoundColumn>& column) -> EmptyValue {
    if (!column.has_value()) {
      return std::nullopt;
    }
    const auto it = values.find(column->id);
    return it == values.end() ? std::nullopt : it->second;
  };
  BranchTruth having = BranchTruth::kAlways;  // the HAVING clauses below, over the empty row
  const auto record = [&values, &having](ColumnId id, LogicalType type, EmptyValue value) {
    switch (having) {
      case BranchTruth::kNever:
        value = Constant{.type = type, .value = std::monostate{}};
        break;
      case BranchTruth::kUnknown:
        value = std::nullopt;
        break;
      case BranchTruth::kAlways:
        break;
    }
    values.insert_or_assign(id, std::move(value));
  };
  for (const Step& step : std::ranges::reverse_view(spine.steps)) {
    if (step.role == Role::kHaving) {
      for (const Predicate& predicate : std::get<FilterNode>(*step.node).predicates) {
        const BranchTruth truth =
            TruthOver(predicate, value_of(predicate.column), value_of(predicate.other));
        if (truth == BranchTruth::kNever) {
          having = BranchTruth::kNever;
        } else if (truth == BranchTruth::kUnknown && having == BranchTruth::kAlways) {
          having = BranchTruth::kUnknown;
        }
      }
      continue;
    }
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
        record(
            compute->ids[i], compute->exprs[i]->type,
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
        record(project.ids[i], constant->type,
               std::holds_alternative<std::monostate>(constant->value) ? EmptyValue()
                                                                       : EmptyValue(*constant));
        continue;
      }
      const auto it = values.find(project.columns[i].id);
      record(project.ids[i], project.columns[i].type,
             it == values.end() ? EmptyValue() : it->second);
    }
  }
  // The values the join appends from below the topmost HAVING, which it decides as well.
  if (const std::optional<std::set<ColumnId>> above = IdsAboveHaving(spine); above.has_value()) {
    for (const ColumnId id : outputs) {
      const auto it = values.find(id);
      if (above->contains(id) || it == values.end()) {
        continue;
      }
      const LogicalType type = it->second.has_value() ? it->second->type : LogicalType::kBigInt;
      record(id, type, it->second);
    }
  }
  return values;
}

// The static aggregate's rule in value mode: its calls, then the empty values of the columns it and
// the maps over it define. Any select item over the calls is answered: the consumer proof joins
// back INNER only where the comparison rejects the subquery's empty value, and LEFT otherwise,
// which gives an outer row with no group the item over the empty values (decision 21).
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
    return correlation.Refuse("whose aggregate is a single value");
  }
  values = EmptyValues(*aggregate, spine, OutputIds(*correlation.join.right));
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
ExprPtr ColumnValue(const BoundColumn& column) {
  return std::make_shared<const Expr>(
      Expr{.node = ColumnExpr{.id = column.id}, .type = column.type, .name = column.name});
}

ExprPtr IsNullValue(const ExprPtr& value) {
  const BoundColumn operand{.name = value->name, .type = value->type};
  return std::make_shared<const Expr>(Expr{
      .node =
          PredicateExpr{.predicate = Predicate{.kind = Predicate::Kind::kIsNull, .column = operand},
                        .operands = {value}},
      .type = LogicalType::kBoolean,
      .name = "IS NULL"});
}

ExprPtr IsNullValue(const BoundColumn& column) { return IsNullValue(ColumnValue(column)); }

ExprPtr MembershipMatch(const JoinKey& pair, bool nulls_match, ExprPtr item = nullptr) {
  if (item == nullptr) {
    item = ColumnValue(pair.right);
  }
  BoundColumn left = pair.left;
  left.index = 0;
  left.id = kNoColumnId;
  BoundColumn right = pair.right;
  right.index = 1;
  right.id = kNoColumnId;
  ExprPtr equality = std::make_shared<const Expr>(
      Expr{.node = PredicateExpr{.predicate = Predicate{.kind = Predicate::Kind::kCompareColumns,
                                                        .column = left,
                                                        .other = right,
                                                        .op = CompareOp::kEq},
                                 .operands = {ColumnValue(pair.left), item}},
           .type = LogicalType::kBoolean,
           .name = "IN"});
  if (!nulls_match) {
    return equality;
  }
  return std::make_shared<const Expr>(Expr{
      .node = BoolExpr{.op = BoolOp::kOr,
                       .args = {std::move(equality), IsNullValue(pair.left), IsNullValue(item)}},
      .type = LogicalType::kBoolean,
      .name = "NOT IN match"});
}

void AddMembership(const DependentJoinNode& join, std::vector<JoinKey>& keys,
                   std::vector<ExprPtr>& residual, const ExprPtr& deferred_item = nullptr) {
  if (!join.membership.has_value()) {
    return;
  }
  if (join.kind == JoinKind::kSemi && deferred_item == nullptr) {
    keys.push_back(*join.membership);
  } else {
    residual.push_back(
        MembershipMatch(*join.membership, join.kind == JoinKind::kAnti, deferred_item));
  }
}

// A second consumer needs its own column definitions. Only output ids can cross the cloned
// input's boundary; matching them by output position also handles Projects that rename columns.
struct ClonedInput {
  LogicalNodePtr node;
  std::map<ColumnId, ColumnId> ids;

  BoundColumn Rename(BoundColumn column) const {
    column.id = ids.at(column.id);
    return column;
  }
};

ClonedInput CloneInput(const LogicalNodePtr& input, ColumnId& last_id) {
  ClonedInput out{.node = CloneWithFreshIds(input, last_id), .ids = {}};
  const std::vector<ColumnId> before = OutputIds(*input);
  const std::vector<ColumnId> after = OutputIds(*out.node);
  for (std::size_t i = 0; i < before.size(); ++i) {
    out.ids.emplace(before[i], after.at(i));
  }
  return out;
}

// A rule's rewrite: the node that stands where the dependent join stood, and the right input of the
// join it makes, which Lemma 3.1's check reads. Lift mode's node is that join; value mode's may be
// the maps over a LEFT join back.
struct Rewritten {
  LogicalNodePtr node;
  LogicalNodePtr right;
  // Only this join's deferred membership comparison runs after its correlation matched.
  ExprPtr deferred_match = nullptr;
  // Decide whether to split this membership after the nested joins have been rewritten.
  std::optional<JoinKey> pending_lookups = std::nullopt;
};

// Equality-correlated NOT IN needs two lookups: the membership key, and per-binding row/NULL
// counts. The second has one row per correlation tuple, so even its residual visits at most one
// pair per outer row. Empty tuples have no group and therefore keep even a NULL probe. Residual
// correlations retain the general candidate-pair path; do not clone pending dependent joins or
// LIMITs that could choose different rows on their second evaluation.
LogicalNodePtr MembershipLookups(JoinNode join, const JoinKey& membership, ColumnId& last_id) {
  const auto fresh = [&last_id] {
    last_id = ColumnId{std::to_underlying(last_id) + 1};
    return last_id;
  };
  const ClonedInput counted = CloneInput(join.right, last_id);
  const BoundColumn rows{.id = fresh(), .name = "membership_rows", .type = LogicalType::kBigInt};
  const BoundColumn values{
      .id = fresh(), .name = "membership_values", .type = LogicalType::kBigInt};
  GroupAggregateNode groups{.input = counted.node,
                            .keys = {},
                            .key_ids = {},
                            .aggregates = {AggregateCall{.kind = AggKind::kCountStar,
                                                         .type = rows.type,
                                                         .id = rows.id,
                                                         .span = join.span},
                                           AggregateCall{.kind = AggKind::kCount,
                                                         .arg = counted.Rename(membership.right),
                                                         .type = values.type,
                                                         .id = values.id,
                                                         .span = join.span}},
                            .span = join.span};
  std::map<ColumnId, BoundColumn> grouped;
  std::vector<JoinKey> keys = join.keys;
  for (JoinKey& key : keys) {
    auto [found, inserted] = grouped.try_emplace(key.right.id, key.right);
    if (inserted) {
      groups.keys.push_back(counted.Rename(key.right));
      groups.key_ids.push_back(fresh());
      found->second.id = groups.key_ids.back();
    }
    key.right = found->second;
  }
  const ExprPtr has_null = std::make_shared<const Expr>(Expr{
      .node = PredicateExpr{.predicate =
                                Predicate{.kind = Predicate::Kind::kCompareColumns,
                                          .column = BoundColumn{.index = 0, .type = rows.type},
                                          .other = BoundColumn{.index = 1, .type = values.type},
                                          .op = CompareOp::kGt},
                            .operands = {ColumnValue(rows), ColumnValue(values)}},
      .type = LogicalType::kBoolean,
      .name = "membership_has_null"});
  join.left = Make(JoinNode{
      .kind = JoinKind::kAnti,
      .left = join.left,
      .right = Make(std::move(groups)),
      .keys = std::move(keys),
      .residual = {std::make_shared<const Expr>(Expr{
          .node = BoolExpr{.op = BoolOp::kOr, .args = {IsNullValue(membership.left), has_null}},
          .type = LogicalType::kBoolean,
          .name = "NULL membership"})},
      .build = BuildSide::kRight,
      .estimate = join.estimate,
      .span = join.span});
  join.keys.push_back(membership);
  // Preserve duplicates on the outer side without enumerating them as build-side candidates.
  join.build = BuildSide::kRight;
  return Make(std::move(join));
}

// Each hash-based NOT IN reads its set twice. Keep the candidate-pair plan around a nested
// NOT IN so a chain of them does not double the size of the plan at every level.
bool ContainsDependentNotIn(const LogicalNodePtr& node) {
  if (const auto* join = std::get_if<DependentJoinNode>(node.get());
      join != nullptr && join->membership.has_value() && join->kind == JoinKind::kAnti) {
    return true;
  }
  return std::ranges::any_of(InputsOf(*node), ContainsDependentNotIn);
}

// Approximate the correlation-key domain on the set rows that survive a nested EXISTS.
// Stop at a filter: the scan count can substantially overstate the rows it passes.
struct MembershipDomain {
  std::optional<int64_t> values;
  std::optional<Int128> min;
  std::optional<Int128> max;
};

MembershipDomain MembershipKeyDomain(const LogicalNodePtr& input, ColumnId id, int64_t rows) {
  if (const auto* scan = std::get_if<ScanNode>(input.get())) {
    const auto at = std::ranges::find(scan->ids, id);
    if (at == scan->ids.end()) {
      return {};
    }
    const int field = scan->fields.at(Narrow<std::size_t>(at - scan->ids.begin()));
    const ColumnRange range = ColumnRangeOf(*scan->table, field);
    return {.values = ColumnDomain(*scan->table, field, range), .min = range.min, .max = range.max};
  }
  if (const auto* project = std::get_if<ProjectNode>(input.get())) {
    const auto at = std::ranges::find(project->ids, id);
    return at == project->ids.end()
               ? MembershipDomain{}
               : MembershipKeyDomain(
                     project->input,
                     project->columns.at(Narrow<std::size_t>(at - project->ids.begin())).id, rows);
  }
  if (const auto* compute = std::get_if<ComputeNode>(input.get())) {
    if (const auto* definition = DefinedBy(*compute, id)) {
      if (const auto column = WidenedColumn(**definition)) {
        return MembershipKeyDomain(compute->input, *column, rows);
      }
      const auto* op = std::get_if<ArithExpr>(&(*definition)->node);
      if (op != nullptr && op->op == ArithOp::kModulo) {
        const auto* literal = std::get_if<ConstantExpr>(&op->right->node);
        const auto* divisor =
            literal == nullptr ? nullptr : std::get_if<Int128>(&literal->value.value);
        if (divisor != nullptr && *divisor > 0 && *divisor < rows) {
          // A signed remainder can be negative as well as positive.
          return {.values = static_cast<int64_t>(std::min<Int128>(rows, 2 * *divisor - 1)),
                  .min = 1 - *divisor,
                  .max = *divisor - 1};
        }
      }
      return {};
    }
    return MembershipKeyDomain(compute->input, id, rows);
  }
  if (const auto* join = std::get_if<JoinNode>(input.get());
      join != nullptr && Traits(join->kind).filters_left) {
    return MembershipKeyDomain(join->left, id, rows);
  }
  return {};
}

// Count probes only across row-preserving maps. A filter, LIMIT or another join could leave a
// single probe; avoid building the nested set a second time on a speculative row count.
bool WorthNestedLookups(const JoinNode& join, std::span<const Ancestor> above) {
  auto probe = join.left;
  while (std::holds_alternative<ProjectNode>(*probe) ||
         std::holds_alternative<ComputeNode>(*probe)) {
    probe = InputsOf(*probe).front();
  }
  if (!std::holds_alternative<ScanNode>(*probe) ||
      std::ranges::any_of(above, [](const Ancestor& step) {
        return std::holds_alternative<LimitNode>(*step.node);
      })) {
    return false;
  }
  const int64_t probes = EstimateRows(*probe).value_or(0);
  const int64_t rows = EstimateRows(*join.right).value_or(0);
  // Two hash builds and a grouped count cost more than a few candidate tests per row.
  constexpr int64_t kPairsPerRow = 32;
  if (std::min(rows, probes) <= kPairsPerRow) {
    return false;
  }
  Int128 pairs = Int128{rows} * probes;
  for (const JoinKey& key : join.keys) {
    const auto outer = MembershipKeyDomain(join.left, key.left.id, probes);
    const auto inner = MembershipKeyDomain(join.right, key.right.id, rows);
    if (!outer.values || !inner.values || (outer.min && inner.max && *outer.min > *inner.max) ||
        (inner.min && outer.max && *inner.min > *outer.max)) {
      return false;
    }
    pairs /= std::max({int64_t{1}, *outer.values, *inner.values});
  }
  return pairs >= kPairsPerRow * (Int128{rows} + probes);
}

Rewritten MembershipNode(const Correlation& correlation, JoinNode join,
                         const ExprPtr& deferred_item = nullptr) {
  Rewritten out{.node = nullptr, .right = join.right};
  const auto& membership = correlation.join.membership;
  if (membership && join.kind == JoinKind::kAnti && deferred_item == nullptr &&
      !join.keys.empty() && join.residual.empty()) {
    if (FirstDependentJoin(join.right) != nullptr) {
      if (!ContainsDependentNotIn(join.right)) {
        out.pending_lookups = membership;
      }
    } else if (!UnsafeToClone(join.right) && FreeVariables(*join.right).empty()) {
      out.node = MembershipLookups(std::move(join), *membership, correlation.last_id);
      return out;
    }
  }
  AddMembership(correlation.join, join.keys, join.residual, deferred_item);
  if (deferred_item != nullptr) {
    out.deferred_match = join.residual.back();
  }
  out.node = Make(std::move(join));
  return out;
}

// Visit each child, including CASE's optional condition and result arms.
template <class E, class F>
void VisitValueChildren(E& expr, const F& visit) {
  std::visit(
      [&visit](auto& node) {
        using Node = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<Node, ArithExpr>) {
          visit(node.left);
          visit(node.right);
        } else if constexpr (std::is_same_v<Node, NegateExpr> || std::is_same_v<Node, CastExpr>) {
          visit(node.operand);
        } else if constexpr (std::is_same_v<Node, FunctionExpr> || std::is_same_v<Node, BoolExpr>) {
          std::ranges::for_each(node.args, visit);
        } else if constexpr (std::is_same_v<Node, PredicateExpr>) {
          std::ranges::for_each(node.operands, visit);
        } else if constexpr (std::is_same_v<Node, CaseExpr>) {
          std::ranges::for_each(node.whens, visit);
          std::ranges::for_each(node.thens, visit);
          visit(node.otherwise);
        } else {
          static_assert(std::is_same_v<Node, ColumnExpr> || std::is_same_v<Node, ConstantExpr>);
        }
      },
      expr.node);
}

// A binding can group positive and negative zero together. A quotient can distinguish those
// bindings when its divisor reads the outer row, even though ordinary comparisons cannot.
bool ObservesBindingZeroSign(const Expr& expr, const std::set<ColumnId>& outer) {
  if (const auto* op = std::get_if<ArithExpr>(&expr.node);
      op != nullptr && op->op == ArithOp::kDivide && op->right->type == LogicalType::kDouble &&
      SidesOf(*op->right, outer).outer) {
    return true;
  }
  bool observes = false;
  VisitValueChildren(expr, [&](const ExprPtr& child) {
    observes = observes || (child && ObservesBindingZeroSign(*child, outer));
  });
  return observes;
}

ExprPtr InlineValues(const ExprPtr& expr, const std::map<ColumnId, ExprPtr>& values) {
  if (expr == nullptr) {
    return nullptr;
  }
  if (const auto* column = std::get_if<ColumnExpr>(&expr->node)) {
    const auto found = values.find(column->id);
    return found == values.end() ? expr : found->second;
  }
  Expr out = *expr;
  VisitValueChildren(out, [&](ExprPtr& child) { child = InlineValues(child, values); });
  return std::make_shared<const Expr>(std::move(out));
}

struct MembershipInput {
  LogicalNodePtr right;
  ExprPtr deferred_item;
};

// This is deliberately a proof of harmless evaluation, not IsStrict: a strict cast can still
// fail. Arithmetic and functions need the actual binding domain even when many inputs are safe.
bool SafeBeforeMembership(const Expr& expr) {
  return std::visit(
      [&expr](const auto& node) {
        using Node = std::decay_t<decltype(node)>;
        const auto safe = [](const ExprPtr& child) { return SafeBeforeMembership(*child); };
        if constexpr (std::is_same_v<Node, ColumnExpr> || std::is_same_v<Node, ConstantExpr>) {
          return true;
        } else if constexpr (std::is_same_v<Node, CastExpr>) {
          return IsExactWidening(node.operand->type, expr.type) && safe(node.operand);
        } else if constexpr (std::is_same_v<Node, PredicateExpr>) {
          return std::ranges::all_of(node.operands, safe);
        } else if constexpr (std::is_same_v<Node, BoolExpr>) {
          return std::ranges::all_of(node.args, safe);
        } else {
          return false;
        }
      },
      expr.node);
}

const LogicalNode* FirstUnsafeInnerNode(const LogicalNodePtr& input, bool guarded = false,
                                        const std::set<const LogicalNode*>& existing_inputs = {},
                                        const Expr* deferred_match = nullptr);

// Preserve inner maps, with Projects as maps so correlation columns remain available. An
// item that needs the outer row runs in the final residual, after correlation. Only the selected
// value is inlined; aliases reuse their definition.
// A risky inner item also runs after correlation for IN, or for NOT IN with residual correlation;
// plain NOT IN keeps DuckDB's eager evaluation. Safe inner maps retain the hash-key path. All
// qualifying pairs are evaluated, including later items that can fail after an earlier item
// matched.
arrow::Result<MembershipInput> MembershipMaps(const Correlation& correlation, const Spine& spine,
                                              const BoundColumn& item, bool has_residual,
                                              std::span<const JoinKey> keys = {}) {
  LogicalNodePtr input = spine.below;
  std::set<ColumnId> outer = correlation.outer;
  bool mixed = false;
  std::map<ColumnId, ExprPtr> values;
  for (const Step& step : std::ranges::reverse_view(spine.steps)) {
    ComputeNode maps{.input = input, .exprs = {}, .ids = {}, .span = correlation.join.span};
    if (const auto* project = std::get_if<ProjectNode>(step.node.get())) {
      maps.span = project->span;
      for (std::size_t i = 0; i < project->ids.size(); ++i) {
        const std::optional<Constant> constant =
            i < project->constants.size() ? project->constants[i] : std::nullopt;
        if (!constant.has_value() && project->ids[i] == project->columns[i].id) {
          continue;
        }
        maps.ids.push_back(project->ids[i]);
        maps.exprs.push_back(constant.has_value() ? std::make_shared<const Expr>(Expr{
                                                        .node = ConstantExpr{.value = *constant},
                                                        .type = project->columns[i].type,
                                                        .name = project->columns[i].name})
                                                  : ColumnValue(project->columns[i]));
      }
    } else if (const auto* compute = std::get_if<ComputeNode>(step.node.get())) {
      maps = *compute;
      maps.input = input;
    }
    ComputeNode inner{.input = input, .exprs = {}, .ids = {}, .span = maps.span};
    for (std::size_t i = 0; i < maps.exprs.size(); ++i) {
      const Sides sides = SidesOf(*maps.exprs[i], outer);
      mixed |= sides.both();
      std::vector<ColumnId> reads;
      CollectColumnIds(*maps.exprs[i], reads);
      const bool reads_deferred =
          std::ranges::any_of(reads, [&values](ColumnId id) { return values.contains(id); });
      if (sides.outer || reads_deferred ||
          ((has_residual || correlation.join.kind == JoinKind::kSemi) &&
           !SafeBeforeMembership(*maps.exprs[i]))) {
        values.emplace(maps.ids[i], InlineValues(maps.exprs[i], values));
        if (sides.outer) {
          outer.insert(maps.ids[i]);
        }
      } else {
        inner.exprs.push_back(maps.exprs[i]);
        inner.ids.push_back(maps.ids[i]);
      }
    }
    if (!inner.exprs.empty()) {
      input = Make(std::move(inner));
    }
  }
  ExprPtr selected = outer.contains(item.id) || values.contains(item.id)
                         ? InlineValues(ColumnValue(item), values)
                         : nullptr;
  if (mixed) {
    // A checked expression can raise on a different set of rows when DuckDB substitutes an
    // equality key. Include hidden sort items, correlation predicates and derived inputs in
    // the proof: a harmless selected column can name a checked expression below the filter.
    const auto safe = [](const ExprPtr& expr) { return CanEvaluateOnUnmatchedRow(*expr, nullptr); };
    for (const Step& step : spine.steps) {
      const auto* compute = std::get_if<ComputeNode>(step.node.get());
      if (compute != nullptr && !std::ranges::all_of(compute->exprs, safe)) {
        return correlation.Refuse("with a potentially failing mixed projection or sort item");
      }
    }
    if ((selected && ObservesBindingZeroSign(*selected, correlation.outer)) ||
        std::ranges::any_of(spine.computed->exprs, [&](const ExprPtr& expr) {
          return ObservesBindingZeroSign(*expr, correlation.outer);
        })) {
      return correlation.Refuse(
          "with a mixed projection that can observe the sign of a binding's zero");
    }
    if (!std::ranges::all_of(spine.computed->exprs, safe) ||
        FirstUnsafeInnerNode(spine.below, true) != nullptr) {
      return correlation.Refuse("with a mixed projection over a potentially failing set input");
    }
    if (FirstUnsafeInnerNode(correlation.join.left, true) != nullptr) {
      return correlation.Refuse("with a mixed projection over a potentially failing outer input");
    }
    // Equality can supply every outer value in an item. Compute it once per set row so that
    // repeated correlation keys still use the ordinary hash membership path.
    if (selected && SidesOf(*selected, correlation.outer).both()) {
      const auto available = OutputIds(*input);
      std::map<ColumnId, ExprPtr> equivalents;
      for (const JoinKey& key : keys) {
        if (correlation.outer.contains(key.left.id) &&
            std::ranges::contains(available, key.right.id)) {
          equivalents.try_emplace(key.left.id, ColumnValue(key.right));
        }
      }
      ExprPtr local = InlineValues(selected, equivalents);
      if (!SidesOf(*local, correlation.outer).outer) {
        input = Make(ComputeNode{.input = std::move(input),
                                 .exprs = {std::move(local)},
                                 .ids = {item.id},
                                 .span = correlation.join.span});
        selected.reset();
      }
    }
  }
  return MembershipInput{.right = std::move(input), .deferred_item = std::move(selected)};
}

arrow::Result<Rewritten> LiftJoin(const Correlation& correlation, const Spine& spine) {
  const DependentJoinNode& join = correlation.join;
  ColumnId& last_id = correlation.last_id;
  std::vector<JoinKey> keys;
  std::vector<ExprPtr> residual;
  std::vector<ExprPtr> inner_casts;  // a key's inner side widened to the key type
  std::vector<ColumnId> inner_cast_ids;
  std::vector<ExprPtr> outer_casts;  // and its outer side
  std::vector<ColumnId> outer_cast_ids;
  std::vector<ExprPtr> inner_keys;
  std::vector<ColumnId> inner_key_ids;
  std::vector<ExprPtr> outer_keys;
  std::vector<ColumnId> outer_key_ids;

  ARROW_ASSIGN_OR_RAISE(const std::vector<const ExprPtr*> conjuncts,
                        ConjunctsOf(correlation, spine));
  for (const ExprPtr* defined : conjuncts) {
    ExprPtr expr = *defined;
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
      // the anti join keeps the row. A mark keeps every outer row, so there a filter would lose
      // the row whose mark is FALSE; the residual marks it so.
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
    // Membership needs a correlation key even when its NOT IN comparison is a residual.
    // Materialize a computed side only for an equality between independent inputs; a mixed
    // expression remains a residual. The ordinary EXISTS/value rules keep their existing scope.
    if (join.membership.has_value()) {
      const auto* predicate = std::get_if<PredicateExpr>(&expr->node);
      if (predicate != nullptr && predicate->operands.size() == 2 &&
          (predicate->predicate.kind == Predicate::Kind::kIsNotDistinct ||
           (predicate->predicate.kind == Predicate::Kind::kCompareColumns &&
            predicate->predicate.op == CompareOp::kEq)) &&
          KeyType(predicate->operands[0]->type, predicate->operands[1]->type).has_value()) {
        const Sides a = SidesOf(*predicate->operands[0], correlation.outer);
        const Sides b = SidesOf(*predicate->operands[1], correlation.outer);
        if ((a.outer_only() && b.inner && !b.outer) || (b.outer_only() && a.inner && !a.outer)) {
          PredicateExpr keyed = *predicate;
          for (ExprPtr& operand : keyed.operands) {
            if (std::holds_alternative<ColumnExpr>(operand->node)) {
              continue;
            }
            const bool outer = SidesOf(*operand, correlation.outer).outer_only();
            last_id = ColumnId{std::to_underlying(last_id) + 1};
            (outer ? outer_keys : inner_keys).push_back(operand);
            (outer ? outer_key_ids : inner_key_ids).push_back(last_id);
            operand = ColumnValue(
                BoundColumn{.id = last_id, .name = operand->name, .type = operand->type});
          }
          Expr changed = *expr;
          changed.node = std::move(keyed);
          expr = std::make_shared<const Expr>(std::move(changed));
        }
      }
    }
    std::set<ColumnId> key_outer = correlation.outer;
    key_outer.insert(outer_key_ids.begin(), outer_key_ids.end());
    if (const std::optional<KeyParts> key = AsKey(*expr, key_outer)) {
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

  LogicalNodePtr left = join.left;
  LogicalNodePtr right = spine.below;
  ExprPtr deferred_item;
  if (join.membership.has_value()) {
    ARROW_ASSIGN_OR_RAISE(auto mapped, MembershipMaps(correlation, spine, join.membership->right,
                                                      !residual.empty(), keys));
    right = std::move(mapped.right);
    deferred_item = std::move(mapped.deferred_item);
  } else if (Traits(join.kind).single_match) {
    // PlainScalarMaps proves these maps can run on the build before the residual.
    const auto& item = std::get<ProjectNode>(*join.right).columns.front();
    ARROW_ASSIGN_OR_RAISE(auto mapped, MembershipMaps(correlation, spine, item, false));
    right = std::move(mapped.right);
  }
  if (keys.empty() && !Traits(join.kind).single_match &&
      (!join.membership.has_value() || join.kind != JoinKind::kSemi || deferred_item != nullptr)) {
    // ADR 0023: as a LEFT JOIN without one does in ADR 0022, since the hash table and the
    // candidate pairs need a key (ADR 0040's decision 3).
    return correlation.Refuse("without a key");
  }

  if (!outer_keys.empty()) {
    left = Make(ComputeNode{
        .input = std::move(left), .exprs = outer_keys, .ids = outer_key_ids, .span = join.span});
  }
  if (!outer_casts.empty()) {
    left = Make(ComputeNode{
        .input = std::move(left), .exprs = outer_casts, .ids = outer_cast_ids, .span = join.span});
  }
  if (!inner_keys.empty()) {
    right = Make(ComputeNode{
        .input = std::move(right), .exprs = inner_keys, .ids = inner_key_ids, .span = join.span});
  }
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
  return MembershipNode(
      correlation,
      JoinNode{.kind = join.kind,
               .left = std::move(left),
               .right = std::move(right),
               .keys = std::move(keys),
               .residual = std::move(residual),
               .mark = join.mark,  // the id the binder gave it, which the plan reads above
               .build = build,
               // A semi or an anti join keeps a subset of its left input, and a mark join
               // every row of it, so that is an upper bound: without it every join above
               // this rewrite would read its own left input as unknown.
               .estimate = left_rows,
               .span = join.span},
      deferred_item);
}

// A simple selected value can be carried directly by the hash build while the keys and
// residuals are tested on the probe. Other maps and grouped blocks need per-binding evaluation.
bool PlainScalarMaps(const Correlation& correlation, const Spine& spine) {
  const auto* output = std::get_if<ProjectNode>(correlation.join.right.get());
  if (output == nullptr || output->columns.size() != 1) {
    return false;
  }
  return std::ranges::all_of(spine.steps, [&](const Step& step) {
    if (step.role == Role::kDropped) {
      return true;
    }
    if (step.role != Role::kMap) {
      return false;
    }
    if (const auto* project = std::get_if<ProjectNode>(step.node.get())) {
      return std::ranges::none_of(project->columns, [&](const BoundColumn& column) {
        return correlation.outer.contains(column.id);
      });
    }
    return std::ranges::all_of(std::get<ComputeNode>(*step.node).exprs, [&](const ExprPtr& expr) {
      return SafeBeforeMembership(*expr) && !SidesOf(*expr, correlation.outer).outer;
    });
  });
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
// Only correlated equalities in the block's top filter feed these classes (ADR 0023).
//
// That is narrower than the ADR and deliberately so. An inner-inner equality can only extend a
// class that already holds an inner member, so it never gives an outer column its first inner
// equivalent: which shapes are accepted is the same either way, and only the choice of
// representative could differ. What it buys is two whole classes of wrong answer that cannot then
// arise: a LEFT JOIN's ON key, which is not enforced on padded rows and would make the group key
// non-NULL exactly where the correlation was NULL, and a conjunct of a nested dependent join,
// which an anti join keeps the rows of where it is false. Both would be silent.
struct Equivalences {
  ColumnClasses classes;
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

// A scalar or a mark appends one value per left row. Stacked subqueries over left columns can
// therefore use the same binding source without recursively copying the pending dependent joins.
// Keep filters, computed keys and padded rows: evaluating aggregates for extra bindings can raise
// errors even though the final join would discard them.
LogicalNodePtr ExactBindingSource(LogicalNodePtr node, std::set<ColumnId> bindings) {
  while (const auto* dependent = std::get_if<DependentJoinNode>(node.get())) {
    const JoinKindTraits traits = Traits(dependent->kind);
    if (!traits.one_row && !traits.appends_mark && !traits.single_match) {
      return nullptr;
    }
    node = dependent->left;
  }
  const std::vector<ColumnId> outputs = OutputIds(*node);
  if (!std::ranges::all_of(bindings, [&outputs](ColumnId id) {
        return std::ranges::find(outputs, id) != outputs.end();
      })) {
    return nullptr;
  }
  if (FirstDependentJoin(node) == nullptr) {
    return !FreeVariables(*node).empty() || UnsafeToClone(node) ? nullptr : node;
  }
  // A derived table can project both the binding columns and an earlier subquery's result.
  // Rebuild only the columns the domain reads, keeping the projection's output ids and aliases.
  if (const auto* project = std::get_if<ProjectNode>(node.get())) {
    ProjectNode kept{.span = project->span};
    std::set<ColumnId> reads;
    for (std::size_t i = 0; i < project->ids.size(); ++i) {
      if (bindings.contains(project->ids[i])) {
        kept.ids.push_back(project->ids[i]);
        kept.columns.push_back(project->columns.at(i));
        if (!project->constants.empty()) {
          kept.constants.push_back(project->constants.at(i));
        }
        if (kept.columns.back().id != kNoColumnId) {
          reads.insert(kept.columns.back().id);
        }
      }
    }
    kept.input = ExactBindingSource(project->input, std::move(reads));
    return kept.input == nullptr || kept.ids.empty() ? kept.input : Make(std::move(kept));
  }
  if (const auto* compute = std::get_if<ComputeNode>(node.get())) {
    ComputeNode kept{.span = compute->span};
    std::vector<ColumnId> reads;
    for (std::size_t i = 0; i < compute->ids.size(); ++i) {
      if (bindings.erase(compute->ids[i]) != 0) {
        kept.ids.push_back(compute->ids[i]);
        kept.exprs.push_back(compute->exprs.at(i));
        CollectColumnIds(*kept.exprs.back(), reads);
      }
    }
    bindings.insert(reads.begin(), reads.end());
    kept.input = ExactBindingSource(compute->input, std::move(bindings));
    return kept.input == nullptr || kept.ids.empty() ? kept.input : Make(std::move(kept));
  }
  return nullptr;
}

// A boolean combination stays one residual: an equality inside OR or NOT cannot key the join.
// Collect every outer column it reads so a NULL in one comparison can still pass another.
bool CollectDomainColumns(const Expr& expr, const std::set<ColumnId>& outer_ids,
                          const std::vector<ColumnId>& inner_ids,
                          std::map<ColumnId, BoundColumn>& columns) {
  if (const auto* boolean = std::get_if<BoolExpr>(&expr.node)) {
    return std::ranges::all_of(boolean->args, [&](const ExprPtr& arg) {
      return CollectDomainColumns(*arg, outer_ids, inner_ids, columns);
    });
  }
  const auto pair = ComparedColumns(expr, outer_ids);
  if (!pair.has_value() || pair->inner_type != pair->outer_type ||
      std::ranges::find(inner_ids, pair->inner.id) == inner_ids.end()) {
    return false;
  }
  columns.emplace(
      pair->outer.id,
      BoundColumn{.id = pair->outer.id, .name = pair->outer_name, .type = pair->outer_type});
  return true;
}

// SQ9's second terminal. D has one row per complete binding. Without an equality anchor
// the existing nested-loop join evaluates the correlation before emitting each passing pair.
arrow::Result<Representatives> ScalarDomain(const Correlation& correlation, const Spine& spine,
                                            Equivalences& equivalences, LogicalNodePtr& input,
                                            const LogicalNodePtr& single_build = nullptr) {
  const auto refuse = [&correlation] {
    return correlation.Refuse("with an outer column that has no inner equivalent");
  };
  const std::vector<ColumnId> bindings = FreeVariables(*correlation.join.right);
  const LogicalNodePtr source =
      ExactBindingSource(correlation.join.left, {bindings.begin(), bindings.end()});
  if (source == nullptr || !FreeVariables(*input).empty()) {
    return refuse();
  }
  const std::vector<ColumnId> inner = OutputIds(*input);
  std::map<ColumnId, BoundColumn> columns;
  std::vector<JoinKey> anchors;
  std::vector<ExprPtr> residual;
  ARROW_ASSIGN_OR_RAISE(const auto conjuncts, ConjunctsOf(correlation, spine));
  for (const ExprPtr* conjunct : conjuncts) {
    if (!CollectDomainColumns(**conjunct, correlation.outer, inner, columns)) {
      return refuse();
    }
    if (const auto pair = AsKey(**conjunct, correlation.outer); pair.has_value()) {
      anchors.push_back(JoinKey{
          .left =
              BoundColumn{.id = pair->inner.id, .name = pair->inner_name, .type = pair->inner_type},
          .right =
              BoundColumn{.id = pair->outer.id, .name = pair->outer_name, .type = pair->outer_type},
          .null_equal = pair->null_equal});
    } else {
      residual.push_back(*conjunct);
    }
  }
  if (!std::ranges::all_of(bindings, [&columns](ColumnId id) { return columns.contains(id); })) {
    return refuse();
  }
  const ClonedInput clone = CloneInput(source, correlation.last_id);
  GroupAggregateNode domain{.input = clone.node, .span = correlation.join.span};
  Representatives out;
  for (const auto& [id, column] : columns) {
    domain.keys.push_back(clone.Rename(column));
    const ColumnId fresh{std::to_underlying(correlation.last_id) + 1};
    correlation.last_id = fresh;
    domain.key_ids.push_back(fresh);
    BoundColumn grouped = column;
    grouped.id = fresh;
    equivalences.described.insert_or_assign(id, column);
    equivalences.described.emplace(fresh, std::move(grouped));
    out.of.emplace(id, fresh);
    out.null_equal.insert(id);  // the join back identifies the binding, including NULL
  }
  for (JoinKey& key : anchors) {
    key.right.id = out.of.at(key.right.id);
  }
  equivalences.kept.clear();
  if (anchors.empty()) {
    for (ExprPtr& expr : residual) {
      expr = MapColumns(expr, [&out](ColumnExpr column) {
        if (const auto it = out.of.find(column.id); it != out.of.end()) {
          column.id = it->second;
        }
        return column;
      });
    }
  } else {
    // Preserve the keyed plan: its remaining conjuncts run above the hash join.
    for (const ExprPtr& expr : residual) {
      equivalences.kept.push_back(expr.get());
    }
    residual.clear();
  }
  if (single_build != nullptr) {
    // The build already carries the scalar item. Check cardinality during the probe,
    // once per distinct binding, before the join back repeats its value for outer rows.
    ANTB1_CHECK(anchors.empty());
    input = Make(JoinNode{.kind = JoinKind::kSingle,
                          .left = Make(std::move(domain)),
                          .right = single_build,
                          .residual = std::move(residual),
                          .build = BuildSide::kRight,
                          .span = correlation.join.span});
    return out;
  }
  // An empty inner build short-circuits the nested loop before it opens the binding domain.
  const BuildSide build =
      anchors.empty() && EstimateRows(*input) == 0 ? BuildSide::kLeft : BuildSide::kRight;
  input = Make(JoinNode{.kind = JoinKind::kInner,
                        .left = std::move(input),
                        .right = Make(std::move(domain)),
                        .keys = std::move(anchors),
                        .residual = std::move(residual),
                        .build = build,
                        .span = correlation.join.span});
  return out;
}

// Every outer column the subquery reads needs a representative among the columns below the
// correlated filter, and gets its OWN key: one key per class would drop a second outer column's
// constraint and keep rows SQL rejects. A scalar aggregate can also obtain its representatives
// from the distinct bindings attached to its input.
arrow::Result<Representatives> RepresentativesOf(const Correlation& correlation, const Spine& spine,
                                                 Equivalences& equivalences,
                                                 LogicalNodePtr* domain_input = nullptr,
                                                 Equivalences* enabled = nullptr) {
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
    // The lifted guard still enforces its outer equalities on every row that reads a group.
    // Use that original class only for a surviving reference without a direct representative.
    if (found == kNoColumnId && enabled != nullptr) {
      const auto member = std::ranges::find_if(
          inner_ids, [&](ColumnId candidate) { return enabled->classes.Same(id, candidate); });
      if (member != inner_ids.end()) {
        found = *member;
        equivalences.described.try_emplace(id, enabled->described.at(id));
      }
    }
    if (found == kNoColumnId) {
      if (domain_input != nullptr) {
        return ScalarDomain(correlation, spine, equivalences, *domain_input);
      }
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
  const auto existing = equivalences.kept.size();
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
  if (Traits(correlation.join.kind).single_match) {
    // Surviving equality keys still decide which rows can reach a throwing residual.
    std::ranges::rotate(equivalences.kept,
                        equivalences.kept.begin() + Narrow<std::ptrdiff_t>(existing));
  }
  return out;
}

LogicalNodePtr FilterScalarBindings(LogicalNodePtr input, const ExprPtr& condition,
                                    ColumnId& last_id, SourceSpan span) {
  last_id = ColumnId{std::to_underlying(last_id) + 1};
  const BoundColumn tested{.id = last_id, .name = condition->name, .type = LogicalType::kBoolean};
  auto computed = Make(ComputeNode{
      .input = std::move(input), .exprs = {condition}, .ids = {tested.id}, .span = span});
  return Make(FilterNode{.input = std::move(computed),
                         .predicates = {{.kind = Predicate::Kind::kIsTrue, .column = tested}},
                         .span = span});
}

// A range wider than its population can contain mostly gaps and one very frequent value.
bool RangeFitsRows(const std::optional<Int128>& min, const std::optional<Int128>& max,
                   int64_t rows) {
  Int128 width = 0;
  return min && max && !__builtin_sub_overflow(*max, *min, &width) && width >= 0 && width < rows;
}

std::optional<int64_t> CompactDomain(const Table& table, int field, const ColumnRange& range) {
  return range.exact && RangeFitsRows(range.min, range.max, range.non_null)
             ? ColumnDomain(table, field, range)
             : ColumnDomain(table, field, ColumnRange{});
}

// A global range can look dense simply because every part spans the same wide interval.
// Require a dense range within each part as well before speculatively re-reading a large
// outer table. A recorded distinct count can establish the group count directly.
bool ManyScalarGroups(const Table& table, int field, bool speculative) {
  if (speculative) {
    for (int64_t part = 0; part < table.num_parts(); ++part) {
      const auto stats = table.part_stats(part, field);
      if (!stats || stats->null_count < 0 ||
          (stats->rows > stats->null_count &&
           !RangeFitsRows(stats->min, stats->max, stats->rows - stats->null_count))) {
        return ColumnDomain(table, field, ColumnRange{}).value_or(0) >= kMinRestrictionRows;
      }
    }
  }
  return CompactDomain(table, field, ColumnRangeOf(table, field)).value_or(0) >=
         kMinRestrictionRows;
}

// Estimate a simple guard on the outer scan using the join planner's footer statistics.
// For AND, each conjunct bounds the result on its own; do not assume they are independent.
int64_t EnabledScanRows(const Expr& condition, const ScanNode& scan, int64_t rows) {
  if (const auto* both = std::get_if<BoolExpr>(&condition.node);
      both != nullptr && both->op == BoolOp::kAnd) {
    int64_t kept = rows;
    for (const ExprPtr& arg : both->args) {
      kept = std::min(kept, EnabledScanRows(*arg, scan, rows));
    }
    return kept;
  }
  const auto* test = std::get_if<PredicateExpr>(&condition.node);
  if (test == nullptr || test->operands.size() != 1) {
    return rows;
  }
  const auto* column = std::get_if<ColumnExpr>(&test->operands.front()->node);
  if (column == nullptr) {
    return rows;
  }
  const auto at = std::ranges::find(scan.ids, column->id);
  if (at == scan.ids.end()) {
    return rows;
  }
  const int field = scan.fields.at(Narrow<std::size_t>(at - scan.ids.begin()));
  const ColumnRange range = ColumnRangeOf(*scan.table, field);
  if (test->predicate.kind == Predicate::Kind::kIsNull ||
      test->predicate.kind == Predicate::Kind::kIsNotNull) {
    return range.exact ? PredicateRows(rows, test->predicate, range, std::nullopt) : rows;
  }
  const auto domain = CompactDomain(*scan.table, field, range);
  return domain.has_value() ? PredicateRows(rows, test->predicate, range, domain) : rows;
}

// Limit the speculative read and the membership table to a few hundred bindings. This
// prefilter only earns its extra scan when the guard is expected to fit in that set.
constexpr int64_t kMaxPrefilterRows = 512;

// LEFT must preserve disabled rows, but their keys need no aggregate. Re-read a plain scan
// only when it is no larger than the input and the expected enabled rows are few relative
// to the groups. An enabled row can request at most one group, even with duplicate keys.
bool RestrictSelectiveBindings(const LogicalNodePtr& outer, const Expr& condition,
                               const LogicalNodePtr& inner, std::span<const ColumnId> keys) {
  const auto* scan = std::get_if<ScanNode>(outer.get());
  const auto* grouped_scan = std::get_if<ScanNode>(inner.get());
  if (scan == nullptr || grouped_scan == nullptr || keys.size() != 1) {
    return false;
  }
  const auto rows = scan->table->exact_row_count();
  const int64_t inner_rows = EstimateRows(*inner).value_or(0);
  if (!rows.has_value() || *rows <= 0 || *rows > inner_rows || inner_rows < kMinRestrictionRows) {
    return false;
  }
  const int64_t enabled = EnabledScanRows(condition, *scan, *rows);
  if (enabled >= *rows || enabled > kMaxPrefilterRows) {
    return false;
  }
  const auto key = std::ranges::find(grouped_scan->ids, keys.front());
  if (key == grouped_scan->ids.end()) {
    return false;
  }
  const int field = grouped_scan->fields.at(Narrow<std::size_t>(key - grouped_scan->ids.begin()));
  return ManyScalarGroups(*grouped_scan->table, field, *rows > kMaxPrefilterRows);
}

// A bounded set is enough for an optional prefilter. Above the limit a NULL in the
// membership set makes every nonmatch UNKNOWN, so the probe keeps every row. Exclude real NULL
// keys from the set and retain the NULL group separately for a NULL-equal correlation. The
// outer join and guard still decide the answer.
// A checked aggregate must never use this prefilter: it needs the exact set of enabled keys.
LogicalNodePtr PrefilterScalarGroups(const Correlation& correlation, const LogicalNodePtr& input,
                                     const LogicalNodePtr& domain, JoinKey key) {
  const auto fresh = [&correlation] {
    correlation.last_id = ColumnId{std::to_underlying(correlation.last_id) + 1};
    return correlation.last_id;
  };
  const SourceSpan span = correlation.join.span;
  auto non_null =
      Make(FilterNode{.input = domain,
                      .predicates = {{.kind = Predicate::Kind::kIsNotNull, .column = key.right}},
                      .span = span});
  auto limited =
      Make(LimitNode{.input = std::move(non_null), .limit = kMaxPrefilterRows + 1, .span = span});
  const BoundColumn rows{.id = fresh(), .name = "enabled_rows", .type = LogicalType::kBigInt};
  auto counted = Make(WindowNode{.input = std::move(limited),
                                 .calls = {WindowCall{.frame = WindowCall::Frame::kWholePartition,
                                                      .name = rows.name,
                                                      .id = rows.id,
                                                      .span = span,
                                                      .over_span = span}},
                                 .span = span});
  const auto small = std::make_shared<const Expr>(Expr{
      .node =
          PredicateExpr{
              .predicate = Predicate{.kind = Predicate::Kind::kCompare,
                                     .column = BoundColumn{.name = rows.name, .type = rows.type},
                                     .op = CompareOp::kLe,
                                     .constant = Constant{.type = rows.type,
                                                          .value = Int128{kMaxPrefilterRows}}},
              .operands = {ColumnValue(rows)}},
      .type = LogicalType::kBoolean,
      .name = "small enabled set"});
  auto bounded = std::make_shared<const Expr>(Expr{
      .node = CaseExpr{.whens = {small}, .thens = {ColumnValue(key.right)}, .otherwise = nullptr},
      .type = key.right.type,
      .name = "enabled key"});
  const auto null_group = key.null_equal ? IsNullValue(key.left) : nullptr;
  key.right.id = fresh();
  key.null_equal = false;
  auto build = Make(ComputeNode{.input = std::move(counted),
                                .exprs = {std::move(bounded)},
                                .ids = {key.right.id},
                                .span = span});
  const BoundColumn mark{
      .id = fresh(), .name = "enabled key membership", .type = LogicalType::kBoolean};
  auto marked = Make(JoinNode{.kind = JoinKind::kNullAwareMark,
                              .left = input,
                              .right = std::move(build),
                              .keys = {std::move(key)},
                              .mark = mark.id,
                              .span = span});
  std::vector<ExprPtr> possible{ColumnValue(mark), IsNullValue(mark)};
  if (null_group != nullptr) {
    possible.push_back(null_group);
  }
  auto keep = std::make_shared<const Expr>(
      Expr{.node = BoolExpr{.op = BoolOp::kOr, .args = std::move(possible)},
           .type = LogicalType::kBoolean,
           .name = "possibly enabled"});
  return FilterScalarBindings(std::move(marked), keep, correlation.last_id, span);
}

// Evaluate error-prone maps only for actual bindings.
arrow::Result<LogicalNodePtr> RestrictBindingInput(const Correlation& correlation,
                                                   const LogicalNodePtr& input,
                                                   const std::map<ColumnId, BoundColumn>& carried,
                                                   const Representatives& representatives,
                                                   const Equivalences& equivalences,
                                                   const ExprPtr& condition = nullptr,
                                                   bool prefilter_only = false) {
  LogicalNodePtr outer = correlation.join.left;
  if (Traits(correlation.join.kind).single_match || correlation.mode == Mode::kValue) {
    std::set<ColumnId> bindings;
    for (const auto& [id, inner] : carried) {
      bindings.insert(id);
    }
    if (condition != nullptr) {
      std::vector<ColumnId> reads;
      CollectColumnIds(*condition, reads);
      bindings.insert(reads.begin(), reads.end());
    }
    outer = ExactBindingSource(outer, std::move(bindings));
  }
  if (outer == nullptr || FirstDependentJoin(outer) != nullptr || !FreeVariables(*outer).empty() ||
      UnsafeToClone(outer)) {
    return correlation.Refuse(
        "whose selected or grouped expression needs an exact outer domain over subqueries or "
        "LIMIT");
  }
  if (condition != nullptr) {
    outer = FilterScalarBindings(std::move(outer), condition, correlation.last_id,
                                 correlation.join.span);
  }
  const ClonedInput domain = CloneInput(outer, correlation.last_id);
  std::vector<JoinKey> keys;
  keys.reserve(carried.size());
  for (const auto& [id, inner] : carried) {
    keys.push_back(JoinKey{.left = inner,
                           .right = domain.Rename(equivalences.described.at(id)),
                           .null_equal = representatives.null_equal.contains(id)});
  }
  if (prefilter_only) {
    return PrefilterScalarGroups(correlation, input, domain.node, keys.front());
  }
  const auto estimate = EstimateRows(*input);
  return Make(JoinNode{.kind = JoinKind::kSemi,
                       .left = input,
                       .right = domain.node,
                       .keys = std::move(keys),
                       .build = BuildSide::kRight,
                       .estimate = estimate,
                       .span = correlation.join.span});
}

// These unary operators cannot emit a row from an empty scan. Join estimates are
// deliberately excluded: zero estimated rows is not a proof that a LEFT join may drop rows.
bool ReadsEmptyScan(const LogicalNodePtr& input) {
  if (const auto* scan = std::get_if<ScanNode>(input.get())) {
    return scan->table->exact_row_count() == 0;
  }
  if (std::holds_alternative<ProjectNode>(*input) || std::holds_alternative<ComputeNode>(*input) ||
      std::holds_alternative<FilterNode>(*input) || std::holds_alternative<LimitNode>(*input) ||
      std::holds_alternative<SortNode>(*input)) {
    return ReadsEmptyScan(InputsOf(*input).front());
  }
  return false;
}

// A derived table may hide a checked expression behind an ordinary column reference.
const LogicalNode* FirstUnsafeInnerNode(const LogicalNodePtr& input, bool guarded,
                                        const std::set<const LogicalNode*>& existing_inputs,
                                        const Expr* deferred_match) {
  if (existing_inputs.contains(input.get())) {
    return nullptr;
  }
  for (const auto& child : InputsOf(*input)) {
    if (const auto* unsafe = FirstUnsafeInnerNode(child, guarded, existing_inputs)) {
      return unsafe;
    }
  }
  const bool safe = std::visit(
      [guarded, deferred_match](const auto& node) {
        using Node = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<Node, ComputeNode>) {
          return std::ranges::all_of(node.exprs, [&](const ExprPtr& expr) {
            return guarded ? CanEvaluateOnUnmatchedRow(*expr, node.input)
                           : CanEvaluateUnboundProjection(*expr, node.input);
          });
        } else if constexpr (std::is_same_v<Node, FilterNode>) {
          return !guarded || std::ranges::all_of(node.predicates, CanSkipPredicate);
        } else if constexpr (std::is_same_v<Node, AggregateNode> ||
                             std::is_same_v<Node, GroupAggregateNode> ||
                             std::is_same_v<Node, GroupingSetsNode>) {
          return std::ranges::none_of(node.aggregates, [](const AggregateCall& call) {
            return call.kind == AggKind::kSingleValue || CanFailOnGroup(call);
          });
        } else if constexpr (std::is_same_v<Node, JoinNode>) {
          const auto traits = Traits(node.kind);
          return !traits.single_match &&
                 std::ranges::all_of(node.residual, [&](const ExprPtr& expr) {
                   return expr.get() == deferred_match ||
                          (guarded ? CanEvaluateOnUnmatchedRow(*expr, nullptr)
                                   : SafeBeforeMembership(*expr));
                 });
        } else if constexpr (std::is_same_v<Node, DependentJoinNode>) {
          return !guarded && !Traits(node.kind).single_match;
        } else {
          return guarded
                     ? (std::is_same_v<Node, ScanNode> || std::is_same_v<Node, ProjectNode> ||
                        std::is_same_v<Node, LimitNode> || std::is_same_v<Node, SortNode> ||
                        std::is_same_v<Node, RowCountNode> || std::is_same_v<Node, UnionAllNode> ||
                        std::is_same_v<Node, SetOperationNode>)
                     : !std::is_same_v<Node, WindowNode>;
        }
      },
      *input);
  return safe ? nullptr : input.get();
}

bool ConsumesAllBindings(std::span<const Ancestor> above, std::span<const ColumnId> values = {}) {
  return std::ranges::all_of(above, [values](const Ancestor& step) {
    const auto& ancestor = *step.node;
    // A filter on the scalar itself must read its value before it can discard any row.
    if (const auto* filter = std::get_if<FilterNode>(&ancestor);
        filter != nullptr && !values.empty()) {
      return std::ranges::all_of(filter->predicates, [values](const Predicate& p) {
        return CanSkipPredicate(p) && p.column && std::ranges::contains(values, p.column->id);
      });
    }
    return std::holds_alternative<ProjectNode>(ancestor) ||
           std::holds_alternative<ComputeNode>(ancestor) ||
           std::holds_alternative<SortNode>(ancestor) ||
           std::holds_alternative<AggregateNode>(ancestor) ||
           std::holds_alternative<GroupAggregateNode>(ancestor) ||
           std::holds_alternative<GroupingSetsNode>(ancestor);
  });
}

// Place the exact binding restriction below the deepest checked derived projection. A
// key must already exist there, and all original correlation conditions must be enforced
// by the restriction before the checked expression can run.
arrow::Result<LogicalNodePtr> GuardDerivedProjection(const Correlation& correlation,
                                                     const LogicalNodePtr& input,
                                                     const LogicalNode* unsafe,
                                                     const Representatives& representatives,
                                                     const Equivalences& equivalences) {
  std::map<ColumnId, BoundColumn> carried;
  for (const auto& [outer, inner] : representatives.of) {
    carried.emplace(outer, equivalences.described.at(inner));
  }
  std::vector<LogicalNodePtr> path;
  LogicalNodePtr node = input;
  while (node != nullptr) {
    path.push_back(node);
    if (const auto* project = std::get_if<ProjectNode>(node.get())) {
      for (auto& [outer, key] : carried) {
        const auto at = std::ranges::find(project->ids, key.id);
        if (at == project->ids.end()) {
          return correlation.Refuse("whose derived projection cannot carry its correlation key");
        }
        key = project->columns.at(Narrow<std::size_t>(at - project->ids.begin()));
      }
      node = project->input;
      continue;
    }
    const auto* compute = std::get_if<ComputeNode>(node.get());
    if (compute == nullptr) {
      break;
    }
    const auto ids = OutputIds(*compute->input);
    if (!std::ranges::all_of(carried, [&](const auto& binding) {
          return std::ranges::contains(ids, binding.second.id);
        })) {
      break;
    }
    if (node.get() == unsafe) {
      ARROW_ASSIGN_OR_RAISE(node, RestrictBindingInput(correlation, compute->input, carried,
                                                       representatives, equivalences));
      for (const auto& layer : path | std::views::reverse) {
        node = WithInputs(*layer, {std::move(node)});
      }
      return node;
    }
    node = compute->input;
  }
  return correlation.Refuse("with a potentially failing derived input");
}

ExprPtr SubstituteBindings(const ExprPtr& expr, const std::map<ColumnId, BoundColumn>& carried) {
  return MapColumns(expr, [&carried](ColumnExpr column) {
    if (const auto found = carried.find(column.id); found != carried.end()) {
      column.id = found->second.id;
    }
    return column;
  });
}

arrow::Result<LogicalNodePtr> FilterBindingRows(const Correlation& correlation, const Spine& spine,
                                                const Representatives& representatives,
                                                const Equivalences& equivalences,
                                                const std::map<ColumnId, BoundColumn>& carried,
                                                LogicalNodePtr right, bool& restricted) {
  for (const Expr* expr : equivalences.kept) {
    if (Traits(correlation.join.kind).single_match && !restricted && !SafeBeforeMembership(*expr)) {
      ARROW_ASSIGN_OR_RAISE(
          right, RestrictBindingInput(correlation, right, carried, representatives, equivalences));
      restricted = true;
    }
    const ColumnId id{std::to_underlying(correlation.last_id) + 1};
    correlation.last_id = id;
    right = Make(
        ComputeNode{.input = std::move(right),
                    .exprs = {SubstituteBindings(std::make_shared<const Expr>(*expr), carried)},
                    .ids = {id},
                    .span = spine.computed->span});
    right = Make(FilterNode{
        .input = std::move(right),
        .predicates = {Predicate{
            .kind = Predicate::Kind::kIsTrue,
            .column = BoundColumn{.id = id, .name = expr->name, .type = LogicalType::kBoolean}}},
        .span = spine.filter->span});
  }
  return right;
}

// A HAVING conjunct as a BOOLEAN expression over the columns the filter read, as the binder builds
// a condition inside an expression: a computed condition is its column, any other predicate reads
// its columns as operands. A folded FALSE reads none, so it reads `any`: false or NULL, it takes
// no CASE branch either way.
ExprPtr ConditionOf(const Predicate& predicate, const BoundColumn& any) {
  if (predicate.kind == Predicate::Kind::kIsTrue && predicate.column.has_value()) {
    return std::make_shared<const Expr>(
        Expr{.node = ColumnExpr{.index = 0, .id = predicate.column->id},
             .type = LogicalType::kBoolean,
             .name = predicate.column->name});
  }
  Predicate local = predicate;
  std::vector<ExprPtr> operands;
  const auto operand = [&operands](BoundColumn& column) {
    operands.push_back(
        std::make_shared<const Expr>(Expr{.node = ColumnExpr{.index = 0, .id = column.id},
                                          .type = column.type,
                                          .name = column.name}));
    column.index = Narrow<int>(operands.size() - 1);
    column.id = kNoColumnId;
  };
  if (!local.column.has_value()) {
    local.column = any;
  }
  operand(*local.column);
  if (local.other.has_value()) {
    operand(*local.other);
  }
  std::string name = local.column->name;
  return std::make_shared<const Expr>(
      Expr{.node = PredicateExpr{.predicate = std::move(local), .operands = std::move(operands)},
           .type = LogicalType::kBoolean,
           .name = std::move(name)});
}

// The name a reader of the call's column gives it: the select list's, else the call as EXPLAIN
// shows one.
std::string NameOfCall(const AggregateCall& call, const Spine& spine) {
  for (const Step& step : spine.steps) {
    if (const auto* project = std::get_if<ProjectNode>(step.node.get())) {
      for (const BoundColumn& column : project->columns) {
        if (column.id == call.id) {
          return column.name;
        }
      }
    }
  }
  if (!call.arg.has_value()) {
    return "count_star()";
  }
  return std::format("{}({}{})", ToString(call.kind),
                     call.kind == AggKind::kCountDistinct ? "DISTINCT " : "", call.arg->name);
}

arrow::Result<Rewritten> ValueJoin(const Correlation& correlation, const Spine& spine,
                                   bool computed_keys = false,
                                   const ExprPtr& outer_condition = nullptr,
                                   Equivalences* enabled = nullptr);

// Repeated keys on one side share their computed column.
ExprPtr ScalarKeyColumn(const ExprPtr& value, ComputeNode& keys, ColumnId& last_id) {
  if (std::holds_alternative<ColumnExpr>(value->node)) {
    return value;
  }
  const auto existing =
      std::ranges::find_if(keys.exprs, [&](const ExprPtr& e) { return SameExpr(*e, *value); });
  const auto index = Narrow<std::size_t>(existing - keys.exprs.begin());
  if (existing == keys.exprs.end()) {
    last_id = ColumnId{std::to_underlying(last_id) + 1};
    keys.exprs.push_back(value);
    keys.ids.push_back(last_id);
  }
  return ColumnValue(
      BoundColumn{.id = keys.ids.at(index), .name = value->name, .type = value->type});
}

// Identify a side by every column it reads. A constant, a mixed expression or a reference
// to a different query block cannot supply an independent key.
ComputeNode* ScalarKeyInput(const Expr& expr, const std::set<ColumnId>& outer_ids,
                            const std::vector<ColumnId>& inner_ids, ComputeNode& outer,
                            ComputeNode& inner) {
  std::vector<ColumnId> reads;
  CollectColumnIds(expr, reads);
  if (!reads.empty()) {
    if (std::ranges::all_of(reads, [&](ColumnId id) { return outer_ids.contains(id); })) {
      return &outer;
    }
    if (std::ranges::all_of(reads,
                            [&](ColumnId id) { return std::ranges::contains(inner_ids, id); })) {
      return &inner;
    }
  }
  return nullptr;
}

// Try a single predicate without changing either input until both operands are safe.
ExprPtr NormalizeScalarKey(const ExprPtr& expr, bool computed_pass, const Correlation& correlation,
                           const std::vector<ColumnId>& inner_ids, ComputeNode& left,
                           ComputeNode& right) {
  if (!IsEquality(*expr)) {
    return expr;
  }
  const auto& ops = std::get<PredicateExpr>(expr->node).operands;
  if (ops.size() != 2) {
    return expr;
  }
  const bool computed = !std::holds_alternative<ColumnExpr>(ops[0]->node) ||
                        !std::holds_alternative<ColumnExpr>(ops[1]->node);
  if (computed != computed_pass || (!computed && ops[0]->type == ops[1]->type)) {
    return expr;
  }
  const auto type = KeyType(ops[0]->type, ops[1]->type);
  if (!type || !IsExactWidening(ops[0]->type, *type) || !IsExactWidening(ops[1]->type, *type)) {
    return expr;
  }
  auto* a = ScalarKeyInput(*ops[0], correlation.outer, inner_ids, left, right);
  auto* b = ScalarKeyInput(*ops[1], correlation.outer, inner_ids, left, right);
  if (a == nullptr || b == nullptr || a == b) {
    return expr;
  }
  const ExprPtr first = WidenTo(RewriteArithmetic(ops[0]), *type);
  const ExprPtr second = WidenTo(RewriteArithmetic(ops[1]), *type);
  if (!CanPrecomputeScalarKey(*first, a->input) || !CanPrecomputeScalarKey(*second, b->input)) {
    return expr;
  }
  Expr changed = *expr;
  auto& keyed = std::get<PredicateExpr>(changed.node);
  const auto replace = [&](std::size_t index, const ExprPtr& value, ComputeNode& keys) {
    if (value == ops[index] && std::holds_alternative<ColumnExpr>(value->node)) {
      return;
    }
    keyed.operands[index] = ScalarKeyColumn(value, keys, correlation.last_id);
    auto& column = index == 0 ? keyed.predicate.column : keyed.predicate.other;
    column = BoundColumn{.index = Narrow<int>(index), .name = value->name, .type = *type};
  };
  replace(0, first, *a);
  replace(1, second, *b);
  return std::make_shared<const Expr>(std::move(changed));
}

// SQ10: normalize an equality of two side-local expressions to columns of its exact common
// type. Once key normalization is exhausted, lift safe outer-only conjuncts and retry substitution.
arrow::Result<Rewritten> NormalizeScalarCorrelation(const Correlation& correlation, Spine spine,
                                                    const arrow::Status& missing_representative,
                                                    Equivalences& equivalences,
                                                    bool computed_keys) {
  const auto inner_ids = OutputIds(*spine.below);
  DependentJoinNode join = correlation.join;
  ComputeNode left{.input = join.left, .span = join.span};
  ComputeNode right{.input = spine.below, .span = join.span};
  ComputeNode predicates = *spine.computed;
  const auto slot = [&spine](const ExprPtr* definition) {
    return Narrow<std::size_t>(definition - spine.computed->exprs.data());
  };
  bool changed_key = false;
  ARROW_ASSIGN_OR_RAISE(const auto conjuncts, ConjunctsOf(correlation, spine));
  // Keep plans that plain mixed-width keys already answer. On a retry only computed
  // operands remain, so their original outer columns cannot get an accidental representative.
  for (const bool computed_pass : {false, true}) {
    for (const ExprPtr* defined : conjuncts) {
      const ExprPtr key =
          NormalizeScalarKey(*defined, computed_pass, correlation, inner_ids, left, right);
      if (key != *defined) {
        predicates.exprs.at(slot(defined)) = key;
        changed_key = true;
      }
    }
    if (changed_key) {
      computed_keys |= computed_pass;
      break;
    }
  }
  FilterNode filter = *spine.filter;
  ExprPtr outer_condition;
  if (!changed_key) {
    // Only retry after the existing substitutions and domain rules have declined. Removing
    // the lifted definitions lets FreeVariables find exactly the keys still needed inside.
    std::vector<ExprPtr> conditions;
    std::set<ColumnId> lifted;
    filter.predicates.clear();
    for (std::size_t i = 0; i < conjuncts.size(); ++i) {
      const ExprPtr& expr = *conjuncts[i];
      if (SidesOf(*expr, correlation.outer).outer_only() &&
          CanEvaluateOnUnmatchedRow(*expr, join.left)) {
        conditions.push_back(expr);
        lifted.insert(predicates.ids.at(slot(conjuncts[i])));
      } else {
        filter.predicates.push_back(spine.filter->predicates[i]);
      }
    }
    if (conditions.empty() || filter.predicates.empty()) {
      return missing_representative;
    }
    ComputeNode kept{.input = predicates.input, .span = predicates.span};
    for (std::size_t i = 0; i < predicates.ids.size(); ++i) {
      if (!lifted.contains(predicates.ids[i])) {
        kept.ids.push_back(predicates.ids[i]);
        kept.exprs.push_back(predicates.exprs[i]);
      }
    }
    predicates = std::move(kept);
    outer_condition = conditions.size() == 1
                          ? conditions.front()
                          : std::make_shared<const Expr>(Expr{
                                .node = BoolExpr{.op = BoolOp::kAnd, .args = std::move(conditions)},
                                .type = LogicalType::kBoolean,
                                .name = "outer WHERE"});
  }
  if (!left.ids.empty()) {
    join.left = Make(std::move(left));
  }
  if (!right.ids.empty()) {
    spine.below = Make(std::move(right));
  }
  predicates.input = spine.below;
  filter.input = Make(std::move(predicates));
  spine.computed = &std::get<ComputeNode>(*filter.input);
  join.right = Make(std::move(filter));
  spine.filter = &std::get<FilterNode>(*join.right);
  for (Step& step : std::ranges::reverse_view(spine.steps)) {
    auto inputs = InputsOf(*step.node);
    inputs.front() = join.right;
    step.node = WithInputs(*step.node, std::move(inputs));
    join.right = step.node;
  }
  const auto outer = OutputIds(*join.left);
  const Correlation widened{.join = join,
                            .mode = Mode::kValue,
                            .outer = {outer.begin(), outer.end()},
                            .last_id = correlation.last_id,
                            .build_side_ratio = correlation.build_side_ratio,
                            .above = correlation.above,
                            .report = correlation.report};
  // Each retry either normalizes an equality or removes the outer conditions in one pass.
  return ValueJoin(widened, spine, computed_keys, outer_condition,
                   outer_condition != nullptr ? &equivalences : nullptr);
}

// Even a NULL select item can have an operand that raises on the empty aggregate values.
bool CanSkipEmptyScalarItem(const Step& step) {
  if (step.role == Role::kMap) {
    if (const auto* compute = std::get_if<ComputeNode>(step.node.get())) {
      return std::ranges::all_of(compute->exprs, [&](const ExprPtr& expr) {
        return CanEvaluateOnUnmatchedRow(*expr, compute->input);
      });
    }
  }
  if (step.role == Role::kHaving) {
    return std::ranges::all_of(std::get<FilterNode>(*step.node).predicates, CanSkipPredicate);
  }
  return step.role != Role::kJoin;
}

// Value mode's rewrite (ADR 0023's Path 2; the formalization's Lemmas 4.14 and 4.2): each outer
// column is replaced by its representative, so the correlated conjuncts read inner columns only and
// stay below the aggregation; an equality that became c = c is dropped. The static aggregate groups
// by the representatives, and the one-row dependent join becomes a join with one key per outer
// column the subquery reads. Decision 20's proof picks the join back (decision 21): INNER where the
// comparison over the value rejects its empty value, the maps rebuilt above the group; LEFT
// otherwise, the maps rebuilt above the join so that an outer row with no group computes them over
// the empty values, and a count read there through COALESCE(c, 0).
//
//   Filter o.v <= s                      Filter o.v <= s
//     DependentJoin one-row                Join inner  keys o.k = <key of t.k>
//       Scan o                               Scan o
//       Aggregate m := max(t.x)              GroupAggregate keys t.k  m := max(t.x)
//         Filter t.k = o.k                     Scan t
//           Scan t
//
//   Filter n = 0                         Filter n = 0
//     DependentJoin one-row                Compute n := COALESCE(n', 0)
//       Scan o                               Join LEFT build=right  keys o.k = <key of t.k>
//       Aggregate n := count(*)                Scan o
//         Filter t.k = o.k                     GroupAggregate keys t.k  n' := count(*)
//           Scan t                               Scan t
arrow::Result<Rewritten> ValueJoin(const Correlation& correlation, const Spine& spine,
                                   bool computed_keys, const ExprPtr& outer_condition,
                                   Equivalences* enabled) {
  const DependentJoinNode& join = correlation.join;
  ColumnId& last_id = correlation.last_id;
  std::map<ColumnId, EmptyValue> values;
  ARROW_RETURN_NOT_OK(CheckStatic(correlation, spine, values));
  ARROW_ASSIGN_OR_RAISE(Equivalences equivalences, EquivalencesOf(correlation, spine));
  LogicalNodePtr right = spine.below;
  const bool try_domain = correlation.mode == Mode::kValue && outer_condition == nullptr;
  auto resolved =
      RepresentativesOf(correlation, spine, equivalences, try_domain ? &right : nullptr, enabled);
  if (!resolved.ok() && try_domain) {
    return NormalizeScalarCorrelation(correlation, spine, resolved.status(), equivalences,
                                      computed_keys);
  }
  ARROW_ASSIGN_OR_RAISE(const Representatives representatives, std::move(resolved));
  const bool has_domain = right != spine.below;
  // Decision 20's proof, over the values the subquery appends: INNER only where the reader of each
  // value rejects its empty value, LEFT otherwise (decision 21).
  std::map<ColumnId, EmptyValue> appended;
  for (const ColumnId id : OutputIds(*join.right)) {
    const auto it = values.find(id);
    appended.insert_or_assign(id, it == values.end() ? EmptyValue() : it->second);
  }
  // An empty outer relation needs no padding. Build that side so the grouped input never opens.
  const bool guarded = outer_condition != nullptr;
  const bool empty_outer = (computed_keys || guarded) && ReadsEmptyScan(join.left);
  JoinBack join_back = empty_outer ? JoinBack::kInner : ProveJoinBack(correlation.above, appended);
  if (!empty_outer && guarded && join_back == JoinBack::kInner &&
      !std::ranges::all_of(spine.steps, CanSkipEmptyScalarItem)) {
    join_back = JoinBack::kLeft;
  }
  // Grouping can visit keys that no outer row reads. A checked aggregate requires an
  // exact restriction to enabled keys before its argument or state is evaluated.
  bool needs_enabled_keys = false;
  if (computed_keys || guarded) {
    for (const Step& step : spine.steps) {
      const auto* compute = std::get_if<ComputeNode>(step.node.get());
      const auto* aggregate = std::get_if<AggregateNode>(step.node.get());
      const bool checked_input =
          compute != nullptr &&
          (step.role == Role::kArguments || (!guarded && join_back == JoinBack::kInner)) &&
          std::ranges::any_of(compute->exprs, [&](const ExprPtr& expr) {
            return guarded ? !CanEvaluateOnUnmatchedRow(*expr, compute->input)
                           : !CanPrecomputeScalarKey(*expr, compute->input);
          });
      if (checked_input ||
          (aggregate != nullptr && std::ranges::any_of(aggregate->aggregates, CanFailOnGroup))) {
        if (!guarded) {
          return correlation.Refuse(
              "with a computed key and a potentially failing aggregate expression");
        }
        needs_enabled_keys = true;
      }
    }
    if (std::ranges::any_of(equivalences.kept, [&](const Expr* expr) {
          return guarded ? !CanEvaluateOnUnmatchedRow(*expr, right) : !SafeBeforeMembership(*expr);
        })) {
      return correlation.Refuse("with a potentially failing scalar correlation predicate");
    }
  }
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
  // Under a HAVING the subquery's value is CASE WHEN having THEN item END, which needs a column of
  // its own wherever the value the join appends is defined below the HAVING -- by the aggregate, as
  // `SELECT COUNT(*) ... HAVING ...` appends its call, or passed through the select list. Such an
  // id is renamed: whatever defined it defines a fresh one, which everything below reads, and the
  // id the join appends becomes the CASE over the fresh one, above the HAVING.
  const bool having =
      std::ranges::any_of(spine.steps, [](const Step& step) { return step.role == Role::kHaving; });
  std::map<ColumnId, ColumnId> renamed;
  std::map<ColumnId, std::pair<LogicalType, std::string>> typed;  // each renamed id's type, name
  if (const std::optional<std::set<ColumnId>> above = IdsAboveHaving(spine); above.has_value()) {
    for (const AggregateCall& call : aggregate->aggregates) {
      typed.insert_or_assign(call.id, std::pair{call.type, NameOfCall(call, spine)});
    }
    for (const Step& step : spine.steps) {
      if (const auto* compute = std::get_if<ComputeNode>(step.node.get());
          compute != nullptr && step.role == Role::kMap) {
        for (std::size_t i = 0; i < compute->ids.size(); ++i) {
          typed.insert_or_assign(compute->ids[i],
                                 std::pair{compute->exprs[i]->type, compute->exprs[i]->name});
        }
      }
    }
    for (const ColumnId id : OutputIds(*join.right)) {
      if (!above->contains(id) && typed.contains(id)) {
        renamed.emplace(id, ColumnId{std::to_underlying(last_id) + 1});
        last_id = renamed.at(id);
      }
    }
  }
  const auto rename = [&renamed](ColumnId id) {
    const auto it = renamed.find(id);
    return it == renamed.end() ? id : it->second;
  };
  for (AggregateCall& call : grouped.aggregates) {
    call.id = rename(call.id);
  }
  // An absent group needs its empty aggregate value. A disabled binding needs that same
  // value even if another outer row with the same key enabled the group.
  std::vector<std::pair<AggregateCall, ColumnId>> empty_defaults;
  if (join_back == JoinBack::kLeft) {
    for (AggregateCall& call : grouped.aggregates) {
      if (const EmptyValue empty = EmptyValueOf(call);
          guarded || (empty.has_value() && !IsNull(empty))) {
        empty_defaults.emplace_back(call, ColumnId{std::to_underlying(last_id) + 1});
        last_id = empty_defaults.back().second;
        call.id = last_id;
      }
    }
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
  bool restricted = has_domain;
  if (const auto* unsafe =
          (computed_keys || guarded) ? FirstUnsafeInnerNode(spine.below, guarded) : nullptr) {
    if (guarded || empty_outer || has_domain || !equivalences.kept.empty() ||
        !ConsumesAllBindings(correlation.above)) {
      return correlation.Refuse("with a potentially failing derived input");
    }
    ARROW_ASSIGN_OR_RAISE(
        right, GuardDerivedProjection(correlation, right, unsafe, representatives, equivalences));
    restricted = true;
  }
  const bool prefilter = !needs_enabled_keys && guarded && join_back == JoinBack::kLeft &&
                         RestrictSelectiveBindings(join.left, *outer_condition, right, group);
  if (needs_enabled_keys || prefilter) {
    if (needs_enabled_keys && !ConsumesAllBindings(correlation.above, OutputIds(*join.right))) {
      return correlation.Refuse("whose checked aggregate needs every enabled binding");
    }
    std::map<ColumnId, BoundColumn> carried;
    for (const auto& [outer, inner] : equivalent) {
      carried.emplace(outer, described.at(inner));
    }
    ARROW_ASSIGN_OR_RAISE(right, RestrictBindingInput(correlation, right, carried, representatives,
                                                      equivalences, outer_condition, prefilter));
    restricted |= needs_enabled_keys;
  }
  if (Traits(join.kind).single_match &&
      std::ranges::any_of(equivalences.kept,
                          [](const Expr* expr) { return !SafeBeforeMembership(*expr); })) {
    std::map<ColumnId, BoundColumn> carried;
    for (const auto& [outer, inner] : equivalent) {
      carried.emplace(outer, described.at(inner));
    }
    ARROW_ASSIGN_OR_RAISE(right, FilterBindingRows(correlation, spine, representatives,
                                                   equivalences, carried, right, restricted));
  } else if (!equivalences.kept.empty()) {
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
  // The steps above the static aggregate, bottom-up, where they are rebuilt rather than re-made
  // over the group: above a LEFT join back, and wherever a HAVING is a CASE over them.
  const bool rebuilt =
      guarded || join_back == JoinBack::kLeft || having ||
      std::ranges::any_of(spine.steps, [](const Step& step) { return step.role == Role::kJoin; });
  std::vector<const Step*> maps;
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
        node.input = (restricted || prefilter)
                         ? std::move(right)
                         : RestrictToTheOuterKeys(std::move(right), join, equivalent,
                                                  representatives.null_equal, described, join_back,
                                                  last_id, correlation.build_side_ratio);
        right = Make(std::move(node));
        break;
      }
      case Role::kHaving:
      case Role::kJoin:
        ANTB1_CHECK(rebuilt);
        maps.push_back(&step);
        break;
      case Role::kMap: {
        if (rebuilt) {
          maps.push_back(&step);
          break;
        }
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
      case Role::kGroup:
      case Role::kSets:
      case Role::kLimit:
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
  // The join's estimate is its left input's rows: the keys above are exactly the aggregate's group
  // keys, which a group by makes unique, so each outer row matches at most one group and the join
  // cannot multiply its left input (nor, LEFT, drop one). That is an upper bound rather than a
  // count of the groups, which nothing estimates -- but leaving it unknown left every join above
  // this rewrite reading its own left input as unknown, which is what kept a lopsided join building
  // on the wrong side (ADR 0026).
  // The steps above the static aggregate over `top`, bottom-up: a Compute as it was, a HAVING as a
  // Compute of its condition, which the maps above it then read as CASE WHEN condition THEN value
  // END, and the select list's Project as a Compute that defines its ids, since the join reads
  // columns past it. A constant item keeps its value, COALESCE(SUM(x), 0) its default.
  // A column for a HAVING folded to FALSE to read: NULL or not, a CASE takes it as not true.
  BoundColumn any_key = group_keys.front();
  any_key.id = key_of.at(group.front());
  const auto rename_reads = [&rename, &renamed](const ExprPtr& expr) {
    if (renamed.empty()) {
      return expr;
    }
    return MapColumns(expr, [&rename](ColumnExpr column) {
      column.id = rename(column.id);
      return column;
    });
  };
  const auto rebuild = [&maps, &last_id, &any_key, &rename, &rename_reads, &renamed,
                        &typed](LogicalNodePtr top) {
    std::optional<BoundColumn> guard;  // the HAVING clauses below, once there is one
    std::set<ColumnId> defined;        // the renamed ids the select list defines
    const auto apply_having = [&guard](ExprPtr expr) -> ExprPtr {
      if (!guard.has_value()) {
        return expr;
      }
      const LogicalType type = expr->type;
      std::string name = expr->name;
      return std::make_shared<const Expr>(
          Expr{.node = CaseExpr{.whens = {std::make_shared<const Expr>(
                                    Expr{.node = ColumnExpr{.index = 0, .id = guard->id},
                                         .type = LogicalType::kBoolean,
                                         .name = guard->name})},
                                .thens = {std::move(expr)},
                                .otherwise = nullptr},
               .type = type,
               .name = std::move(name)});
    };
    for (const Step* step : maps) {
      if (step->role == Role::kJoin) {
        // Its right input reads no column below it, so it stands over the new top as it stood.
        top = WithInputs(*step->node, {std::move(top), std::get<JoinNode>(*step->node).right});
        continue;
      }
      if (step->role == Role::kHaving) {
        const auto& filter = std::get<FilterNode>(*step->node);
        std::vector<ExprPtr> conditions;
        if (guard.has_value()) {
          conditions.push_back(
              std::make_shared<const Expr>(Expr{.node = ColumnExpr{.index = 0, .id = guard->id},
                                                .type = LogicalType::kBoolean,
                                                .name = guard->name}));
        }
        for (Predicate predicate : filter.predicates) {
          for (std::optional<BoundColumn>* column : {&predicate.column, &predicate.other}) {
            if (column->has_value()) {
              (*column)->id = rename((*column)->id);
            }
          }
          conditions.push_back(ConditionOf(predicate, any_key));
        }
        Expr condition =
            conditions.size() == 1
                ? *conditions.front()
                : Expr{.node = BoolExpr{.op = BoolOp::kAnd, .args = std::move(conditions)},
                       .type = LogicalType::kBoolean,
                       .name = ""};
        condition.name = "HAVING";  // EXPLAIN's name for the condition's column
        guard = BoundColumn{.id = ColumnId{std::to_underlying(last_id) + 1},
                            .name = condition.name,
                            .type = LogicalType::kBoolean};
        last_id = guard->id;
        top = Make(ComputeNode{.input = std::move(top),
                               .exprs = {std::make_shared<const Expr>(std::move(condition))},
                               .ids = {guard->id},
                               .span = filter.span});
        continue;
      }
      if (const auto* compute = std::get_if<ComputeNode>(step->node.get())) {
        ComputeNode out = *compute;
        out.input = std::move(top);
        for (ExprPtr& expr : out.exprs) {
          expr = apply_having(rename_reads(expr));
        }
        for (ColumnId& id : out.ids) {
          id = rename(id);
        }
        top = Make(std::move(out));
        continue;
      }
      const auto& project = std::get<ProjectNode>(*step->node);
      ComputeNode items{.input = std::move(top), .exprs = {}, .ids = {}, .span = project.span};
      for (std::size_t i = 0; i < project.ids.size() && i < project.columns.size(); ++i) {
        const std::optional<Constant>* constant =
            i < project.constants.size() && project.constants[i].has_value() ? &project.constants[i]
                                                                             : nullptr;
        const BoundColumn& column = project.columns[i];
        if (constant == nullptr && rename(column.id) == project.ids[i]) {
          continue;  // passed through, and no HAVING guards it: defined below already
        }
        items.exprs.push_back(apply_having(std::make_shared<const Expr>(
            constant != nullptr
                ? Expr{.node = ConstantExpr{.value = **constant},
                       .type = (*constant)->type,
                       .name = column.name.empty() ? ToString(**constant) : column.name}
                : Expr{.node = ColumnExpr{.index = 0, .id = rename(column.id)},
                       .type = column.type,
                       .name = column.name})));
        items.ids.push_back(project.ids[i]);
        defined.insert(project.ids[i]);
      }
      top = items.exprs.empty() ? std::move(items.input) : Make(std::move(items));
    }
    // A value the join appends that no select list defines: the CASE over its fresh id, over the
    // last HAVING.
    ComputeNode guarded_values{.input = std::move(top), .exprs = {}, .ids = {}, .span = {}};
    for (const auto& [id, fresh] : renamed) {
      if (defined.contains(id)) {
        continue;
      }
      const auto& [type, name] = typed.at(id);
      guarded_values.exprs.push_back(apply_having(std::make_shared<const Expr>(
          Expr{.node = ColumnExpr{.index = 0, .id = fresh}, .type = type, .name = name})));
      guarded_values.ids.push_back(id);
    }
    return guarded_values.exprs.empty() ? std::move(guarded_values.input)
                                        : Make(std::move(guarded_values));
  };

  const std::optional<int64_t> left_rows = EstimateRows(*join.left);
  if (join_back == JoinBack::kInner) {
    if (rebuilt && !guarded) {
      // Over the group: a HAVING's CASE reads it as it is, and a one-row join moves up with the
      // maps, which a Project's single input cannot carry.
      right = rebuild(std::move(right));
    }
    // An inner join, so either side may build: the aggregate it puts on the right is often far
    // larger than the outer rows that probe it (ADR 0026).
    const std::optional<int64_t> right_rows = EstimateRows(*right);
    LogicalNodePtr out = Make(JoinNode{
        .kind = JoinKind::kInner,
        .left = guarded ? FilterScalarBindings(join.left, outer_condition, last_id, join.span)
                        : join.left,
        .right = right,
        .keys = std::move(keys),
        .residual = {},
        .build = empty_outer ? BuildSide::kLeft
                             : ChooseBuildSide(left_rows, right_rows, correlation.build_side_ratio),
        .estimate = left_rows,
        .span = join.span});
    if (guarded) {
      out = rebuild(std::move(out));
    }
    return Rewritten{.node = std::move(out), .right = std::move(right)};
  }
  // LEFT builds its right input: it pads that side, which only a probe of the left one can tell
  // (JoinKindTraits: no may_build_left).
  LogicalNodePtr top = Make(JoinNode{.kind = JoinKind::kLeft,
                                     .left = join.left,
                                     .right = right,
                                     .keys = std::move(keys),
                                     .residual = {},
                                     .build = BuildSide::kRight,
                                     .estimate = left_rows,
                                     .span = join.span});
  if (!empty_defaults.empty()) {
    ComputeNode defaults{.input = std::move(top), .exprs = {}, .ids = {}, .span = aggregate->span};
    for (const auto& [call, fresh] : empty_defaults) {
      const Constant empty =
          EmptyValueOf(call).value_or(Constant{.type = call.type, .value = std::monostate{}});
      const ExprPtr fallback = std::make_shared<const Expr>(
          Expr{.node = ConstantExpr{.value = empty}, .type = call.type, .name = ToString(empty)});
      ExprPtr value =
          ColumnValue(BoundColumn{.id = fresh, .name = NameOfCall(call, spine), .type = call.type});
      if (!std::holds_alternative<std::monostate>(empty.value)) {
        value = std::make_shared<const Expr>(
            Expr{.node = CaseExpr{.whens = {nullptr}, .thens = {value}, .otherwise = fallback},
                 .type = call.type,
                 .name = std::format("COALESCE({}, {})", value->name, fallback->name)});
      }
      if (guarded) {
        value = std::make_shared<const Expr>(Expr{
            .node = CaseExpr{.whens = {outer_condition}, .thens = {value}, .otherwise = fallback},
            .type = call.type,
            .name = std::format("CASE WHEN {} THEN {} ELSE {} END", outer_condition->name,
                                value->name, fallback->name)});
      }
      defaults.exprs.push_back(std::move(value));
      defaults.ids.push_back(call.id);
    }
    top = Make(std::move(defaults));
  }
  return Rewritten{.node = rebuild(std::move(top)), .right = std::move(right)};
}

// Apply an existence value to the outer rows. A mark is two-valued; semi/anti consumers filter
// that same value. In particular, a static aggregate's absent HAVING row is FALSE, never NULL.
LogicalNodePtr ExistenceValue(const Correlation& correlation, LogicalNodePtr input, ExprPtr value) {
  const DependentJoinNode& join = correlation.join;
  if (join.kind == JoinKind::kAnti) {
    value = std::make_shared<const Expr>(
        Expr{.node = BoolExpr{.op = BoolOp::kNot, .args = {std::move(value)}},
             .type = LogicalType::kBoolean,
             .name = "NOT EXISTS"});
  }
  const ColumnId id =
      join.mark != kNoColumnId ? join.mark : ColumnId{std::to_underlying(correlation.last_id) + 1};
  if (join.mark == kNoColumnId) {
    correlation.last_id = id;
  }
  LogicalNodePtr out = Make(ComputeNode{
      .input = std::move(input), .exprs = {std::move(value)}, .ids = {id}, .span = join.span});
  if (join.mark == kNoColumnId) {
    out = Make(FilterNode{
        .input = std::move(out),
        .predicates = {Predicate{
            .kind = Predicate::Kind::kIsTrue,
            .column = BoundColumn{.id = id, .name = "EXISTS", .type = LogicalType::kBoolean}}},
        .span = join.span});
  }
  return out;
}

LogicalNodePtr EmptyExistence(const Correlation& correlation) {
  // IS NULL of a non-NULL constant is a non-nullable BOOLEAN FALSE.
  const BoundColumn column{.name = "1", .type = LogicalType::kInteger};
  const ExprPtr one = std::make_shared<const Expr>(Expr{
      .node = ConstantExpr{.value = Constant{.type = LogicalType::kInteger, .value = Int128{1}}},
      .type = LogicalType::kInteger,
      .name = "1"});
  const ExprPtr no = std::make_shared<const Expr>(Expr{
      .node =
          PredicateExpr{.predicate = Predicate{.kind = Predicate::Kind::kIsNull, .column = column},
                        .operands = {one}},
      .type = LogicalType::kBoolean,
      .name = "EXISTS"});
  return ExistenceValue(correlation, correlation.join.left, no);
}

// Normalize the operators above a block's static aggregate. DISTINCT and
// ORDER BY cannot change its one row per binding, including after GROUP BY ().
// A static aggregate has a row even over empty input. Its HAVING's IN/NOT IN must
// therefore be decided after the empty-value join back, as a mark followed by a
// HAVING condition, rather than filtering away an outer row before EXISTS reads it.
LogicalNodePtr StaticBlockInput(const LogicalNodePtr& node, ColumnId* having_mark_id = nullptr) {
  if (std::holds_alternative<AggregateNode>(*node)) {
    return node;
  }
  std::vector<LogicalNodePtr> inputs = InputsOf(*node);
  if (inputs.empty()) {
    return node;
  }
  inputs.front() = StaticBlockInput(inputs.front(), having_mark_id);
  LogicalNodePtr rewritten = WithInputs(*node, std::move(inputs));
  if (const auto* sort = std::get_if<SortNode>(rewritten.get())) {
    return sort->input;
  }
  if (const auto* group = std::get_if<GroupAggregateNode>(rewritten.get());
      group != nullptr && group->aggregates.empty()) {
    return Make(ProjectNode{.input = group->input,
                            .columns = group->keys,
                            .constants = {},
                            .ids = group->key_ids,
                            .span = group->span});
  }
  if (having_mark_id == nullptr) {
    return rewritten;
  }
  ColumnId& last_id = *having_mark_id;
  const auto* join = std::get_if<JoinNode>(rewritten.get());
  if (join == nullptr || !Traits(join->kind).filters_left || !FreeVariables(*join->right).empty()) {
    return rewritten;
  }
  JoinNode marked = *join;
  marked.kind = join->kind == JoinKind::kNullAwareAnti ? JoinKind::kNullAwareMark : JoinKind::kMark;
  marked.mark = ColumnId{std::to_underlying(last_id) + 1};
  last_id = marked.mark;
  marked.build = BuildSide::kRight;
  BoundColumn condition{.id = marked.mark, .name = "HAVING", .type = LogicalType::kBoolean};
  LogicalNodePtr top = Make(std::move(marked));
  if (!Traits(join->kind).drops_unmatched_left) {
    const ExprPtr mark = std::make_shared<const Expr>(Expr{
        .node = ColumnExpr{.id = condition.id}, .type = condition.type, .name = condition.name});
    condition.id = ColumnId{std::to_underlying(last_id) + 1};
    last_id = condition.id;
    top = Make(ComputeNode{.input = std::move(top),
                           .exprs = {std::make_shared<const Expr>(
                               Expr{.node = BoolExpr{.op = BoolOp::kNot, .args = {mark}},
                                    .type = condition.type,
                                    .name = condition.name})},
                           .ids = {condition.id},
                           .span = join->span});
  }
  return Make(
      FilterNode{.input = std::move(top),
                 .predicates = {Predicate{.kind = Predicate::Kind::kIsTrue, .column = condition}},
                 .span = join->span});
}

// SQ8b's static aggregate uses the existing empty-value rule. Append a constant 1 instead of
// reading an aggregate result: HAVING changes it to NULL exactly when it rejects the aggregate's
// row. A NULL aggregate value itself says nothing about whether that row exists.
arrow::Result<Rewritten> StaticExistence(const Correlation& correlation,
                                         const LogicalNodePtr& input) {
  ColumnId& last_id = correlation.last_id;
  const ColumnId present{std::to_underlying(last_id) + 1};
  last_id = present;
  const BoundColumn column{.id = present, .name = "exists_row", .type = LogicalType::kInteger};
  DependentJoinNode scalar = correlation.join;
  scalar.kind = JoinKind::kOneRow;
  scalar.mark = kNoColumnId;
  scalar.membership.reset();
  std::vector<BoundColumn> columns;
  std::vector<std::optional<Constant>> constants;
  std::vector<ColumnId> ids;
  if (correlation.join.membership.has_value()) {
    columns.push_back(correlation.join.membership->right);
    constants.emplace_back(std::nullopt);
    ids.push_back(columns.back().id);
  }
  columns.push_back(BoundColumn{.index = -1, .name = column.name, .type = column.type});
  constants.emplace_back(Constant{.type = column.type, .value = Int128{1}});
  ids.push_back(present);
  scalar.right = Make(ProjectNode{.input = StaticBlockInput(input, &last_id),
                                  .columns = std::move(columns),
                                  .constants = std::move(constants),
                                  .ids = std::move(ids),
                                  .span = scalar.span});
  LogicalNodePtr declined;
  const std::optional<Spine> spine =
      Descend(Mode::kValue, scalar.right, &declined, correlation.outer, true);
  if (!spine.has_value()) {
    return correlation.RefuseShape();
  }
  const Correlation value{.join = scalar,
                          .mode = Mode::kLift,
                          .outer = correlation.outer,
                          .last_id = last_id,
                          .build_side_ratio = correlation.build_side_ratio,
                          .above = {},
                          .report = correlation.report};
  Spine bounded = *spine;
  const bool risky_arguments = std::ranges::any_of(spine->steps, [](const Step& step) {
    const auto* compute = std::get_if<ComputeNode>(step.node.get());
    return step.role == Role::kArguments && compute != nullptr &&
           std::ranges::any_of(compute->exprs,
                               [](const ExprPtr& expr) { return !SafeBeforeMembership(*expr); });
  });
  if (correlation.join.membership.has_value() && risky_arguments) {
    ARROW_ASSIGN_OR_RAISE(Equivalences equivalences, EquivalencesOf(value, bounded));
    ARROW_ASSIGN_OR_RAISE(const Representatives representatives,
                          RepresentativesOf(value, bounded, equivalences));
    std::map<ColumnId, BoundColumn> carried;
    for (const auto& [outer, inner] : representatives.of) {
      carried.emplace(outer, equivalences.described.at(inner));
    }
    ARROW_ASSIGN_OR_RAISE(bounded.below, RestrictBindingInput(correlation, bounded.below, carried,
                                                              representatives, equivalences));
  }
  ARROW_ASSIGN_OR_RAISE(Rewritten out, ValueJoin(value, bounded));
  const ExprPtr missing = std::make_shared<const Expr>(Expr{
      .node =
          PredicateExpr{
              .predicate = Predicate{.kind = Predicate::Kind::kIsNull, .column = column},
              .operands = {std::make_shared<const Expr>(Expr{
                  .node = ColumnExpr{.id = present}, .type = column.type, .name = column.name})}},
      .type = LogicalType::kBoolean,
      .name = "EXISTS"});
  ExprPtr exists =
      std::make_shared<const Expr>(Expr{.node = BoolExpr{.op = BoolOp::kNot, .args = {missing}},
                                        .type = LogicalType::kBoolean,
                                        .name = "EXISTS"});
  if (correlation.join.membership.has_value()) {
    exists = std::make_shared<const Expr>(
        Expr{.node = BoolExpr{.op = BoolOp::kAnd,
                              .args = {std::move(exists),
                                       MembershipMatch(*correlation.join.membership,
                                                       correlation.join.kind == JoinKind::kAnti)}},
             .type = LogicalType::kBoolean,
             .name = "IN row match"});
  }
  out.node = ExistenceValue(correlation, std::move(out.node), exists);
  return out;
}

// A grouped EXISTS (or an OFFSET) keeps its rows partitioned by the representatives, through
// every Project and aggregation. The correlation filter is substituted below them, so HAVING
// and DISTINCT operate on the same rows as a separately evaluated block for each outer binding.
arrow::Result<Rewritten> PartitionedBlock(const Correlation& correlation, const Spine& spine) {
  ColumnId& last_id = correlation.last_id;
  const auto fresh = [&last_id] {
    last_id = ColumnId{std::to_underlying(last_id) + 1};
    return last_id;
  };
  ARROW_ASSIGN_OR_RAISE(Equivalences equivalences, EquivalencesOf(correlation, spine));
  const bool single = Traits(correlation.join.kind).single_match;
  LogicalNodePtr right = spine.below;
  ARROW_ASSIGN_OR_RAISE(
      const Representatives representatives,
      RepresentativesOf(correlation, spine, equivalences, single ? &right : nullptr));
  bool restricted = right != spine.below;   // ScalarDomain already selected the exact bindings.
  std::map<ColumnId, BoundColumn> carried;  // outer id -> its representative at this step
  for (const auto& [outer, inner] : representatives.of) {
    const auto found = equivalences.described.find(inner);
    if (found == equivalences.described.end()) {
      return correlation.Refuse("whose correlated key the pass cannot name");
    }
    carried.emplace(outer, found->second);
  }
  ARROW_ASSIGN_OR_RAISE(right, FilterBindingRows(correlation, spine, representatives, equivalences,
                                                 carried, right, restricted));
  const auto group_keys = [&](auto& node) {
    for (auto& [outer, column] : carried) {
      const auto at = std::ranges::find(node.keys, column.id, &BoundColumn::id);
      if (at == node.keys.end()) {
        node.keys.push_back(column);
        node.key_ids.push_back(fresh());
        column.id = node.key_ids.back();
      } else {
        column.id = node.key_ids.at(Narrow<std::size_t>(at - node.keys.begin()));
      }
    }
  };
  // DuckDB retains eager inner-map evaluation for NOT IN over DISTINCT alone. Aggregated
  // sets and IN need the actual bindings; adding DISTINCT above an aggregate changes neither.
  const bool aggregate_set = std::ranges::any_of(spine.steps, [](const Step& step) {
    if (const auto* group = std::get_if<GroupAggregateNode>(step.node.get())) {
      return !group->aggregates.empty();
    }
    return step.role == Role::kSets;
  });
  bool at_most_one = false;
  const bool needs_bindings =
      single || (correlation.join.membership.has_value() &&
                 (correlation.join.kind != JoinKind::kAnti || aggregate_set));
  for (const Step& step : std::ranges::reverse_view(spine.steps)) {
    switch (step.role) {
      case Role::kDropped:
        break;  // existence, including OFFSET, does not depend on row order
      case Role::kMap:
        if (const auto* project = std::get_if<ProjectNode>(step.node.get())) {
          ProjectNode out = *project;
          out.input = std::move(right);
          for (BoundColumn& column : out.columns) {
            if (const auto found = carried.find(column.id); found != carried.end()) {
              column.id = found->second.id;
            }
          }
          for (const auto& [outer, column] : carried) {
            if (std::ranges::contains(out.ids, column.id)) {
              continue;
            }
            out.columns.push_back(column);
            out.ids.push_back(column.id);
            if (!out.constants.empty()) {
              out.constants.emplace_back(std::nullopt);
            }
          }
          right = Make(std::move(out));
        } else {
          ComputeNode out = std::get<ComputeNode>(*step.node);
          if (needs_bindings && !restricted &&
              std::ranges::any_of(
                  out.exprs, [](const ExprPtr& expr) { return !SafeBeforeMembership(*expr); })) {
            ARROW_ASSIGN_OR_RAISE(right, RestrictBindingInput(correlation, right, carried,
                                                              representatives, equivalences));
            restricted = true;
          }
          out.input = std::move(right);
          for (ExprPtr& expr : out.exprs) {
            expr = SubstituteBindings(expr, carried);
          }
          right = Make(std::move(out));
        }
        break;
      case Role::kHaving:
        right = WithInputs(*step.node, {std::move(right)});
        break;
      case Role::kJoin:
        at_most_one = false;
        right = WithInputs(*step.node, {std::move(right), std::get<JoinNode>(*step.node).right});
        break;
      case Role::kGroup: {
        GroupAggregateNode out = std::get<GroupAggregateNode>(*step.node);
        out.input = std::move(right);
        // Once grouped, each binding has at most one row if its representatives
        // determine every written key. HAVING and projections preserve that bound.
        at_most_one = std::ranges::all_of(out.keys, [&](const BoundColumn& key) {
          return std::ranges::any_of(carried,
                                     [&](const auto& entry) { return entry.second.id == key.id; });
        });
        group_keys(out);
        right = Make(std::move(out));
        break;
      }
      case Role::kSets: {
        at_most_one = false;
        GroupingSetsNode out = std::get<GroupingSetsNode>(*step.node);
        // A set may pad the written key. Carry a separate copy that every set groups by, so
        // neither the written key's NULL padding nor GROUPING() changes.
        ComputeNode copies{.input = std::move(right), .exprs = {}, .ids = {}, .span = out.span};
        std::map<ColumnId, ColumnId> copied;
        for (auto& [outer, column] : carried) {
          auto [at, inserted] = copied.try_emplace(column.id, kNoColumnId);
          if (inserted) {
            at->second = fresh();
            copies.exprs.push_back(std::make_shared<const Expr>(Expr{
                .node = ColumnExpr{.id = column.id}, .type = column.type, .name = column.name}));
            copies.ids.push_back(at->second);
          }
          column.id = at->second;
        }
        out.input = Make(std::move(copies));
        const std::size_t first = out.keys.size();
        group_keys(out);
        for (auto& set : out.sets) {
          for (std::size_t i = first; i < out.keys.size(); ++i) {
            set.push_back(i);
          }
        }
        right = Make(std::move(out));
        break;
      }
      case Role::kLimit: {
        const auto& limit = std::get<LimitNode>(*step.node);
        if (limit.offset == 0) {
          break;
        }
        const BoundColumn count{
            .id = fresh(), .name = "exists_count", .type = LogicalType::kBigInt};
        GroupAggregateNode out{.input = std::move(right),
                               .keys = {},
                               .key_ids = {},
                               .aggregates = {AggregateCall{.kind = AggKind::kCountStar,
                                                            .arg = std::nullopt,
                                                            .type = count.type,
                                                            .id = count.id,
                                                            .span = limit.span}},
                               .span = limit.span};
        group_keys(out);
        right = Make(std::move(out));
        right = Make(FilterNode{
            .input = std::move(right),
            .predicates = {Predicate{
                .kind = Predicate::Kind::kCompare,
                .column = count,
                .op = CompareOp::kGt,
                .constant = Constant{.type = LogicalType::kBigInt, .value = Int128{limit.offset}}}},
            .span = limit.span});
        break;
      }
      case Role::kStatic:
      case Role::kArguments:
        return correlation.RefuseShape();
    }
  }
  std::vector<JoinKey> keys;
  keys.reserve(carried.size());
  for (const auto& [outer, inner] : carried) {
    keys.push_back(JoinKey{.left = equivalences.described.at(outer),
                           .right = inner,
                           .null_equal = representatives.null_equal.contains(outer)});
  }
  return MembershipNode(
      correlation, JoinNode{.kind = single && at_most_one ? JoinKind::kLeft : correlation.join.kind,
                            .left = correlation.join.left,
                            .right = std::move(right),
                            .keys = std::move(keys),
                            .residual = {},
                            .mark = correlation.join.mark,
                            .build = BuildSide::kRight,
                            .estimate = EstimateRows(*correlation.join.left),
                            .span = correlation.join.span});
}

// GROUP BY () is the single empty grouping set, hence a static aggregate. Mixed
// grouping sets that include () still need SQ16's empty-binding rows.
LogicalNodePtr StaticGrouping(const LogicalNodePtr& node) {
  if (const auto* sets = std::get_if<GroupingSetsNode>(node.get());
      sets != nullptr && sets->sets.size() == 1 && sets->sets.front().empty() &&
      sets->keys.empty() && sets->grouping.empty()) {
    return Make(
        AggregateNode{.input = sets->input, .aggregates = sets->aggregates, .span = sets->span});
  }
  if (std::holds_alternative<AggregateNode>(*node)) {
    return node;
  }
  std::vector<LogicalNodePtr> inputs = InputsOf(*node);
  if (inputs.empty()) {
    return node;
  }
  const LogicalNodePtr input = StaticGrouping(inputs.front());
  if (input == inputs.front()) {
    return node;
  }
  inputs.front() = input;
  return WithInputs(*node, std::move(inputs));
}

arrow::Result<Rewritten> ExistsJoin(const Correlation& correlation) {
  LogicalNodePtr input = StaticGrouping(correlation.join.right);
  const auto* limit = std::get_if<LimitNode>(input.get());
  const int64_t offset = limit == nullptr ? 0 : limit->offset;
  if (limit != nullptr && limit->limit == 0) {
    return Rewritten{.node = EmptyExistence(correlation), .right = nullptr};
  }
  // A positive LIMIT without OFFSET is transparent even with residual correlations that have
  // no representative; retain the existing candidate-pair algorithm for that plain shape.
  if (limit != nullptr && limit->offset == 0) {
    input = limit->input;
  }
  LogicalNodePtr declined;
  if (const auto plain = Descend(Mode::kLift, input, &declined, correlation.outer)) {
    return LiftJoin(correlation, *plain);
  }
  const auto spine = Descend(Mode::kPartitioned, input, &declined, correlation.outer);
  if (!spine.has_value()) {
    return correlation.RefuseShape();
  }
  if (std::ranges::any_of(spine->steps,
                          [](const Step& step) { return step.role == Role::kStatic; })) {
    if (offset > 0) {
      return Rewritten{.node = EmptyExistence(correlation), .right = nullptr};
    }
    return StaticExistence(correlation, input);
  }
  return PartitionedBlock(correlation, *spine);
}

arrow::Result<Rewritten> MembershipJoin(const Correlation& correlation) {
  const LogicalNodePtr input = StaticGrouping(correlation.join.right);
  LogicalNodePtr declined;
  if (const auto plain = Descend(Mode::kLift, input, &declined, correlation.outer)) {
    return LiftJoin(correlation, *plain);
  }
  const auto spine = Descend(Mode::kPartitioned, input, &declined, correlation.outer);
  if (!spine.has_value()) {
    return correlation.RefuseShape();
  }
  for (const Step& step : spine->steps) {
    if (step.role == Role::kLimit) {
      if (std::get<LimitNode>(*step.node).limit == 0) {
        return Rewritten{.node = EmptyExistence(correlation), .right = nullptr};
      }
      return correlation.Refuse("with LIMIT or OFFSET");
    }
  }
  if (std::ranges::any_of(spine->steps,
                          [](const Step& step) { return step.role == Role::kStatic; })) {
    return StaticExistence(correlation, input);
  }
  return PartitionedBlock(correlation, *spine);
}

// Same-type keys in this block's own correlated filter may replace an enclosing probe below
// that filter. A nested join's keys or a padded join's ON would not establish that equivalence.
std::vector<KeyParts> MembershipEquivalents(const LogicalNodePtr& right,
                                            const std::set<ColumnId>& outer) {
  LogicalNodePtr declined;
  const std::optional<Spine> spine = Descend(Mode::kPartitioned, right, &declined, outer);
  std::vector<KeyParts> keys;
  if (!spine.has_value()) {
    return keys;
  }
  for (const Predicate& predicate : spine->filter->predicates) {
    const ExprPtr* expr =
        predicate.column.has_value() ? DefinedBy(*spine->computed, predicate.column->id) : nullptr;
    if (expr != nullptr) {
      if (const auto key = AsKey(**expr, outer);
          key.has_value() && key->inner_type == key->outer_type) {
        keys.push_back(*key);
      }
    }
  }
  return keys;
}

// Keep an enclosing probe's mark in this block when a same-type key represents its column;
// grouping and OFFSET then consume the ordinary inner mark. Otherwise compute the three-valued
// mark on the enclosing rows, and let the correlated filter consume it. Moving a semi/anti filter
// instead would incorrectly remove outer rows of NOT EXISTS.
LogicalNodePtr RewriteOuterMembership(const LogicalNodePtr& node, LogicalNodePtr& outer,
                                      std::span<const KeyParts> equivalents) {
  if (std::holds_alternative<WindowNode>(*node)) {
    return node;
  }
  std::vector<LogicalNodePtr> inputs = InputsOf(*node);
  if (inputs.empty() || std::holds_alternative<UnionAllNode>(*node) ||
      std::holds_alternative<SetOperationNode>(*node)) {
    return node;
  }
  if (const auto* join = std::get_if<JoinNode>(node.get());
      join != nullptr && Traits(join->kind).pads_left) {
    return node;
  }
  const LogicalNodePtr rewritten = RewriteOuterMembership(inputs.front(), outer, equivalents);
  LogicalNodePtr current = node;
  if (rewritten != inputs.front()) {
    inputs.front() = rewritten;
    current = WithInputs(*node, std::move(inputs));
  }
  const auto* join = std::get_if<JoinNode>(current.get());
  if (join == nullptr || join->kind != JoinKind::kNullAwareMark || join->keys.size() != 1 ||
      !join->residual.empty() || !FreeVariables(*join->right).empty()) {
    return current;
  }
  const ColumnId probe = join->keys.front().left.id;
  std::vector<ColumnId> available = OutputIds(*outer);
  const std::set<ColumnId> outer_ids(available.begin(), available.end());
  LogicalNodePtr inner = join->left;
  const auto* compute = std::get_if<ComputeNode>(inner.get());
  const ExprPtr* defined = compute == nullptr ? nullptr : DefinedBy(*compute, probe);
  ExprPtr expr;
  if (outer_ids.contains(probe)) {
    expr = ColumnValue(join->keys.front().left);
  } else if (defined != nullptr) {
    expr = *defined;
  }
  if (expr == nullptr || !SidesOf(*expr, outer_ids).outer_only()) {
    return current;
  }
  if (const std::optional<ColumnId> column = WidenedColumn(*expr)) {
    const std::vector<ColumnId> inner_ids =
        OutputIds(*(defined == nullptr ? inner : compute->input));
    const auto equivalent = std::ranges::find_if(equivalents, [&](const KeyParts& key) {
      return key.outer.id == *column &&
             std::ranges::find(inner_ids, key.inner.id) != inner_ids.end();
    });
    if (equivalent != equivalents.end()) {
      JoinNode kept = *join;
      if (defined == nullptr) {
        kept.keys.front().left.id = equivalent->inner.id;
      } else {
        ComputeNode mapped = *compute;
        const auto at = std::ranges::find(mapped.ids, probe);
        mapped.exprs.at(Narrow<std::size_t>(at - mapped.ids.begin())) =
            MapColumns(expr, [&](ColumnExpr read) {
              if (read.id == *column) {
                read.id = equivalent->inner.id;
              }
              return read;
            });
        kept.left = Make(std::move(mapped));
      }
      return Make(std::move(kept));
    }
  }
  // InKeys emits one Compute for a cast or expression probe. Move only the definition read by
  // this mark; other definitions in that layer keep their own input and evaluation place.
  if (!outer_ids.contains(probe)) {
    ComputeNode kept = *compute;
    const auto at = std::ranges::find(kept.ids, probe);
    const auto index = at - kept.ids.begin();
    kept.ids.erase(at);
    kept.exprs.erase(kept.exprs.begin() + index);
    outer = Make(ComputeNode{
        .input = std::move(outer), .exprs = {expr}, .ids = {probe}, .span = compute->span});
    inner = kept.exprs.empty() ? kept.input : Make(std::move(kept));
  }
  JoinNode lifted = *join;
  lifted.left = std::move(outer);
  lifted.estimate = EstimateRows(*lifted.left);
  lifted.needed_above.clear();
  outer = Make(std::move(lifted));
  return inner;
}

// The scalar's one empty grouping set has the static aggregate's empty value (COUNT gives
// zero). A nonempty grouping set or a plain select has no row for a missing binding.
arrow::Result<Rewritten> SingleJoin(const Correlation& correlation) {
  const LogicalNodePtr input = StaticGrouping(correlation.join.right);
  LogicalNodePtr declined;
  const auto spine = Descend(Mode::kPartitioned, input, &declined, correlation.outer);
  if (!spine.has_value()) {
    return correlation.RefuseShape();
  }
  if (std::ranges::any_of(spine->steps,
                          [](const Step& step) { return step.role == Role::kLimit; })) {
    return correlation.Refuse("with LIMIT or OFFSET");
  }
  if (std::ranges::any_of(spine->steps,
                          [](const Step& step) { return step.role == Role::kStatic; })) {
    auto value = correlation;
    value.mode = Mode::kValue;
    const auto normalized = StaticBlockInput(input);
    if (const auto aggregate = Descend(value.mode, normalized, &declined, value.outer)) {
      return ValueJoin(value, *aggregate);
    }
    return correlation.RefuseShape();
  }
  if (PlainScalarMaps(correlation, *spine)) {
    ARROW_ASSIGN_OR_RAISE(auto out, LiftJoin(correlation, *spine));
    const auto& single = std::get<JoinNode>(*out.node);
    // A LIMIT can stop between input batches. A filter or join above the scalar may
    // also move below it. Only check bindings eagerly when every outer row is consumed.
    const bool all_bindings = ConsumesAllBindings(correlation.above);
    if (!single.keys.empty() || EstimateRows(*single.right) == 0 || !all_bindings) {
      return out;
    }
    LogicalNodePtr values = spine->below;
    Equivalences equivalences;
    ARROW_ASSIGN_OR_RAISE(const auto bindings,
                          ScalarDomain(correlation, *spine, equivalences, values, single.right));
    std::vector<JoinKey> keys;
    keys.reserve(bindings.of.size());
    for (const auto& [outer, binding] : bindings.of) {
      keys.push_back(JoinKey{.left = equivalences.described.at(outer),
                             .right = equivalences.described.at(binding),
                             .null_equal = true});
    }
    return Rewritten{.node = Make(JoinNode{.kind = JoinKind::kLeft,
                                           .left = single.left,
                                           .right = values,
                                           .keys = std::move(keys),
                                           .build = BuildSide::kRight,
                                           .estimate = single.estimate,
                                           .span = single.span}),
                     .right = values};
  }
  return PartitionedBlock(correlation, *spine);
}

// A sibling still evaluates its own independent inputs as before (D23). Only operations
// that acquire extra middle bindings need the SQ10 check. Match an unchanged input by identity.
void CollectIndependentInputs(const LogicalNodePtr& node, std::set<const LogicalNode*>& inputs) {
  if (FreeVariables(*node).empty()) {
    inputs.insert(node.get());
    return;
  }
  for (const auto& child : InputsOf(*node)) {
    CollectIndependentInputs(child, inputs);
  }
}

// The dependent join's rule: its mode, the descent, the mode's rewrite, then Lemma 3.1's check.
arrow::Result<Rewritten> UnnestJoin(DependentJoinNode join, ColumnId& last_id,
                                    int64_t build_side_ratio, std::span<const Ancestor> above,
                                    UnnestReport* report) {
  const auto holds_window = [](this const auto& self, const LogicalNodePtr& node) -> bool {
    return std::holds_alternative<WindowNode>(*node) || std::ranges::any_of(InputsOf(*node), self);
  };
  if (holds_window(join.right)) {
    return UnsupportedError("W11g/SQ17: window-containing correlated subplans are not supported",
                            join.span);
  }
  const std::optional<Mode> mode = ModeOf(join.kind);
  if (!mode.has_value()) {
    return UnsupportedError("a correlated subquery is not supported", join.span);
  }
  if ((join.mark != kNoColumnId) != Traits(join.kind).appends_mark) {
    // A binder bug: ResolvePositions would abort the process over it rather than fail the query.
    return arrow::Status::NotImplemented(
        std::format("the unnesting pass met a {} dependent join {} a mark", ToString(join.kind),
                    join.mark == kNoColumnId ? "without" : "with"));
  }
  const std::vector<ColumnId> initial_ids = OutputIds(*join.left);
  const std::vector<KeyParts> equivalents =
      MembershipEquivalents(join.right, std::set<ColumnId>(initial_ids.begin(), initial_ids.end()));
  std::set<const LogicalNode*> sibling_inputs;
  if (join.middle_key == DependentJoinNode::MiddleKey::kSibling) {
    sibling_inputs.insert(join.left.get());
    CollectIndependentInputs(join.right, sibling_inputs);
  }
  join.right = RewriteOuterMembership(join.right, join.left, equivalents);
  const std::vector<ColumnId> outer_ids = OutputIds(*join.left);
  const Correlation correlation{.join = join,
                                .mode = *mode,
                                .outer = std::set<ColumnId>(outer_ids.begin(), outer_ids.end()),
                                .last_id = last_id,
                                .build_side_ratio = build_side_ratio,
                                .above = above,
                                .report = report};
  Rewritten out;
  if (join.membership.has_value()) {
    ARROW_ASSIGN_OR_RAISE(out, MembershipJoin(correlation));
  } else if (Traits(join.kind).single_match) {
    ARROW_ASSIGN_OR_RAISE(out, SingleJoin(correlation));
  } else if (*mode == Mode::kLift) {
    ARROW_ASSIGN_OR_RAISE(out, ExistsJoin(correlation));
  } else {
    LogicalNodePtr declined;
    const std::optional<Spine> spine = Descend(*mode, join.right, &declined, correlation.outer);
    if (!spine.has_value()) {
      if (const auto* filter =
              declined == nullptr ? nullptr : std::get_if<JoinNode>(declined.get());
          filter != nullptr && Traits(filter->kind).filters_left) {
        return correlation.Refuse("whose HAVING holds an IN subquery");
      }
      return correlation.RefuseShape();
    }
    ARROW_ASSIGN_OR_RAISE(out, ValueJoin(correlation, *spine));
  }
  // Lemma 3.1: the rewrite is legal only when the new join's right input has no free variables
  // left, and when what it put above the join reads none the dependent join did not. A bug in a
  // rule is the only way this fails, and it must not reach ResolvePositions, which would abort the
  // process rather than fail the query.
  const std::vector<ColumnId> free_before = FreeVariables(LogicalNode{join});
  const std::vector<ColumnId> free_after = FreeVariables(*out.node);
  if ((out.right != nullptr && !FreeVariables(*out.right).empty()) ||
      !std::ranges::all_of(free_after, [&free_before](ColumnId id) {
        return std::ranges::find(free_before, id) != free_before.end();
      })) {
    return arrow::Status::NotImplemented(
        "the unnesting pass left a free variable in a rewritten join's right input");
  }
  // Nor may the rewrite lose a column the dependent join output, a mark included: the nodes above
  // read them.
  const std::vector<ColumnId> outputs = OutputIds(*out.node);
  for (const ColumnId id : OutputIds(LogicalNode{join})) {
    if (std::ranges::find(outputs, id) == outputs.end()) {
      return arrow::Status::NotImplemented("the unnesting pass lost an output of a rewritten join");
    }
  }
  if (!sibling_inputs.empty() &&
      FirstUnsafeInnerNode(out.node, /*guarded=*/true, sibling_inputs) != nullptr) {
    return correlation.Refuse(
        "with a potentially failing sibling expression under a middle-block key");
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
    if (middle_inputs_.empty()) {
      return WalkNode(node);
    }
    ARROW_ASSIGN_OR_RAISE(LogicalNodePtr result, WalkNode(node));
    if (node != result) {
      for (auto& input : middle_inputs_) {
        if (input == node) {
          input = result;
        }
      }
    }
    return result;
  }

 private:
  arrow::Result<LogicalNodePtr> WalkSubstituted(const Rewritten& rewritten,
                                                const DependentJoinNode& dependent) {
    // Follow the existing middle input through sibling rewrites. Check work on extra middle
    // rows, including eager projections. The final deferred item only runs on matched pairs.
    middle_inputs_.push_back(dependent.left);
    auto result = Walk(rewritten.node);
    LogicalNodePtr middle = std::move(middle_inputs_.back());
    middle_inputs_.pop_back();
    if (result.ok() && FirstUnsafeInnerNode(*result, /*guarded=*/true, {middle.get()},
                                            rewritten.deferred_match.get()) != nullptr) {
      if (dependent.membership.has_value()) {
        return UnsupportedError(
            "membership with a middle-block key and a potentially failing "
            "set expression is not supported",
            dependent.span);
      }
      return UnsupportedError(
          "nested EXISTS with a middle-block key and a potentially failing "
          "inner expression is not supported",
          dependent.span);
    }
    return result;
  }

  arrow::Result<LogicalNodePtr> WalkNode(const LogicalNodePtr& node) {
    if (node == nullptr) {
      return node;
    }
    if (const auto* dependent = std::get_if<DependentJoinNode>(node.get())) {
      ARROW_ASSIGN_OR_RAISE(const Rewritten rewritten,
                            UnnestJoin(*dependent, last_id_, build_side_ratio_, above_, report_));
      if (dependent->middle_key == DependentJoinNode::MiddleKey::kUnsafeConsumer) {
        return UnsupportedError(
            "nested EXISTS with a middle-block key and a potentially failing sibling expression is "
            "not supported",
            dependent->span);
      }
      const bool guarded = dependent->guard_membership ||
                           dependent->middle_key == DependentJoinNode::MiddleKey::kSubstituted;
      ARROW_ASSIGN_OR_RAISE(
          auto result, guarded ? WalkSubstituted(rewritten, *dependent) : Walk(rewritten.node));
      if (rewritten.pending_lookups.has_value()) {
        const auto& join = std::get<JoinNode>(*result);
        if (WorthNestedLookups(join, above_) && !UnsafeToClone(join.right) &&
            FreeVariables(*join.right).empty()) {
          JoinNode keyed = join;
          keyed.residual.clear();  // The sole residual was the provisional NOT IN comparison.
          return MembershipLookups(std::move(keyed), *rewritten.pending_lookups, last_id_);
        }
      }
      return result;
    }
    std::vector<LogicalNodePtr> inputs = InputsOf(*node);
    bool changed = false;
    above_.push_back(Ancestor{.node = node.get(), .input = 0});
    for (std::size_t i = 0; i < inputs.size(); ++i) {
      LogicalNodePtr& input = inputs[i];
      if (input == nullptr) {
        continue;
      }
      above_.back().input = i;  // which input of this node the proof below comes up from
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

  std::vector<LogicalNodePtr> middle_inputs_;
  ColumnId& last_id_;
  int64_t build_side_ratio_;
  UnnestReport* report_;
  std::vector<Ancestor> above_;
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

JoinBack ProveJoinBack(std::span<const Ancestor> above,
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
  for (const Ancestor& step : std::ranges::reverse_view(above)) {
    const LogicalNode* node = step.node;
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
    if (const auto* filter = std::get_if<FilterNode>(node)) {
      for (const Predicate& predicate : filter->predicates) {
        if ((reads(predicate.column) || reads(predicate.other)) &&
            TruthOver(predicate, value_of(predicate.column), value_of(predicate.other)) ==
                BranchTruth::kNever) {
          return JoinBack::kInner;  // every row whose value is empty fails this conjunct
        }
      }
      continue;  // it only drops rows: each row it keeps meets the nodes above, its columns as they
                 // were
    }
    // A join reached from its left input that pads no left row (inner, LEFT, semi, anti, one-row,
    // mark) decides each left row by that row and its right input alone, and outputs the row's
    // columns as they are. A FULL join, or a join reached from its right input, does not.
    if (const auto* join = std::get_if<JoinNode>(node);
        join != nullptr && step.input == 0 && !Traits(join->kind).pads_left) {
      continue;
    }
    return JoinBack::kLeft;  // a node whose rows depend on other rows, or that hides the value
  }
  return JoinBack::kLeft;  // nothing rejects the value: an INNER join would drop outer rows
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
