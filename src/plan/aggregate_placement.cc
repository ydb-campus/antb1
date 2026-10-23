#include "aggregate_placement.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
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
  // The nodes from the aggregate's input down to `node`, outermost first and `node` last. One
  // element is the shape ADR 0032 shipped; more is the chain this walks. The rewrite rebuilds them
  // over the join it has spliced a partial into, which is why they are kept and not just counted.
  std::vector<LogicalNodePtr> path;
  std::size_t joins = 0;  // joins on the path, `node` included: 1 is the join the aggregate reads
};

// Every join a grouped aggregate could be computed below, nearest first: the one it reads, then the
// one that join's aggregated side reads, and so on. Descends through Computes, collecting what each
// appends, so that an aggregate over a computed column resolves to the columns that computed it.
//
// `aggregated` says which input to descend into, and only the caller can answer it -- the side is
// the one carrying every aggregate call's argument, which is decided per join. So this yields a
// candidate and is asked where to go next.
template <class F>
void EachBelowAggregate(const LogicalNodePtr& input, const F& candidate) {
  Below out;
  LogicalNodePtr node = input;
  while (node != nullptr) {
    if (const auto* join = std::get_if<JoinNode>(node.get())) {
      out.node = node;
      out.join = join;
      out.path.push_back(node);
      ++out.joins;
      const std::optional<bool> left = candidate(out);
      if (!left.has_value() || join->left == nullptr || join->right == nullptr) {
        return;  // the candidate was refused, and a deeper one reads through it
      }
      node = *left ? join->left : join->right;
      continue;
    }
    const auto* compute = std::get_if<ComputeNode>(node.get());
    if (compute == nullptr) {
      return;  // a Filter removes rows, a Project moves ids: neither travels yet
    }
    for (std::size_t i = 0; i < compute->ids.size() && i < compute->exprs.size(); ++i) {
      out.computed.emplace(compute->ids[i], compute->exprs[i].get());
    }
    out.path.push_back(node);
    node = compute->input;
  }
}

// Which side of the join a column belongs to, resolving a computed column to the columns it reads.
//
// kLeft or kRight when every one of them is on that side. **kAbove when none of them is on
// either**, which a column introduced above this join is -- the chain of candidates reaches joins
// that a grouping column of the aggregate is not below at all, and such a column is simply not
// pre-grouped by. nullopt when the column reads both sides, or is computed from an expression this
// cannot resolve: those separate rows no pre-grouping of one input can.
enum class Side : std::uint8_t { kLeft, kRight, kAbove };

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
  bool any_above = false;
  for (const ColumnId read : reads) {
    // A computed column may read another computed column, which this does not follow: it resolves
    // one layer, which is what an aggregate over an arithmetic expression of table columns needs.
    if (left.contains(read)) {
      any_left = true;
    } else if (right.contains(read)) {
      any_right = true;
    } else {
      any_above = true;
    }
  }
  if (any_left && any_right) {
    return std::nullopt;  // both sides: no pre-grouping of one input separates these rows
  }
  if (any_left || any_right) {
    // A column reading one side and something from above reads rows this join has already paired,
    // so a pre-grouping of that side cannot stand for it.
    if (any_above) {
      return std::nullopt;
    }
    return any_left ? Side::kLeft : Side::kRight;
  }
  return Side::kAbove;
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
    case AggKind::kAvg:        // as a (SUM, COUNT) pair, divided above the join
    case AggKind::kSumCounts:  // a sum of counts is itself one, being associative
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

// The per-row cost of grouping by one column, coarsely: what a hash table hashes, compares and
// stores for it. A VARCHAR costs far more than the pointer and length a row holds -- hashing reads
// its bytes, a collision memcmps them, and the table copies them -- so it is costed well above the
// widest fixed-width type rather than at its handle's size.
constexpr int64_t kVarcharCost = 32;

int64_t GroupingCost(LogicalType type) {
  switch (type.id()) {
    case LogicalType::kBoolean:
      return 1;
    case LogicalType::kSmallInt:
    case LogicalType::kUSmallInt:
      return 2;
    case LogicalType::kInteger:
    case LogicalType::kDate:
      return 4;
    case LogicalType::kBigInt:
    case LogicalType::kDouble:
    case LogicalType::kTimestamp:
      return 8;
    case LogicalType::kHugeInt:
      return 16;
    case LogicalType::kDecimal:  // as ToArrow splits them: decimal64 up to the narrow width
      return type.width() <= LogicalType::kMaxNarrowDecimalWidth ? 8 : 16;
    case LogicalType::kVarchar:
      return kVarcharCost;
  }
  return kVarcharCost;  // an id this does not know, costed as the most expensive one it does
}

// The cost of grouping by a set of columns, or std::nullopt where any column's type is not among
// `types` -- which a column computed above the candidate join is. A placement is admitted on this
// quantity, so an unknown must withhold it rather than guess at it.
std::optional<int64_t> GroupingCostOf(std::span<const ColumnId> columns,
                                      const std::unordered_map<ColumnId, LogicalType>& types) {
  int64_t total = 0;
  for (const ColumnId column : columns) {
    const auto found = types.find(column);
    if (found == types.end()) {
      return std::nullopt;
    }
    total += GroupingCost(found->second);
  }
  return total;
}

// The gate's thresholds. Deliberately coarse: the quantities they compare are footer estimates,
// and ADR 0032's survey separates its two worthwhile queries from its refusals by an order of
// magnitude on each, not by a few percent.
constexpr int64_t kLargestRows = std::numeric_limits<int64_t>::max();
constexpr int64_t kLeastReduction = 3;     // rows per group, below which the grouping is overhead
constexpr int64_t kLeastKeptPercent = 90;  // of the groups built, surviving the joins above
// Times cheaper per row the pre-grouping must be than the grouping it feeds, for that to carry a
// placement on its own. Between the two placements measured on the query this was added for: the
// one that pays groups by a single BIGINT where the aggregate groups by seven columns including
// three wide strings (23x), and the one above it groups by all but one of those seven (1.2x).
constexpr int64_t kLeastNarrowing = 8;

// Whether the pre-grouping is far cheaper per row than the aggregate's own grouping.
bool MuchNarrower(const AggregatePlacement& placement) {
  return placement.pre_grouping_cost.has_value() && *placement.pre_grouping_cost > 0 &&
         placement.grouping_cost >= *placement.pre_grouping_cost * kLeastNarrowing;
}

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

// Whether the gate admits a placement: both of the two arguments for one, and nothing else. Asked
// by the finder's choice of candidate and by the rewrite, which must not drift apart.
bool Admitted(AggregatePlacement::Verdict verdict) {
  return verdict == AggregatePlacement::Verdict::kWorth ||
         verdict == AggregatePlacement::Verdict::kWorthNarrower;
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
  //
  // Unless the pre-grouping is far cheaper per row than the grouping it feeds. The reduction test
  // weighs the rows removed against a partial costed as if it grouped by what the aggregate groups
  // by, so it asks for three rows a group however narrow the partial is. Where the partial groups
  // by one fixed-width column and the aggregate by many wide ones, the work removed costs many
  // times the work added, and a reduction too small for this test -- or, as on the query this was
  // added for, one the footers cannot see at all -- still pays.
  //
  // **This is the gate's one condition not founded on an estimate**, and it is a heuristic: where
  // the reduction is genuinely absent the partial is pure overhead. It is bounded rather than
  // argued -- `kLeastNarrowing` is large, conditions 3 and 4 still refuse, and a placement that
  // reduces keeps the verdict it has today, so nothing already admitted moves.
  const bool reduces = below / groups >= kLeastReduction;
  const bool narrower = MuchNarrower(placement);
  if (!reduces && !narrower) {
    return Verdict::kNoReduction;
  }
  // Condition 4: the joins above must keep most of the groups, or the pre-aggregation builds them
  // to have them thrown away. `now` is what the join above actually emits, so the share of the
  // aggregated side's rows that survive it stands in for the share of its groups that do.
  if (Int128{now} * 100 < Int128{below} * kLeastKeptPercent) {
    return Verdict::kJoinDiscards;
  }
  return reduces ? Verdict::kWorth : Verdict::kWorthNarrower;
}

void Add(std::vector<ColumnId>& out, ColumnId id) {
  if (id != kNoColumnId && std::ranges::find(out, id) == out.end()) {
    out.push_back(id);
  }
}

// The placement at one candidate join, or none where any of ADR 0032's legality conditions fails
// there. `aggregate` is the node `below` was walked from.
std::optional<AggregatePlacement> PlacementAt(const LogicalNodePtr& node,
                                              const GroupAggregateNode& aggregate_node,
                                              const Below& below_at) {
  const GroupAggregateNode* aggregate = &aggregate_node;
  const Below* below = &below_at;
  if (!Exchangeable(*below->join)) {
    return std::nullopt;
  }
  const JoinNode& join = *below->join;
  if (join.left == nullptr || join.right == nullptr) {
    return std::nullopt;  // only a plan built by hand reaches this
  }
  // An outer join between the aggregate and the candidate pads rows the candidate's partials never
  // saw, which `needs_identity` does not describe -- it describes the candidate's own padding, not
  // one applied above it. Refused rather than reasoned about: no query this was measured on has
  // the shape, so admitting it would widen what the gate must classify before anything asks.
  for (const LogicalNodePtr& above : below->path) {
    if (above == below->node) {
      continue;
    }
    const auto* join_above = std::get_if<JoinNode>(above.get());
    if (join_above != nullptr && join_above->kind != JoinKind::kInner) {
      return std::nullopt;
    }
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
    // kAbove is a refusal here where it is merely "not pre-grouped by" for a grouping column: an
    // argument the candidate's inputs do not carry is one no partial below it can aggregate.
    if (!side.has_value() || *side == Side::kAbove ||
        (aggregated.has_value() && *aggregated != *side)) {
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
  // What each grouping costs per row. The types come from the BoundColumns the two nodes already
  // carry: a join key names a column of its own input, and the aggregate's keys name its own. A
  // pre-grouping column from neither -- one a Compute above the join defined, reached through
  // Reads -- leaves the cost unknown, and MuchNarrower then declines to argue from it.
  std::unordered_map<ColumnId, LogicalType> types;
  for (const JoinKey& key : join.keys) {
    types.emplace(key.left.id, key.left.type);
    types.emplace(key.right.id, key.right.type);
  }
  for (const BoundColumn& grouping : aggregate->keys) {
    types.emplace(grouping.id, grouping.type);
    out.grouping_cost += GroupingCost(grouping.type);
  }
  out.pre_grouping_cost = GroupingCostOf(out.pre_grouping, types);
  // The estimates the gate judges on, and then the verdict.
  const LogicalNodePtr& aggregated_side = left_aggregated ? join.left : join.right;
  out.rows_now = JoinRows(join);
  out.rows_below = EstimateRows(*aggregated_side);
  out.groups = DistinctValues(*aggregated_side, out.pre_grouping);
  out.verdict = Judge(out);
  out.joins_down = below->joins;
  out.path = below->path;
  return out;
}

// The placement to report for one grouped aggregate: **the deepest candidate the gate admits**, and
// where it admits none, the nearest -- so that EXPLAIN still says what was considered and why
// nothing was worth doing. One per aggregate, because explain.cc keys its note by the aggregate and
// appends, so several would stack several notes on one line.
std::optional<AggregatePlacement> PlacementOf(const LogicalNodePtr& node) {
  const auto* aggregate = std::get_if<GroupAggregateNode>(node.get());
  if (aggregate == nullptr || aggregate->aggregates.empty()) {
    return std::nullopt;  // SELECT DISTINCT: a grouping with no call has nothing to move
  }
  std::optional<AggregatePlacement> nearest;
  std::optional<AggregatePlacement> best;
  EachBelowAggregate(aggregate->input, [&](const Below& below) -> std::optional<bool> {
    std::optional<AggregatePlacement> here = PlacementAt(node, *aggregate, below);
    if (!here.has_value()) {
      return std::nullopt;  // refused here, and a deeper partial would have to pass through this
    }
    if (!nearest.has_value()) {
      nearest = here;
    }
    if (Admitted(here->verdict)) {
      best = here;  // the deepest so far: the walk only ever goes down
    }
    return here->left_is_aggregated;
  });
  return best.has_value() ? best : nearest;
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
    case AggregatePlacement::Verdict::kWorthNarrower:
      return "worth, a narrower pre-grouping";
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
  if (join == nullptr || placement.path.empty() || placement.path.back() != placement.join) {
    return std::nullopt;  // only a placement this module did not produce reaches this
  }
  // The finder refuses a residual before this runs; re-checked because it is the one assumption
  // whose future relaxation would reach the builder, where the pre-grouping destroys exactly the
  // per-row values a residual reads.
  if (!join->residual.empty()) {
    return std::nullopt;
  }
  // What is still refused, and why:
  //
  //   - kAvg, whose partial is a (SUM, COUNT) pair and whose final divides, so the exchange would
  //     change the node's output arity.
  //   - COUNT(*) where the grouped side can go missing. A count merges through kSumCounts, which
  //     reads a missing partial as having counted nothing -- which is exactly what COUNT(col)
  //     answered for a row the join found no match for, so that case needs nothing. COUNT(*)
  //     answered 1 there, having counted the padded row itself, so it needs a value supplied
  //     rather than a zero assumed, and that mapping is not built.
  if (std::ranges::any_of(aggregate.aggregates, [&placement](const AggregateCall& call) {
        return call.kind == AggKind::kAvg ||
               (call.kind == AggKind::kCountStar && placement.needs_identity);
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
    if (call.kind == AggKind::kCountStar || call.kind == AggKind::kCount) {
      above.kind = AggKind::kSumCounts;  // a count below, a sum of those counts above
    }
    above.arg =
        BoundColumn{.index = 0,
                    .id = below.id,
                    .name = call.arg.has_value() ? call.arg->name : std::string("count_star()"),
                    .type = below.type};
    finals.push_back(above);
  }
  // The path, rebuilt. A Compute on it moves DOWN onto the aggregated side, because the call
  // argument may be one of its outputs and the partial has to compute it -- which is sound only
  // while nothing else reads that output, checked below. The joins are rebuilt above the partial,
  // each renaming whatever pre-grouping column its keys read.
  IdSet read_above;  // ids the rebuilt path and the final grouping still read
  for (const BoundColumn& key : aggregate.keys) {
    read_above.insert(key.id);
  }
  for (const LogicalNodePtr& above : placement.path) {
    if (const auto* join_above = std::get_if<JoinNode>(above.get())) {
      for (const JoinKey& key : join_above->keys) {
        read_above.insert(key.left.id);
        read_above.insert(key.right.id);
      }
    }
  }
  // A partial outputs its keys and its calls and nothing else, so every other column of the
  // aggregated side is gone above it. A node on the path that reads one -- a join key or a residual
  // -- would then read an id its input does not carry, which ResolvePositions aborts on rather than
  // answers wrongly. Condition 3 covers the candidate's own keys and the grouping set; this covers
  // the rest of the path, and refuses what it cannot rename.
  const IdSet aggregated_ids = OutputSet(*partial.input);
  for (const LogicalNodePtr& above : placement.path) {
    const auto* join_above = std::get_if<JoinNode>(above.get());
    if (join_above == nullptr || above == placement.join) {
      continue;
    }
    std::vector<ColumnId> reads;
    for (const JoinKey& key : join_above->keys) {
      reads.push_back(key.left.id);
      reads.push_back(key.right.id);
    }
    for (const ExprPtr& expr : join_above->residual) {
      CollectColumnIds(*expr, reads);
    }
    for (const ColumnId id : reads) {
      if (aggregated_ids.contains(id) && !renamed.contains(id)) {
        return std::nullopt;
      }
    }
  }

  // Innermost first, so each Compute wraps the one below it as it did in the written plan, and so
  // that a Compute reading a deeper one's output still finds it.
  IdSet available = aggregated_ids;
  LogicalNodePtr moved_over = partial.input;
  for (const LogicalNodePtr& above : std::ranges::reverse_view(placement.path)) {
    const auto* compute = std::get_if<ComputeNode>(above.get());
    if (compute == nullptr) {
      continue;
    }
    for (const ColumnId id : compute->ids) {
      if (read_above.contains(id)) {
        return std::nullopt;  // its output is read above the partial as well as by a call
      }
    }
    std::vector<ColumnId> reads;
    for (const ExprPtr& expr : compute->exprs) {
      CollectColumnIds(*expr, reads);
    }
    if (std::ranges::any_of(reads,
                            [&available](const ColumnId id) { return !available.contains(id); })) {
      return std::nullopt;  // it reads a column the aggregated side does not carry
    }
    ComputeNode moved = *compute;
    moved.input = moved_over;
    for (const ColumnId id : moved.ids) {
      available.insert(id);
    }
    moved_over = std::make_shared<const LogicalNode>(std::move(moved));
  }
  partial.input = std::move(moved_over);

  JoinNode candidate = *join;
  for (JoinKey& key : candidate.keys) {
    rename(placement.left_is_aggregated ? key.left : key.right);
  }
  (placement.left_is_aggregated ? candidate.left : candidate.right) =
      std::make_shared<const LogicalNode>(std::move(partial));
  LogicalNodePtr input = std::make_shared<const LogicalNode>(std::move(candidate));
  // Up the path, each node standing in for the one it replaced -- a Compute that moved down stands
  // in too, since whatever read it above now reads the rebuilt join in its place. Matching the
  // child against that node rather than assuming a side is what keeps this honest about the shape
  // the finder walked: a path that is not a chain of single children declines here.
  LogicalNodePtr replaced = placement.path.back();
  for (std::size_t i = placement.path.size() - 1; i-- > 0;) {
    const LogicalNodePtr& above = placement.path[i];
    if (std::get_if<ComputeNode>(above.get()) != nullptr) {
      replaced = above;
      continue;
    }
    const auto* join_above = std::get_if<JoinNode>(above.get());
    if (join_above == nullptr) {
      return std::nullopt;
    }
    JoinNode over = *join_above;
    for (JoinKey& key : over.keys) {
      rename(key.left);  // a no-op on the side that is not aggregated: its ids are disjoint
      rename(key.right);
    }
    if (over.left == replaced) {
      over.left = input;
    } else if (over.right == replaced) {
      over.right = input;
    } else {
      return std::nullopt;
    }
    input = std::make_shared<const LogicalNode>(std::move(over));
    replaced = above;
  }

  // No padding is built, and none is needed for any kind that reaches here. `needs_identity` says a
  // preserved row may find no partial at all, and the value the written query gave such a row is:
  //
  //   NULL for SUM, MIN and MAX   -- already what an aggregate of a missing partial answers
  //   0    for COUNT(col)         -- and kSumCounts reads a missing partial as having counted
  //                                  nothing, so that is already what it answers too
  //   1    for COUNT(*)           -- it counted the padded row, so this one does want a value
  //                                  supplied, which is why it is refused above
  //
  // So the mapping ADR 0032's condition 5 asks for is, for every spelling this performs, a property
  // of the kind rather than something to build.
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
      placement.has_value() && Admitted(placement->verdict)) {
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
