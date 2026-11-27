#pragma once

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include "antb1/plan/logical_plan.h"

// Sub-plans of one plan that compute the same rows (docs/adr/0030-shared-sub-plans.md, E1).
//
// Nothing here shares anything: it reports what a later item could compute once. A plan built by
// the binder holds a subtree per reference -- ADR 0022 binds a CTE's body once per reference, and a
// subquery that repeats a FROM list produces the same duplication with no CTE in it. The optimizer
// uses these groups to preserve reuse and the physical planner chooses which results to hold.
//
// Public since E2, which is what reads it from exec to decide what to materialise; E1 kept it
// module-internal because only explain.cc and this module's tests used it (ADR 0027's S1: a
// producer shipped ahead of its consumer is a producer nothing measures).

namespace antb1::plan {

// A shared result and its readers. With a combined producer, each reader takes one disjoint
// group-key partition; otherwise every member computes the same rows.
struct SharedSubPlan {
  // Two or more distinct nodes, in the order a walk from the root meets them.
  std::vector<LogicalNodePtr> members;
  // The member whose output columns contain every other member's: the one a later item would
  // actually compute, the others reading a subset of its result. A group whose members' outputs do
  // not nest this way is not reported at all, since one result could not serve them.
  std::size_t widest = 0;
  // Per member, for each of its output columns, that column's position in members[widest]'s
  // output. This is what lets a consumer read the shared result without anything above it being
  // rewritten: give it those positions and its own column ids, and its own view is unchanged.
  std::vector<std::vector<int>> columns;
  // The table fields the one shared sub-plan would read: per scan, the union over the members, and
  // the sum of those. A member reads at most this many, so the difference is what sharing would
  // decode for a consumer that does not want it.
  int64_t fields = 0;
  LogicalNodePtr combined_producer;
  // True when widening the equality changes the input to the producer's lookup build.
  bool widens_join_build = false;
  // Each reader needs its own per-part accumulation, merge and emission order for DOUBLE SUM.
  bool separate_partition_states = false;
  // Empty for exact matches, otherwise one equality on each member's own output column.
  std::vector<Predicate> reader_predicates;
};

// Index parents once so a shared producer can find restrictions common to all its readers.
// A consumer with no restriction on a key needs its full domain.
class SharedConsumerFilters {
 public:
  explicit SharedConsumerFilters(const LogicalPlan& plan);
  [[nodiscard]] LogicalNodePtr Producer(const SharedSubPlan& group) const;

 private:
  static bool HasBoundedDomain(const Predicate& predicate);
  std::vector<Predicate> ReaderPredicates(const LogicalNode* member) const;
  static LogicalNodePtr PushGroupKey(const LogicalNodePtr& producer, const Predicate& predicate);
  std::unordered_map<const LogicalNode*, const LogicalNode*> parents_;
};

// The maximal groups of `plan`, in the order a walk from the root meets them; empty when nothing
// repeats.
//
// Two sub-plans are equivalent when they have the same shape and the same per-node content **with
// every column reference compared by where its column comes from** rather than by its position or
// its id, and their scans read the same table. The fields a scan reads are deliberately *not* part
// of the comparison: selecting fields does not change which rows a scan returns, so two scans of
// one table differing only there compute the same rows, and the shared sub-plan reads the union of
// what its consumers want. That is what makes this reach beyond exact equality -- and it is the
// whole of the difference in two of the three queries the ADR measures.
//
// Why by origin and not by id: BoundColumn::index is a position in the input, so a branch whose
// scan reads one field fewer has different indices in every node above it; and a ColumnId is minted
// per reference, so two references to one CTE never share one. Only the source of a column is the
// same on both sides.
//
// **And by the path it is read through, not by origin alone.** A table's field is one origin
// wherever it is read, so the two sides of a self-join -- and the COUNT(*)s of two aggregations --
// read columns of one origin. Every reference therefore also carries its path: the inputs it
// passes, from the node that reads it down to the node that defines it. Equivalent sub-plans have
// one shape and so the same paths; without them, two sub-plans grouping one join by `a.k` and by
// `b.k` were one group, and the second was answered from the first's result.
//
// Groups are **outermost-first and never wholly nested**: where a subtree repeats so does every
// subtree inside it, and a group all of whose members lie inside another group's members is not
// reported -- the outer one is what a later item would materialise. A group with even one member
// outside is still reported, and its `members` then do include the nested ones, because sharing the
// outer group leaves that outside member duplicating work the report would otherwise hide. Three
// scans of one table where two of them sit under equivalent filters is exactly that shape.
//
// A sub-plan that holds a Sort, a Limit, a Window, a RowCount or a dependent join is never a
// member. Order and row limits are not properties a shared result can hand to two consumers without
// deciding whose order wins; windows may depend on that order. A metadata row count is not worth
// sharing, and a dependent join cannot reach here (plan::Optimize removes every one). Refusing is
// always safe: it reports less.
//
// A repeated UnionAll is keyed by its inputs in order. The structural report stops at unions and
// set operations and reports outermost matches regardless of their cost. Physical materialization
// visits their branches and continues past matches that are ineligible.
std::vector<SharedSubPlan> SharedSubPlans(const LogicalPlan& plan);

// The physical planner's non-overlapping, worthwhile producers, including compound queries
// and candidates inside their branches. It leaves the logical EXPLAIN's structural groups alone.
// Ordered inner joins can share a scan between two differently filtered DOUBLE sums using
// independent states. The guarded deferred-dimension alternative also admits exact aggregates.
std::vector<SharedSubPlan> MaterializableSubPlans(const LogicalPlan& plan,
                                                  bool share_filtered_groups = false);

// Recompute exact shares, excluding the subtrees already used by physically accepted partitions.
// An independent fallback has no such exclusions.
std::vector<SharedSubPlan> MaterializableIndependentSubPlans(
    const LogicalPlan& plan, std::span<const LogicalNodePtr> accepted_members = {});

// The cap used for both the plan-time estimate and the bytes held at execution.
inline constexpr int64_t kMaxSharedResultBytes = int64_t{256} * 1024 * 1024;

// Whether the widest member's aggregate saves enough input work and fits the estimate (ADR 0030).
// The optimizer and the physical materialization decision use the same policy.
bool WorthSharing(const SharedSubPlan& group);

// Share one allowance across all candidates and rewrites in a planning stage. Only an otherwise
// oversized result spends it, on plan nodes, predicates and computed-column lookups.
inline constexpr int kSharingEstimateBudget = 256;
bool WorthSharing(const SharedSubPlan& group, int& remaining);

}  // namespace antb1::plan
