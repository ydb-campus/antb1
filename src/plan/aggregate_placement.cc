#include "aggregate_placement.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "antb1/common/int128.h"
#include "antb1/plan/logical_plan.h"

#include "join_order.h"

namespace antb1::plan {
namespace {

using IdSet = std::unordered_set<ColumnId>;

// The ids of a node's output, as a set.
IdSet OutputSet(const LogicalNode& node) {
  const std::vector<ColumnId> ids = OutputIds(node);
  return {ids.begin(), ids.end()};
}

// What sits between a grouped aggregate and the join it reads: the join itself, and the expression
// each Compute on the way appends, so that an aggregate over a computed column can be resolved to
// the columns that computed it.
struct Below {
  LogicalNodePtr node;             // the join, to name in a placement
  const JoinNode* join = nullptr;  // the same node, read
  std::unordered_map<ColumnId, const Expr*> computed;
};

// Descends from a grouped aggregate's input to the join it reads, through Computes only.
std::optional<Below> BelowAggregate(const LogicalNodePtr& input) {
  Below out;
  LogicalNodePtr node = input;
  while (node != nullptr) {
    if (const auto* join = std::get_if<JoinNode>(node.get())) {
      out.node = node;
      out.join = join;
      return out;
    }
    const auto* compute = std::get_if<ComputeNode>(node.get());
    if (compute == nullptr) {
      return std::nullopt;  // a Filter removes rows, a Project moves ids: neither travels yet
    }
    for (std::size_t i = 0; i < compute->ids.size() && i < compute->exprs.size(); ++i) {
      out.computed.emplace(compute->ids[i], compute->exprs[i].get());
    }
    node = compute->input;
  }
  return std::nullopt;
}

// Which side of the join a column belongs to, resolving a computed column to the columns it reads.
// kLeft or kRight when every one of them is on that side; nullopt when the column reads both sides,
// reads nothing the join outputs, or is computed from an expression this cannot resolve.
enum class Side : std::uint8_t { kLeft, kRight };

// The columns of the join's output that `id` reads: itself, or -- where a Compute between the two
// positions defines it -- the columns its expression reads. Empty when nothing is resolvable, which
// a constant expression and a column of a second Compute layer both are.
std::vector<ColumnId> Reads(ColumnId id, const Below& below) {
  std::vector<ColumnId> out;
  if (const auto found = below.computed.find(id); found != below.computed.end()) {
    CollectColumnIds(*found->second, out);
    return out;
  }
  out.push_back(id);
  return out;
}

std::optional<Side> SideOf(ColumnId id, const Below& below, const IdSet& left, const IdSet& right) {
  const std::vector<ColumnId> reads = Reads(id, below);
  if (reads.empty()) {
    return std::nullopt;  // a constant expression names no side
  }
  bool any_left = false;
  bool any_right = false;
  for (const ColumnId read : reads) {
    // A computed column may read another computed column, which this does not follow: it resolves
    // one layer, which is what an aggregate over an arithmetic expression of table columns needs.
    if (left.contains(read)) {
      any_left = true;
    } else if (right.contains(read)) {
      any_right = true;
    } else {
      return std::nullopt;
    }
  }
  if (any_left == any_right) {
    return std::nullopt;  // both, or neither
  }
  return any_left ? Side::kLeft : Side::kRight;
}

// Whether the exchange can split this call into a partial and a final (ADR 0032's condition 2).
// The same classification ADR 0014's two-level aggregation already makes for its merge, and for the
// same reason: a call that cannot be merged out of order cannot be computed in two places either.
bool Decomposable(AggKind kind) {
  switch (kind) {
    case AggKind::kCountStar:
    case AggKind::kCount:
    case AggKind::kSum:
    case AggKind::kMin:
    case AggKind::kMax:
    case AggKind::kAvg:  // as a (SUM, COUNT) pair, divided above the join
      return true;
    case AggKind::kCountDistinct:
      return false;
  }
  return false;
}

// A join whose output holds both inputs' columns and whose keys the exchange can group by.
bool Exchangeable(const JoinNode& join) {
  if (join.keys.empty()) {
    return false;
  }
  // A residual decides which pairs match on something other than the keys, and the pre-grouping
  // destroys exactly what it reads: a partial would aggregate the rows the residual rejects, over
  // columns no longer there to reject them by, and no final step can take that back. ADR 0032's
  // condition 4 asks for an equi-join on the grouped columns and this is the rest of that ask. A
  // residual reading only the side that is NOT aggregated would be safe, since it then filters the
  // other input's rows and the partials it matches are whole; admitting that wants its own test.
  if (!join.residual.empty()) {
    return false;
  }
  switch (join.kind) {
    case JoinKind::kInner:
    case JoinKind::kLeft:
      return true;
    // Refused, and the reason is conservatism rather than illegality. The right input's columns
    // are not output, so no aggregate can read that side -- but a placement on the LEFT side would
    // be sound: every row of a group shares the join key, so a group matches entirely or not at
    // all, and the exchange holds as it does for an inner join. Neither query ADR 0032 measures
    // has this shape, and admitting it would widen what the gate of item G2 has to classify
    // before anything has asked for it.
    case JoinKind::kSemi:
    case JoinKind::kAnti:
    case JoinKind::kNullAwareAnti:
    case JoinKind::kOneRow:  // and this one has no keys to group by
      return false;
  }
  return false;
}

// The gate's thresholds. Deliberately coarse: the quantities they compare are footer estimates,
// and ADR 0032's survey separates its two worthwhile queries from its refusals by an order of
// magnitude on each, not by a few percent.
constexpr int64_t kLargestRows = std::numeric_limits<int64_t>::max();
constexpr int64_t kLeastReduction = 3;     // rows per group, below which the grouping is overhead
constexpr int64_t kLeastKeptPercent = 90;  // of the groups built, surviving the joins above

// The rows a join outputs. The binder keeps an estimate on the node for the joins its order search
// ranked, and **not for a LEFT JOIN**, so this computes one the same way ADR 0022's rule 3 does:
// the inputs multiplied over the larger of the two key domains, and never below the preserved side
// for an outer join. Local to this gate on purpose -- EstimateRows feeds ChooseBuildSide, and
// changing what a join reads as there is ADR 0026's decision and not this item's.
std::optional<int64_t> JoinRows(const JoinNode& join) {
  if (join.estimate.has_value()) {
    return join.estimate;
  }
  const std::optional<int64_t> left = EstimateRows(*join.left);
  const std::optional<int64_t> right = EstimateRows(*join.right);
  if (!left.has_value() || !right.has_value()) {
    return std::nullopt;
  }
  // The largest domain of any key column, over both inputs: two walks of the plan rather than two
  // a key, which is what asking column by column would cost.
  std::vector<ColumnId> on_left;
  std::vector<ColumnId> on_right;
  on_left.reserve(join.keys.size());
  on_right.reserve(join.keys.size());
  for (const JoinKey& key : join.keys) {
    on_left.push_back(key.left.id);
    on_right.push_back(key.right.id);
  }
  int64_t divisor = 1;
  for (const auto& [node, ids] :
       {std::pair(join.left.get(), &on_left), std::pair(join.right.get(), &on_right)}) {
    for (const std::optional<int64_t>& domain : EachDistinctValues(*node, *ids)) {
      if (domain.has_value()) {
        divisor = std::max(divisor, *domain);
      }
    }
  }
  const int64_t rows =
      Int128ToInt64(Int128{*left} * Int128{*right} / divisor).value_or(kLargestRows);
  return join.kind == JoinKind::kLeft ? std::max(rows, *left) : rows;
}

// What the gate makes of a legal placement, from the estimates a plan node can reach.
AggregatePlacement::Verdict Judge(const AggregatePlacement& placement) {
  using Verdict = AggregatePlacement::Verdict;
  if (!placement.rows_now.has_value() || !placement.rows_below.has_value() ||
      !placement.groups.has_value()) {
    return Verdict::kUnknown;
  }
  const int64_t now = *placement.rows_now;
  const int64_t below = *placement.rows_below;
  const int64_t groups = std::max<int64_t>(*placement.groups, 1);
  // Condition 3: the aggregate must not end up reading more rows than it reads today. A filter
  // between the two positions is the usual way that happens, and a reduction ratio hides it.
  if (below > now) {
    return Verdict::kBelowAFilter;
  }
  // Conditions 2 and 5: the pre-grouping has to reduce. A pre-grouping key that is a key of its
  // own side makes one group per row, which is the dominant shape of this schema and the reason
  // most legal placements are not worth taking.
  if (below / groups < kLeastReduction) {
    return Verdict::kNoReduction;
  }
  // Condition 4: the joins above must keep most of the groups, or the pre-aggregation builds them
  // to have them thrown away. `now` is what the join above actually emits, so the share of the
  // aggregated side's rows that survive it stands in for the share of its groups that do.
  if (Int128{now} * 100 < Int128{below} * kLeastKeptPercent) {
    return Verdict::kJoinDiscards;
  }
  return Verdict::kWorth;
}

void Add(std::vector<ColumnId>& out, ColumnId id) {
  if (id != kNoColumnId && std::ranges::find(out, id) == out.end()) {
    out.push_back(id);
  }
}

std::optional<AggregatePlacement> PlacementOf(const LogicalNodePtr& node) {
  const auto* aggregate = std::get_if<GroupAggregateNode>(node.get());
  if (aggregate == nullptr || aggregate->aggregates.empty()) {
    return std::nullopt;  // SELECT DISTINCT: a grouping with no call has nothing to move
  }
  const std::optional<Below> below = BelowAggregate(aggregate->input);
  if (!below.has_value() || !Exchangeable(*below->join)) {
    return std::nullopt;
  }
  const JoinNode& join = *below->join;
  if (join.left == nullptr || join.right == nullptr) {
    return std::nullopt;  // only a plan built by hand reaches this
  }
  const IdSet left = OutputSet(*join.left);
  const IdSet right = OutputSet(*join.right);

  // The side every aggregate reads, and only that side (condition 1). A call naming no column --
  // COUNT(*) -- constrains nothing, so the side comes from the calls that do name one.
  std::optional<Side> aggregated;
  for (const AggregateCall& call : aggregate->aggregates) {
    if (!Decomposable(call.kind)) {
      return std::nullopt;
    }
    if (!call.arg.has_value()) {
      continue;
    }
    const std::optional<Side> side = SideOf(call.arg->id, *below, left, right);
    if (!side.has_value() || (aggregated.has_value() && *aggregated != *side)) {
      return std::nullopt;
    }
    aggregated = side;
  }
  if (!aggregated.has_value()) {
    return std::nullopt;  // every call is COUNT(*): both sides are legal and nothing chooses
  }

  // Condition 3: the aggregated side's join columns, then every grouping column that comes from it.
  const bool left_aggregated = *aggregated == Side::kLeft;
  AggregatePlacement out{.aggregate = node,
                         .join = below->node,
                         .left_is_aggregated = left_aggregated,
                         .pre_grouping = {},
                         .grouping_from_aggregated_side = 0,
                         .needs_identity = join.kind == JoinKind::kLeft && !left_aggregated};
  // A key names a column of its own input by construction (JoinKey), and where the two key types
  // differ the widening is a Compute on that input's branch -- so the widened column is below the
  // join too, and grouping by it is grouping by a function of the column it widened.
  for (const JoinKey& key : join.keys) {
    Add(out.pre_grouping, left_aggregated ? key.left.id : key.right.id);
  }
  if (out.pre_grouping.empty()) {
    return std::nullopt;  // keys the exchange cannot name are keys it cannot group by
  }
  for (const BoundColumn& grouping : aggregate->keys) {
    const std::optional<Side> side = SideOf(grouping.id, *below, left, right);
    if (!side.has_value()) {
      return std::nullopt;  // a grouping column over both inputs separates rows no pre-group can
    }
    if (*side == *aggregated) {
      // What the aggregated side must group by, and not what the grouping set calls it: a column a
      // Compute above the join defines does not exist below it, so the pre-grouping names the
      // columns that computed it. Grouping finer than the outer set keeps the answer, because the
      // final step groups again.
      const std::size_t before = out.pre_grouping.size();
      for (const ColumnId read : Reads(grouping.id, *below)) {
        Add(out.pre_grouping, read);
      }
      if (out.pre_grouping.size() != before) {
        ++out.grouping_from_aggregated_side;
      }
    }
  }
  // The estimates the gate judges on, and then the verdict.
  const LogicalNodePtr& aggregated_side = left_aggregated ? join.left : join.right;
  out.rows_now = JoinRows(join);
  out.rows_below = EstimateRows(*aggregated_side);
  out.groups = DistinctValues(*aggregated_side, out.pre_grouping);
  out.verdict = Judge(out);
  return out;
}

void Walk(const LogicalNodePtr& node, std::vector<AggregatePlacement>& out) {
  if (node == nullptr) {
    return;
  }
  if (std::optional<AggregatePlacement> placement = PlacementOf(node); placement.has_value()) {
    out.push_back(std::move(*placement));
  }
  for (const LogicalNodePtr& input : InputsOf(*node)) {
    Walk(input, out);
  }
}

}  // namespace

std::string_view ToString(AggregatePlacement::Verdict verdict) {
  switch (verdict) {
    case AggregatePlacement::Verdict::kWorth:
      return "worth";
    case AggregatePlacement::Verdict::kNoReduction:
      return "no reduction";
    case AggregatePlacement::Verdict::kBelowAFilter:
      return "below a filter";
    case AggregatePlacement::Verdict::kJoinDiscards:
      return "the join discards";
    case AggregatePlacement::Verdict::kUnknown:
      return "unknown";
  }
  return "unknown";
}

namespace {

ColumnId Mint(ColumnId& last_id) {
  last_id = ColumnId{std::to_underlying(last_id) + 1};
  return last_id;
}

// Every kind this exchange performs computes the same call on both sides: a partial SUM is summed,
// a partial MIN is minimised. A count is the one that differs -- a count below, a SUM of counts
// above -- and since a count is refused here until a kind exists that can merge one (ADR 0032's
// G2c), there is no mapping left to write. The full table is in that ADR's Decision.
// A BoundColumn for `id`, from the columns of the plan that already name it.
struct Columns {
  std::unordered_map<ColumnId, BoundColumn> by_id;

  void Note(const BoundColumn& column) {
    if (column.id != kNoColumnId) {
      by_id.emplace(column.id, column);
    }
  }
  [[nodiscard]] std::optional<BoundColumn> Of(ColumnId id) const {
    const auto found = by_id.find(id);
    return found == by_id.end() ? std::nullopt : std::optional<BoundColumn>(found->second);
  }
};

// One placement performed, or nullopt where this declines to build it.
std::optional<LogicalNodePtr> Exchange(const AggregatePlacement& placement, ColumnId& last_id) {
  const auto& aggregate = std::get<GroupAggregateNode>(*placement.aggregate);
  const auto* join = std::get_if<JoinNode>(placement.join.get());
  if (join == nullptr || aggregate.input != placement.join) {
    return std::nullopt;  // a Compute between the two: the expression would have to travel down
  }
  // The finder refuses a residual before this runs; re-checked because it is the one assumption
  // whose future relaxation would reach the builder, where the pre-grouping destroys exactly the
  // per-row values a residual reads.
  if (!join->residual.empty()) {
    return std::nullopt;
  }
  // Refused for now, each because the plan cannot yet say what the rewrite needs:
  //
  //   - kAvg, whose partial is a (SUM, COUNT) pair and whose final divides, so the exchange would
  //     change the node's output arity.
  //   - the counts, and this one is the interesting refusal. A count decomposes into a count below
  //     and a SUM of counts above -- and an integer SUM returns HUGEINT, by the rule that keeps it
  //     from wrapping at int64 (AGENTS.md, ADR 0021), while COUNT's own output is BIGINT. So the
  //     honest final has a type the query did not ask for, and a plan cannot narrow it back: a
  //     CastExpr is an exact widening only. Summing partial counts needs a kind of its own, whose
  //     result is BIGINT and which cannot overflow one because it is bounded by the rows counted.
  //     **This is the refusal that matters**: the one placement the gate admits across the 22 is a
  //     count, so the rewrite currently fires on none of them.
  if (std::ranges::any_of(aggregate.aggregates, [](const AggregateCall& call) {
        return call.kind == AggKind::kAvg || call.kind == AggKind::kCountStar ||
               call.kind == AggKind::kCount;
      })) {
    return std::nullopt;
  }
  // Every column the pre-grouping names, as the plan already describes it.
  Columns known;
  for (const JoinKey& key : join->keys) {
    known.Note(key.left);
    known.Note(key.right);
  }
  for (const BoundColumn& key : aggregate.keys) {
    known.Note(key);
  }
  GroupAggregateNode partial{.input = placement.left_is_aggregated ? join->left : join->right,
                             .keys = {},
                             .key_ids = {},
                             .aggregates = {},
                             .span = aggregate.span};
  // A grouped aggregate DEFINES its key outputs, so each takes a fresh id and everything above the
  // join that read the old one is pointed at the new. Only the join's own key on this side and the
  // final aggregate's keys can read one -- by condition 3 those are the only aggregated-side
  // columns it groups by -- since its calls read the partials instead.
  std::unordered_map<ColumnId, ColumnId> renamed;
  for (const ColumnId id : placement.pre_grouping) {
    const std::optional<BoundColumn> column = known.Of(id);
    if (!column.has_value()) {
      return std::nullopt;  // a column this cannot name is one it cannot group by
    }
    partial.keys.push_back(*column);
    partial.key_ids.push_back(Mint(last_id));
    renamed.emplace(id, partial.key_ids.back());
  }
  const auto rename = [&renamed](BoundColumn& column) {
    if (const auto found = renamed.find(column.id); found != renamed.end()) {
      column.id = found->second;
    }
  };
  // The partial calls, each with an id of its own, and the finals that read them.
  std::vector<AggregateCall> finals;
  finals.reserve(aggregate.aggregates.size());
  for (const AggregateCall& call : aggregate.aggregates) {
    AggregateCall below = call;
    below.id = Mint(last_id);
    partial.aggregates.push_back(below);
    AggregateCall above = call;  // keeps the original id, so nothing above this changes
    above.arg =
        BoundColumn{.index = 0,
                    .id = below.id,
                    .name = call.arg.has_value() ? call.arg->name : std::string("count_star()"),
                    .type = below.type};
    finals.push_back(above);
  }
  JoinNode over = *join;
  for (JoinKey& key : over.keys) {
    rename(placement.left_is_aggregated ? key.left : key.right);
  }
  (placement.left_is_aggregated ? over.left : over.right) =
      std::make_shared<const LogicalNode>(std::move(partial));
  LogicalNodePtr input = std::make_shared<const LogicalNode>(std::move(over));

  // No padding is built, and with the counts refused none is needed: `needs_identity` says a
  // preserved row may find no partial, and the value the written query gave such a row is NULL for
  // every kind this performs -- which is already what an aggregate of a NULL partial answers. Only
  // a count wants something else there (1 for COUNT(*), 0 for COUNT(col)), so the mapping arrives
  // with the kind that makes a count exchangeable at all, and not before it can be reached.
  GroupAggregateNode final = aggregate;
  final.input = std::move(input);
  final.aggregates = std::move(finals);
  for (BoundColumn& key : final.keys) {
    rename(key);
  }
  return std::make_shared<const LogicalNode>(std::move(final));
}

}  // namespace

LogicalNodePtr ExchangeAggregates(const LogicalNodePtr& root, ColumnId& last_id) {
  if (root == nullptr) {
    return root;
  }
  std::vector<LogicalNodePtr> inputs = InputsOf(*root);
  bool changed = false;
  for (LogicalNodePtr& input : inputs) {
    LogicalNodePtr rewritten = ExchangeAggregates(input, last_id);
    changed = changed || rewritten != input;
    input = std::move(rewritten);
  }
  LogicalNodePtr here = changed ? WithInputs(*root, std::move(inputs)) : root;
  if (const std::optional<AggregatePlacement> placement = PlacementOf(here);
      placement.has_value() && placement->verdict == AggregatePlacement::Verdict::kWorth) {
    if (std::optional<LogicalNodePtr> exchanged = Exchange(*placement, last_id);
        exchanged.has_value()) {
      return *exchanged;
    }
  }
  return here;
}

std::vector<AggregatePlacement> AggregatePlacements(const LogicalPlan& plan) {
  std::vector<AggregatePlacement> out;
  Walk(plan.root, out);
  return out;
}

}  // namespace antb1::plan
