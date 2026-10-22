#include "aggregate_placement.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "antb1/plan/logical_plan.h"

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

std::vector<AggregatePlacement> AggregatePlacements(const LogicalPlan& plan) {
  std::vector<AggregatePlacement> out;
  Walk(plan.root, out);
  return out;
}

}  // namespace antb1::plan
