#include "../aggregate_placement.h"

#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "antb1/plan/catalog.h"
#include "antb1/plan/explain.h"
#include "antb1/plan/logical_plan.h"

#include "plan_test_util.h"

namespace antb1::plan {
namespace {

using testing::BindSql;
using testing::MakeCatalog;

LogicalPlan Planned(std::string_view sql) {
  static const Catalog catalog = MakeCatalog();
  auto plan = BindSql(sql, catalog);
  EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
  return plan.ok() ? Optimized(*plan) : LogicalPlan{};
}

// A catalog of its own for the gate's cases, so that declaring statistics here cannot move another
// suite's expectations. `facts` holds 1000 rows: `few` takes 10 distinct values, `many` is unique,
// and `plain` declares nothing. The dimensions differ only in how many of the facts' keys they
// carry, which is what the gate's conditions turn on.
Catalog GateCatalog() {
  Catalog catalog;
  const auto facts = std::make_shared<testing::FakeTable>(
      arrow::schema({arrow::field("few", arrow::int32()), arrow::field("many", arrow::int32()),
                     arrow::field("plain", arrow::int32()), arrow::field("v", arrow::int32())}),
      1000);
  facts->WithDistinctCount(0, /*field=*/0, 10).WithDistinctCount(0, /*field=*/1, 1000);
  const auto wide =
      std::make_shared<testing::FakeTable>(arrow::schema({arrow::field("dk", arrow::int32())}), 10);
  wide->WithDistinctCount(0, /*field=*/0, 10);
  const auto narrow =
      std::make_shared<testing::FakeTable>(arrow::schema({arrow::field("dk", arrow::int32())}), 2);
  narrow->WithDistinctCount(0, /*field=*/0, 2);
  // Carries every value of the facts' unique key, so a join on it discards nothing and the
  // reduction test is reached rather than shadowed by the condition checked before it.
  const auto twin = std::make_shared<testing::FakeTable>(
      arrow::schema({arrow::field("dk", arrow::int32())}), 1000);
  twin->WithDistinctCount(0, /*field=*/0, 1000);
  // A chain of three, the shape the iterated walk was added for in miniature. The join order
  // search picks `(deep JOIN mid) JOIN top` because `top` is the largest of the three, which is
  // what puts `deep` two joins below the aggregate rather than one. `deep` holds two rows per key,
  // so its pre-grouping reduces by two -- under the reduction the gate asks for, leaving the
  // narrowing to carry it. `mid.g` declares no distinct count, so the nearest placement's group
  // count is unknown exactly as the measured query's is.
  const auto deep = std::make_shared<testing::FakeTable>(
      arrow::schema({arrow::field("mk", arrow::int32()), arrow::field("v", arrow::int32()),
                     arrow::field("dw", arrow::binary()), arrow::field("ak", arrow::int32())}),
      1000);
  deep->WithDistinctCount(0, /*field=*/0, 500).WithDistinctCount(0, /*field=*/2, 500);
  const auto mid = std::make_shared<testing::FakeTable>(
      arrow::schema({arrow::field("mk", arrow::int32()), arrow::field("tk", arrow::int32()),
                     arrow::field("g", arrow::binary())}),
      500);
  mid->WithDistinctCount(0, /*field=*/0, 500).WithDistinctCount(0, /*field=*/1, 500);
  const auto top = std::make_shared<testing::FakeTable>(
      arrow::schema({arrow::field("tk", arrow::int32()), arrow::field("w1", arrow::binary()),
                     arrow::field("w2", arrow::binary()), arrow::field("w3", arrow::binary())}),
      5000);
  top->WithDistinctCount(0, /*field=*/0, 500);
  const auto must = [](const arrow::Status& status) { EXPECT_TRUE(status.ok()) << status; };
  must(catalog.Register("deep", deep));
  must(catalog.Register("mid", mid));
  must(catalog.Register("top", top));
  must(catalog.Register("facts", facts));
  must(catalog.Register("wide", wide));
  must(catalog.Register("narrow", narrow));
  must(catalog.Register("twin", twin));
  return catalog;
}

// Placements over the gate's own catalog, which is where the chain fixture lives. The bound plan,
// for the reason Verdicts gives.
std::vector<std::string> GatePlacements(std::string_view sql) {
  static const Catalog catalog = GateCatalog();
  auto plan = BindSql(sql, catalog);
  EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
  std::vector<std::string> out;
  if (!plan.ok()) {
    return out;
  }
  for (const AggregatePlacement& p : AggregatePlacements(*plan)) {
    out.push_back(std::string(p.left_is_aggregated ? "left " : "right ") +
                  std::to_string(p.pre_grouping.size()) + "+" +
                  std::to_string(p.grouping_from_aggregated_side) +
                  (p.joins_down > 1 ? " " + std::to_string(p.joins_down) + " down" : ""));
  }
  return out;
}

// The optimized plan of a gate-catalog query, to read the shape the exchange built.
std::string GatePlan(std::string_view sql) {
  static const Catalog catalog = GateCatalog();
  auto plan = BindSql(sql, catalog);
  EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
  return plan.ok() ? Explain(Optimized(*plan)) : std::string();
}

std::vector<std::string> Verdicts(std::string_view sql) {
  static const Catalog catalog = GateCatalog();
  auto plan = BindSql(sql, catalog);
  EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
  std::vector<std::string> out;
  if (!plan.ok()) {
    return out;
  }
  // The bound plan, not the optimized one: a verdict is what the gate makes of a placement
  // *before* the exchange, and Optimize performs the ones it calls worth -- so reading the
  // optimized plan would ask the gate about a plan it has already changed.
  for (const AggregatePlacement& p : AggregatePlacements(*plan)) {
    out.emplace_back(ToString(p.verdict));
  }
  return out;
}

// Each placement as "<side> <pre-grouping size>+<from the grouping set>[ padding]", so a case reads
// as the three things a rewrite would need: which input to group, how wide the pre-grouping is, and
// whether the final step has to pad an unmatched row.
std::vector<std::string> Placements(std::string_view sql) {
  std::vector<std::string> out;
  for (const AggregatePlacement& p : AggregatePlacements(Planned(sql))) {
    out.push_back(std::string(p.left_is_aggregated ? "left " : "right ") +
                  std::to_string(p.pre_grouping.size()) + "+" +
                  std::to_string(p.grouping_from_aggregated_side) +
                  (p.needs_identity ? " padding" : "") +
                  (p.joins_down > 1 ? " " + std::to_string(p.joins_down) + " down" : ""));
  }
  return out;
}

// The chain fixture's query: the aggregate reads `deep`, which the join order search puts two
// joins below it, and groups by `top`'s three VARCHARs and `mid`'s one.
constexpr std::string_view kChain =
    "SELECT top.w1, top.w2, top.w3, mid.g, SUM(deep.v) FROM deep, mid, top "
    "WHERE deep.mk = mid.mk AND mid.tk = top.tk GROUP BY top.w1, top.w2, top.w3, mid.g";

// The shape ADR 0032 is written for: the aggregates read one input, the grouping key comes from the
// other, and the pre-grouping is the join key alone. `ok` holds the aggregated side.
TEST(AggregatePlacementTest, FindsTheShapeTheAdrIsWrittenFor) {
  EXPECT_EQ(Placements("SELECT t.i16, SUM(ok.i16) FROM t, ok WHERE t.i32 = ok.i16 GROUP BY t.i16"),
            (std::vector<std::string>{"right 1+0"}));
}

// An aggregate over an expression of two columns of one input. This is the shape of one of the two
// queries the ADR measures, and it is why a Compute between the aggregate and the join has to be
// looked through rather than refused: the aggregate's argument is the Compute's own column, and
// only the expression says which input computed it.
TEST(AggregatePlacementTest, LooksThroughAComputeToTheColumnsAnExpressionReads) {
  EXPECT_EQ(Placements("SELECT t.i16, SUM(ok.i16 * 2) FROM t, ok WHERE t.i32 = ok.i16 "
                       "GROUP BY t.i16"),
            (std::vector<std::string>{"right 1+0"}));
}

// Condition 3's second half, which is the one that changes aggregate *values* when it is dropped: a
// grouping column from the aggregated side must join the pre-grouping, so the count rises to two.
TEST(AggregatePlacementTest, AGroupingColumnOfTheAggregatedSideJoinsThePreGrouping) {
  EXPECT_EQ(Placements("SELECT t.i16, ok.s, SUM(ok.i16) FROM t, ok WHERE t.i32 = ok.i16 "
                       "GROUP BY t.i16, ok.s"),
            (std::vector<std::string>{"right 2+1"}));
}

// The aggregated side is the null-supplying input, so an unmatched row of the preserved side has no
// partial to read and the final step has to supply the aggregate over the empty set.
TEST(AggregatePlacementTest, ReportsPaddingWhenTheAggregatedSideCanBeMissing) {
  EXPECT_EQ(Placements("SELECT t.i16, SUM(ok.i16) FROM t LEFT JOIN ok ON t.i32 = ok.i16 "
                       "GROUP BY t.i16"),
            (std::vector<std::string>{"right 1+0 padding"}));
}

// The preserved side aggregated instead: the join invents no row of `t`, so nothing has to be
// padded even though the join is the same outer one.
TEST(AggregatePlacementTest, NoPaddingWhenThePreservedSideIsTheAggregatedOne) {
  EXPECT_EQ(Placements("SELECT ok.i16, SUM(t.i32) FROM t LEFT JOIN ok ON t.i32 = ok.i16 "
                       "GROUP BY ok.i16"),
            (std::vector<std::string>{"left 1+0"}));
}

// The note a reader sees, pinned here because it is the whole of what G1 ships: the pass is
// internal and `EXPLAIN` is its only consumer.
TEST(AggregatePlacementTest, SaysSoInExplain) {
  const std::string plan =
      Explain(Planned("SELECT t.i16, ok.s, SUM(ok.i16) FROM t LEFT JOIN ok ON t.i32 = ok.i16 GROUP "
                      "BY t.i16, ok.s"));
  // "unknown" because `ok` declares no statistics, so nothing estimates the groups a pre-grouping
  // of it would build -- and an unknown is a refusal here, since a small group count is what would
  // argue for the rewrite.
  EXPECT_NE(plan.find("[groupable below the right of join, 2 keys, 1 from the grouping set, "
                      "padding, unknown]"),
            std::string::npos)
      << plan;
}

// COUNT(*) beside a call that names a side, on the null-supplying input. Reported, and the point
// of the case is the identity the note stands for: the written query answers 1 for a preserved row
// with no match, because the join gave it one padded row to count, where every other kind answers
// what it answers over no rows at all.
TEST(AggregatePlacementTest, CountStarIsTheOneWhosePaddedValueIsNotEmpty) {
  EXPECT_EQ(
      Placements("SELECT t.i16, COUNT(*), COUNT(ok.i16) FROM t LEFT JOIN ok ON t.i32 = ok.i16 "
                 "GROUP BY t.i16"),
      (std::vector<std::string>{"right 1+0 padding"}));
}

// A join key whose two types differ: the narrower side is widened by a Compute on its own branch,
// so the column the aggregated side groups by is below the join after all and the placement stands.
// Worth a case because the opposite is easy to assume.
TEST(AggregatePlacementTest, AWidenedJoinKeyIsStillBelowTheJoin) {
  EXPECT_EQ(Placements("SELECT t.i16, SUM(ok.i16) FROM t, ok WHERE t.i32 = ok.i16 GROUP BY t.i16"),
            (std::vector<std::string>{"right 1+0"}));
}

// A grouping column of the aggregated side that a Compute above the join defines. The pre-grouping
// names the column that computed it, which exists below the join, rather than the one the grouping
// set names, which does not.
TEST(AggregatePlacementTest, AComputedGroupingColumnNamesWhatComputedIt) {
  EXPECT_EQ(Placements("SELECT ok.i16 + 1, SUM(ok.i16) FROM t, ok WHERE t.i16 = ok.i16 "
                       "GROUP BY ok.i16 + 1"),
            (std::vector<std::string>{"right 1+0"}));
}

// The gate, on the one shape it exists to admit: the pre-grouping reduces ten to one, and the join
// above keeps every row because the dimension carries every key the facts hold.
TEST(AggregatePlacementTest, TheGateAdmitsAReductionTheJoinKeeps) {
  EXPECT_EQ(Verdicts("SELECT wide.dk, SUM(facts.v) FROM facts, wide WHERE facts.few = wide.dk "
                     "GROUP BY wide.dk"),
            (std::vector<std::string>{"worth"}));
}

// Each refusal the gate can decide, and what decides it.
TEST(AggregatePlacementTest, TheGateRefusals) {
  struct Case {
    std::string_view sql;
    std::string_view verdict;
    std::string_view why;
  };
  for (const Case& c : {
           // The pre-grouping key is unique in the facts, so grouping by it makes one group a row
           // and the pre-aggregation is pure overhead. This is the shape ADR 0032's survey found
           // in most of the queries that are legal but not worth it.
           Case{.sql = "SELECT twin.dk, SUM(facts.v) FROM facts, twin WHERE facts.many = twin.dk "
                       "GROUP BY twin.dk",
                .verdict = "no reduction",
                .why = "the pre-grouping key is unique"},
           // The same unique key against a dimension holding a tenth of its values: the join keeps
           // ten rows where the pre-aggregation would read a thousand. Two conditions hold at once
           // here, and the order of the checks decides which one is reported.
           Case{.sql = "SELECT wide.dk, SUM(facts.v) FROM facts, wide WHERE facts.many = wide.dk "
                       "GROUP BY wide.dk",
                .verdict = "below a filter",
                .why = "a unique key against a narrow dimension"},
           // The dimension carries two of the facts' ten keys, so the join discards most rows and
           // the aggregate would read far more below it than it reads now.
           Case{.sql = "SELECT narrow.dk, SUM(facts.v) FROM facts, narrow "
                       "WHERE facts.few = narrow.dk GROUP BY narrow.dk",
                .verdict = "below a filter",
                .why = "the join above keeps a fifth of the keys"},
           // No statistics on the pre-grouping column, so nothing estimates the groups. An unknown
           // is a refusal: a *small* group count is what argues for the rewrite, so an unknown one
           // must argue against it.
           Case{.sql = "SELECT wide.dk, SUM(facts.v) FROM facts, wide WHERE facts.plain = wide.dk "
                       "GROUP BY wide.dk",
                .verdict = "unknown",
                .why = "the column declares no distinct count"},
       }) {
    EXPECT_EQ(Verdicts(c.sql), (std::vector<std::string>{std::string(c.verdict)}))
        << c.why << ": " << c.sql;
  }
}

// How many grouped aggregates the optimized plan holds: one where the exchange did not fire, two
// where it did -- the partial on the aggregated side and the original left above the join.
int Aggregates(std::string_view sql) {
  static const Catalog catalog = GateCatalog();
  auto plan = BindSql(sql, catalog);
  EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
  if (!plan.ok()) {
    return -1;
  }
  int count = 0;
  const std::string text = Explain(Optimized(*plan));
  for (std::size_t at = text.find("GroupAggregate"); at != std::string::npos;
       at = text.find("GroupAggregate", at + 1)) {
    ++count;
  }
  return count;
}

// The exchange performed: the admitted placement of the gate's own fixture gains an aggregate.
TEST(AggregatePlacementTest, PerformsThePlacementTheGateAdmits) {
  EXPECT_EQ(Aggregates("SELECT wide.dk, SUM(facts.v) FROM facts, wide WHERE facts.few = wide.dk "
                       "GROUP BY wide.dk"),
            2);
  // And the partial groups the aggregated side by the join key, with the original left above it.
  static const Catalog catalog = GateCatalog();
  auto plan = BindSql(
      "SELECT wide.dk, SUM(facts.v) FROM facts, wide WHERE facts.few = wide.dk "
      "GROUP BY wide.dk",
      catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const std::string text = Explain(Optimized(*plan));
  EXPECT_NE(text.find("GroupAggregate keys=[facts.few] SUM(facts.v)"), std::string::npos) << text;
  EXPECT_NE(text.find("GroupAggregate keys=[wide.dk] SUM("), std::string::npos) << text;
}

// A count exchanges through the kind that merges partial counts: COUNT below, SUM_COUNTS above.
// Its result stays BIGINT, which is what the query asked for and what a SUM of integers would not
// have given.
TEST(AggregatePlacementTest, ACountExchangesThroughTheKindThatMergesCounts) {
  static const Catalog catalog = GateCatalog();
  auto plan = BindSql(
      "SELECT wide.dk, COUNT(facts.v) FROM facts, wide WHERE facts.few = wide.dk "
      "GROUP BY wide.dk",
      catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const std::string text = Explain(Optimized(*plan));
  EXPECT_NE(text.find("GroupAggregate keys=[facts.few] COUNT(facts.v)"), std::string::npos) << text;
  EXPECT_NE(text.find("SUM_COUNTS("), std::string::npos) << text;
  EXPECT_NE(text.find("count(facts.v):BIGINT"), std::string::npos)
      << "the result type is the one the query asked for\n"
      << text;
}

// Everything the exchange declines to build, each leaving one aggregate where the query wrote one.
TEST(AggregatePlacementTest, DeclinesToBuild) {
  struct Case {
    std::string_view sql;
    std::string_view why;
  };
  for (const Case& c : {
           // COUNT(*) where the grouped side can go missing: a missing partial counted nothing,
           // which is COUNT(col)'s answer and not COUNT(*)'s -- that one counted the padded row.
           Case{.sql = "SELECT wide.dk, SUM(facts.v), COUNT(*) FROM wide LEFT JOIN facts "
                       "ON wide.dk = facts.few GROUP BY wide.dk",
                .why = "COUNT(*) over a side that can be missing"},
           // AVG, whose partial is a (SUM, COUNT) pair and whose final divides.
           Case{.sql = "SELECT wide.dk, AVG(facts.v) FROM facts, wide WHERE facts.few = wide.dk "
                       "GROUP BY wide.dk",
                .why = "AVG changes the node's arity"},
           // A placement the gate refuses is not performed however buildable it is.
           Case{.sql = "SELECT twin.dk, SUM(facts.v) FROM facts, twin WHERE facts.many = twin.dk "
                       "GROUP BY twin.dk",
                .why = "the gate refuses it"},
       }) {
    EXPECT_EQ(Aggregates(c.sql), 1) << c.why << ": " << c.sql;
  }
}

// The chain, which is the query this walk was added for in miniature. The deeper placement is the
// one reported: its pre-grouping is the join key alone, where the nearest one's holds a grouping
// column as well and nothing estimates its groups. The three VARCHARs the query groups by come from
// `top`, which the candidate is not below at all -- so they are simply not pre-grouped by, and the
// answer still holds because the final step groups again.
TEST(AggregatePlacementTest, ReportsTheDeeperPlacementOfAChain) {
  EXPECT_EQ(GatePlacements(kChain), (std::vector<std::string>{"left 1+0 2 down"}));
  EXPECT_EQ(Verdicts(kChain), (std::vector<std::string>{"worth, a narrower pre-grouping"}));
  // The exchange performed: the partial two joins down, and what is LEFT reported above it -- the
  // nearest placement, whose group count is unknown because `mid.g` declares no distinct count.
  const std::string text = GatePlan(kChain);
  EXPECT_NE(text.find("GroupAggregate keys=[deep.mk] SUM(deep.v)"), std::string::npos) << text;
  EXPECT_NE(text.find("1 from the grouping set, unknown"), std::string::npos) << text;
}

// The narrowing is what carries that placement, and it is the one condition of the gate not founded
// on an estimate -- so the same chain with a *wide* pre-grouping, at the same reduction, is
// refused. Grouping by `deep.dw` as well puts a VARCHAR in the pre-grouping, and the ratio falls
// from 32 to a little over 4 while the reduction stays under the bar. The two cases bracket
// kLeastNarrowing, so moving it outside [5, 32] fails one of them.
TEST(AggregatePlacementTest, AWidePreGroupingDoesNotEarnTheSamePlacement) {
  static constexpr std::string_view kWide =
      "SELECT top.w1, top.w2, top.w3, mid.g, deep.dw, SUM(deep.v) FROM deep, mid, top "
      "WHERE deep.mk = mid.mk AND mid.tk = top.tk GROUP BY top.w1, top.w2, top.w3, mid.g, deep.dw";
  // Nothing is reported two joins down, and nothing is built there: the only placement left is the
  // nearest, whose groups are unknown for the reason the chain case gives.
  EXPECT_EQ(GatePlacements(kWide), (std::vector<std::string>{"left 3+2"}));
  EXPECT_EQ(Aggregates(kWide), 1);
}

// An outer join between the aggregate and a deeper candidate stops the walk: it pads rows no
// partial below it ever saw, which `needs_identity` does not describe. The nearest placement is
// still reported, so what this pins is that the walk does not travel THROUGH the outer join.
TEST(AggregatePlacementTest, AnOuterJoinOnThePathStopsTheWalk) {
  // The full vector, not a scan of it: a scan holds under any mutation that reports nothing at all.
  EXPECT_EQ(GatePlacements("SELECT top.w1, top.w2, top.w3, SUM(deep.v) FROM deep JOIN mid "
                           "ON deep.mk = mid.mk LEFT JOIN top ON mid.tk = top.tk "
                           "GROUP BY top.w1, top.w2, top.w3"),
            (std::vector<std::string>{"left 1+0"}));
}

// Where the deepest admitted placement declines to build, a nearer admitted one is still performed.
// Reporting one placement per aggregate is an EXPLAIN constraint, not a licence to lose a rewrite:
// before the fallback, the deepest candidate's refusal left the plan untouched.
TEST(AggregatePlacementTest, ANearerPlacementIsBuiltWhenTheDeepestDeclines) {
  // `top.tk = deep.ak` reads a column of the aggregated side the pre-grouping cannot hold, so the
  // two-joins-down candidate is admitted and then declined by the rebuild.
  static constexpr std::string_view kDeclines =
      "SELECT top.w1, top.w2, top.w3, mid.g, SUM(deep.v) FROM deep, mid, top "
      "WHERE deep.mk = mid.mk AND top.tk = deep.ak GROUP BY top.w1, top.w2, top.w3, mid.g";
  EXPECT_EQ(GatePlacements(kDeclines), (std::vector<std::string>{"left 1+0 2 down"}));
  EXPECT_EQ(Aggregates(kDeclines), 1);  // the nearest is not admitted here, so nothing is built
}

// The aggregate's argument is computed above both joins, so the Compute that defines it has to
// travel down with the partial -- a partial cannot aggregate a column that does not exist below it.
// The Compute lands on the aggregated side, under the partial, and nothing above reads its output
// any more because the final call reads the partial's.
TEST(AggregatePlacementTest, AComputedArgumentTravelsDownWithThePartial) {
  const std::string text = GatePlan(
      "SELECT top.w1, top.w2, top.w3, mid.g, SUM(deep.v * 2) FROM deep, mid, top "
      "WHERE deep.mk = mid.mk AND mid.tk = top.tk GROUP BY top.w1, top.w2, top.w3, mid.g");
  EXPECT_NE(text.find("GroupAggregate keys=[deep.mk] SUM("), std::string::npos) << text;
  // The Compute below the partial, over the scan, and not above the join it came from.
  const std::size_t partial = text.find("GroupAggregate keys=[deep.mk]");
  const std::size_t compute = text.find("Compute");
  ASSERT_NE(partial, std::string::npos) << text;
  ASSERT_NE(compute, std::string::npos) << text;
  EXPECT_GT(compute, partial) << text;
}

// A join ABOVE the candidate whose key reads the pre-grouping column. The partial defines a fresh
// id for each key it groups by, so every node on the path that read the old one has to be pointed
// at the new -- not only the candidate's own keys, which is all the single-join exchange had to do.
TEST(AggregatePlacementTest, AnInterveningJoinKeyIsRenamed) {
  static constexpr std::string_view kShared =
      "SELECT top.w1, top.w2, top.w3, mid.g, SUM(deep.v) FROM deep, mid, top "
      "WHERE deep.mk = mid.mk AND top.tk = deep.mk GROUP BY top.w1, top.w2, top.w3, mid.g";
  // The premise first -- a placement two joins down, actually built -- because without it this case
  // would pass while exercising none of the rebuild's renaming.
  EXPECT_EQ(GatePlacements(kShared), (std::vector<std::string>{"left 1+0 2 down"}));
  EXPECT_EQ(Aggregates(kShared), 2);
  // And then: ResolvePositions rejects a plan whose node reads an id its input does not carry, so a
  // missed rename aborts here rather than answering wrongly.
  EXPECT_FALSE(GatePlan(kShared).empty());
}

// A join above the candidate keying on a column of the aggregated side that the pre-grouping does
// NOT hold. The partial outputs its keys and its calls and nothing else, so that column is gone
// above it -- and condition 3 does not put it in the pre-grouping, since it is neither a key of the
// candidate nor a grouping column. Refused, because the alternative is a plan whose join reads an
// id its input does not carry: ResolvePositions aborts on that, so without this the shape is a
// crash rather than a wrong answer.
TEST(AggregatePlacementTest, AJoinAboveReadingADroppedColumnIsRefused) {
  static constexpr std::string_view kDropped =
      "SELECT top.w1, top.w2, top.w3, mid.g, SUM(deep.v) FROM deep, mid, top "
      "WHERE deep.mk = mid.mk AND top.tk = deep.ak GROUP BY top.w1, top.w2, top.w3, mid.g";
  EXPECT_FALSE(GatePlan(kDropped).empty());
  EXPECT_EQ(Aggregates(kDropped), 1);
}

// A placement the rewrite declines mints no column id. It matters because the caller falls back to
// a nearer placement when the deepest declines: ids burned on the way are gaps in a counter every
// later pass keeps extending, and the only way to keep that honest is to put every refusal before
// the first `Mint`.
TEST(AggregatePlacementTest, ADeclinedPlacementMintsNoId) {
  static const Catalog catalog = GateCatalog();
  struct Case {
    std::string_view sql;
    bool builds;
    std::string_view why;
  };
  for (const Case& c : {
           // Admitted two joins down, then declined by the rebuild: the join above reads a column
           // of the aggregated side the pre-grouping cannot hold.
           Case{.sql = "SELECT top.w1, top.w2, top.w3, mid.g, SUM(deep.v) FROM deep, mid, top "
                       "WHERE deep.mk = mid.mk AND top.tk = deep.ak "
                       "GROUP BY top.w1, top.w2, top.w3, mid.g",
                .builds = false,
                .why = "the rebuild declines it"},
           // The control: the same chain without that join, which IS built, so the case above is
           // not passing merely because nothing was ever admitted.
           Case{.sql = kChain, .builds = true, .why = "the chain is built"},
       }) {
    auto plan = BindSql(c.sql, catalog);
    ASSERT_TRUE(plan.ok()) << c.sql << ": " << plan.status().ToString();
    ColumnId last_id = MaxColumnId(*plan->root);
    const ColumnId before = last_id;
    const LogicalNodePtr out = ExchangeAggregates(plan->root, last_id);
    EXPECT_EQ(out != plan->root, c.builds) << c.why << ": " << c.sql;
    if (c.builds) {
      EXPECT_NE(last_id, before) << c.why;
    } else {
      EXPECT_EQ(last_id, before) << c.why;
    }
  }
}

// What the cost model actually claims, pinned on the only thing it can be pinned on: that a VARCHAR
// grouping column costs much more than a fixed-width one. Two VARCHARs are enough to make a
// one-INTEGER pre-grouping worth taking at a reduction the row test refuses; two INTEGERs are not.
// Without this pair the VARCHAR weight is free to fall to a BIGINT's and every other case still
// passes -- the threshold is bracketed by the two cases above, but the weights were not. It pins
// the weight STRICTLY ABOVE a fixed-width type's rather than at its exact value, which is the claim
// the model makes and all it should be held to.
TEST(AggregatePlacementTest, AVarcharGroupingColumnCostsMoreThanAFixedWidthOne) {
  EXPECT_EQ(GatePlacements("SELECT top.w1, mid.g, SUM(deep.v) FROM deep, mid, top "
                           "WHERE deep.mk = mid.mk AND mid.tk = top.tk GROUP BY top.w1, mid.g"),
            (std::vector<std::string>{"left 1+0 2 down"}));
  EXPECT_EQ(Verdicts("SELECT top.w1, mid.g, SUM(deep.v) FROM deep, mid, top "
                     "WHERE deep.mk = mid.mk AND mid.tk = top.tk GROUP BY top.w1, mid.g"),
            (std::vector<std::string>{"worth, a narrower pre-grouping"}));
  // The same chain and the same reduction, grouped by two fixed-width columns instead: nothing is
  // narrow enough to carry it, and nothing is built.
  static constexpr std::string_view kFixed =
      "SELECT top.tk, mid.tk, SUM(deep.v) FROM deep, mid, top "
      "WHERE deep.mk = mid.mk AND mid.tk = top.tk GROUP BY top.tk, mid.tk";
  EXPECT_EQ(Aggregates(kFixed), 1);
}

// Every refusal. Each reports nothing, and reporting nothing is always safe -- but
// each is here because it is a shape that would otherwise look legal.
TEST(AggregatePlacementTest, Refusals) {
  struct Case {
    std::string_view sql;
    std::string_view why;
  };
  for (const Case& c : {
           // An aggregate reading both inputs cannot be computed on either alone (condition 1).
           Case{.sql = "SELECT t.i16, SUM(t.i32 + ok.i16) FROM t, ok WHERE t.i32 = ok.i16 "
                       "GROUP BY t.i16",
                .why = "the aggregate's expression mixes the two inputs"},
           // Two aggregates, one per input: no single side carries both (condition 1).
           Case{.sql = "SELECT t.i16, SUM(t.i32), SUM(ok.i16) FROM t, ok WHERE t.i32 = ok.i16 "
                       "GROUP BY t.i16",
                .why = "the calls read different inputs"},
           // COUNT(DISTINCT) does not decompose unless the distinct column enters the
           // pre-grouping, which would make the pre-grouping a key of its side (condition 2).
           Case{.sql = "SELECT t.i16, COUNT(DISTINCT ok.i16) FROM t, ok WHERE t.i32 = ok.i16 "
                       "GROUP BY t.i16",
                .why = "COUNT(DISTINCT) is not decomposable"},
           // Nothing names a side: both inputs would be legal and this pass does not choose.
           Case{.sql = "SELECT t.i16, COUNT(*) FROM t, ok WHERE t.i32 = ok.i16 GROUP BY t.i16",
                .why = "every call is COUNT(*)"},
           // A global aggregate: a bound one is an AggregateNode and not a grouping at all, so
           // this reaches nothing -- kept because it is the shape a reader will ask about.
           Case{.sql = "SELECT SUM(ok.i16) FROM t, ok WHERE t.i32 = ok.i16", .why = "no GROUP BY"},
           // A semi join, whose left side a placement WOULD be legal on: refused for now, and the
           // refusal is pinned so that admitting it is a visible change rather than a silent one.
           Case{.sql = "SELECT t.i16, SUM(t.i32) FROM t WHERE t.i32 IN (SELECT i16 FROM ok) "
                       "GROUP BY t.i16",
                .why = "a semi join is refused conservatively"},
           // A residual decides which pairs match on something other than the keys, and the
           // pre-grouping destroys the per-row values it reads (condition 4).
           Case{.sql =
                    "SELECT t.i16, SUM(ok.i16) FROM t, ok WHERE t.i32 = ok.i16 AND t.i64 > ok.i16 "
                    "GROUP BY t.i16",
                .why = "the join carries a residual"},
           // A grouping column reading both inputs separates rows no pre-grouping of one input can.
           Case{.sql = "SELECT t.i32 + ok.i16, SUM(ok.i16) FROM t, ok WHERE t.i32 = ok.i16 "
                       "GROUP BY t.i32 + ok.i16",
                .why = "the grouping column reads both inputs"},
           // A Filter and a Project between the aggregate and the join. Both are refused, and the
           // pair is what a bound plan produces: a conjunct over one relation is pushed BELOW the
           // join, so a Filter reaches this position only above a derived table's Project. That is
           // why no case here isolates the Filter -- nothing produces one on its own.
           Case{.sql = "SELECT d.a, SUM(d.b) FROM (SELECT t.i16 AS a, ok.i16 AS b FROM t, ok "
                       "WHERE t.i32 = ok.i16) AS d WHERE d.b > 2 GROUP BY d.a",
                .why = "a Filter and a Project sit between the aggregate and the join"},
           // One input, so there is no join to exchange across.
           Case{.sql = "SELECT i16, SUM(i32) FROM t GROUP BY i16", .why = "no join"},
       }) {
    EXPECT_EQ(Placements(c.sql), (std::vector<std::string>{})) << c.why << ": " << c.sql;
  }
}

}  // namespace
}  // namespace antb1::plan
