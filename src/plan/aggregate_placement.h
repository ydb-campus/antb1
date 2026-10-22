#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "antb1/plan/logical_plan.h"

// A grouped aggregate computed on one input of the join below it
// (docs/adr/0032-aggregate-placement.md). Two things live here: `AggregatePlacements` finds where
// the exchange is legal and what the gate makes of it, reporting only (item G1), and
// `ExchangeAggregates` performs the ones the gate admits (item G2b). The finder shipped first on
// the argument that a pass which only reports cannot give a wrong answer, and it earned that -- two
// defects in the legality test came out of its review before any answer could move.
//
// Condition 3's second half is the one to keep in view: a pre-grouping missing a grouping column of
// the aggregated side merges rows the query separates, which changes aggregate values rather than
// row counts, so nothing crashes and no count looks wrong.
//
// Private to the plan module: explain.cc and this module's tests include it. Public when a consumer
// needs it, as ADR 0030's E1 was (ADR 0027's S1: a producer shipped ahead of its consumer is a
// producer nothing measures).

namespace antb1::plan {

// One legal placement of a grouped aggregate below the join it reads.
struct AggregatePlacement {
  LogicalNodePtr aggregate;  // the GroupAggregateNode, as a walk from the root meets it
  LogicalNodePtr join;       // the JoinNode it reads, below any Compute between them
  // Which input of that join the aggregates read: the side a rewrite would pre-group. The other
  // side carries the grouping columns that do not come from this one.
  bool left_is_aggregated = false;
  // What that side must be grouped by for the rewrite to keep the answer: its join columns, then
  // every grouping column of `aggregate` that comes from it (ADR 0032's condition 3). Ids, not
  // indices, because every node between the two positions moves an index and none moves an id.
  //
  // **Every id here is one the aggregated side outputs**, which is what makes the list usable by a
  // rewrite. A join key is a column of its own input by construction, and a widened key's Compute
  // sits on that input's branch; a grouping column the query computed ABOVE the join names no
  // column below it, so this holds the columns that computed it instead -- grouping finer than the
  // outer set, which keeps the answer because the final step groups again.
  std::vector<ColumnId> pre_grouping;
  // Grouping columns of `aggregate` that come from the aggregated side, of `pre_grouping`. Zero
  // means the pre-grouping is the join key alone, which is the shape where the reduction is most
  // often real; above zero is the shape where condition 3's second half is load-bearing.
  std::size_t grouping_from_aggregated_side = 0;
  // The aggregated side is the null-supplying input of an outer join, so a rewrite's final step has
  // to supply, for a preserved row with no partial, what the written query answered over the one
  // padded row the join gave it (condition 5). That value is per call and COUNT(*) is the one that
  // surprises:
  //
  //   COUNT(*)      1     -- it counted the padded row itself
  //   COUNT(col)    0     -- the padded column was NULL, so it counted nothing
  //   SUM, MIN, MAX, AVG   NULL
  //
  // So "the aggregate over the empty set" is the right rule for every call but COUNT(*), which is
  // over one row and not none.
  bool needs_identity = false;

  // What the gate of ADR 0032 makes of this placement: `worth` only when every condition it can
  // decide holds. The estimates are the ones a plan node can reach, so an unknown is a refusal --
  // this is a judgement about *whether to rewrite*, and the shape of its evidence is that a small
  // group count argues for one, so an unknown group count must argue against.
  enum class Verdict : std::uint8_t {
    kWorth,        // every condition the gate can decide holds
    kNoReduction,  // the pre-grouping makes about as many groups as it reads rows (conditions 2, 5)
    kBelowAFilter,  // the aggregate would read more rows there than it reads now (condition 3)
    kJoinDiscards,  // the joins above throw away most of the groups it would build (condition 4)
    kUnknown,       // the footers do not give an estimate this needs
  };
  Verdict verdict = Verdict::kUnknown;
  // What the verdict was decided on, each std::nullopt where nothing estimated it: the rows the
  // aggregate reads now, the rows it would read below, and the groups it would build there.
  std::optional<int64_t> rows_now;
  std::optional<int64_t> rows_below;
  std::optional<int64_t> groups;
};

// "worth", "no reduction", and so on.
std::string_view ToString(AggregatePlacement::Verdict verdict);

// Every legal placement in `plan`, in the order a walk from the root meets its aggregates.
//
// A placement is reported only when all of ADR 0032's legality conditions hold, and the test is
// deliberately **blind to functional dependencies**: where a grouping column of the aggregated side
// is determined by the join key, omitting it from the pre-grouping still answers correctly, so a
// test that reasoned about dependencies would pass on exactly the queries that hide the error.
//
// What is refused, each because refusing reports less and reporting less is always safe:
//
//   - anything but a Compute between the aggregate and the join. A Filter removes rows, and its
//     predicate may read both inputs, so it cannot simply travel with the aggregate; a Project
//     mints new ids even for a column it passes through. Both are tractable and neither is needed
//     by the two queries this ADR measures. In a bound plan the two arrive together: a conjunct
//     over one relation is pushed below the join, so a Filter reaches this position only above a
//     derived table's Project, which is why the tests pin the pair and not the Filter alone.
//   - a join carrying a residual, which decides which pairs match on something other than the
//     keys. The pre-grouping destroys exactly the per-row values the residual reads, so a partial
//     would aggregate rows the residual rejects and no final step can take that back. A residual
//     reading only the side that is not aggregated would be safe and is not admitted yet.
//   - a join whose kind does not output both inputs' columns, since an aggregate cannot read what
//     a semi or anti join does not emit, and a one-row join has no keys to group by.
//   - a join with no keys: the exchange needs an equi-join on the columns the side is grouped by.
//   - an aggregate call reading both inputs, or an expression mixing them.
//   - COUNT(DISTINCT), which is not decomposable unless the distinct column enters the
//     pre-grouping -- which makes that pre-grouping a key of its side, so the exchange would group
//     once per row and reduce nothing.
//   - a placement no aggregate call names a side for, which is every call being COUNT(*). Both
//     sides are then legal and nothing here chooses between them; reporting both would overstate
//     what this pass knows.
// Two of ADR 0032's six gate conditions are not decidable here, and saying which is the point:
//
//   - "not a superkey" has no metadata behind it, because Parquet carries no keys. Its effect is
//     covered by the reduction test, which refuses the same shapes for a measured reason instead of
//     an asserted one -- but only where the footers give a domain, so a table without statistics is
//     refused as unknown rather than admitted.
//   - condition 5's second half, "a join between the two positions multiplies rows", needs to know
//     whether a build is unique on its key, which is a runtime fact. The obvious plan-time proxy --
//     a join emitting more than its larger input -- is wrong, and measurably: the query this ADR
//     gains most on joins 150,000 rows to 1,500,000 and emits 1,500,000, multiplying nothing at
//     all, and the rewrite is still worth 0.494x there because what shrinks is the build and the
//     final grouping's input rather than the join's output.
//   - "do not destroy an existing prune" is not a property of a logical plan at all: ADR 0029's
//     filters are attached by the physical planner, long after this runs. It belongs with the
//     rewrite and not with this report.
//
// So a `kWorth` verdict here means "every condition a logical plan can decide", which is weaker
// than ADR 0032's gate and is why this reports rather than rewrites.
std::vector<AggregatePlacement> AggregatePlacements(const LogicalPlan& plan);

// `root` with every placement the gate calls `kWorth` performed: the aggregated side grouped by
// `pre_grouping` into partial states, the join over those partials, and the original aggregate left
// in place over them as the final step (ADR 0032's item G2b). `root` itself where none fires, which
// is every plan the gate does not admit.
//
// `last_id` is the largest column id in use and is advanced for each column this mints; the caller
// owns it because a plan's ids must stay unique across every pass that adds one.
//
// **Indices are not maintained and do not need to be.** A column this introduces carries the id of
// the column it stands for, or a fresh one, and `plan::ResolvePositions` -- which `Optimize` runs
// after every pass -- recomputes every `BoundColumn::index` from the ids. A rewrite that tried to
// keep indices in step would be maintaining a second, redundant truth.
//
// What it declines to build, leaving the plan as it is even where the gate admits the placement:
//
//   - a `Compute` between the aggregate and the join. The aggregate's argument is then an
//     expression over the aggregated side, so the `Compute` would have to travel down with the
//     partial; sound, and not needed by the one placement of the 22 the gate admits.
//   - `kAvg`, whose partial is a (SUM, COUNT) pair and whose final divides. Decomposable, but it
//     is the one kind whose rewrite changes the node's output arity, and no query of the 22 has an
//     AVG in a position the gate admits.
LogicalNodePtr ExchangeAggregates(const LogicalNodePtr& root, ColumnId& last_id);

}  // namespace antb1::plan
