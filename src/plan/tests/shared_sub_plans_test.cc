#include "antb1/plan/shared_sub_plans.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <arrow/api.h>
#include <gtest/gtest.h>

#include "antb1/plan/catalog.h"
#include "antb1/plan/explain.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/table.h"
#include "antb1/plan/types.h"

#include "../predicate_domain.h"
#include "plan_test_util.h"

namespace antb1::plan {
namespace {

using testing::BindSql;
using testing::MakeCatalog;

LogicalPlan Planned(std::string_view sql) {
  static const Catalog catalog = MakeCatalog();
  auto plan = BindSql(sql, catalog);
  EXPECT_TRUE(plan.ok()) << plan.status().ToString();
  return plan.ok() ? Optimized(*plan) : LogicalPlan{};
}

// Which node heads each group, with how many members and how many fields: "Join 2x3". The head is
// what matters as much as the count -- two references to one table almost always repeat their
// *scan*, so a test that only counted groups would pass while the node it cares about went
// unshared.
std::vector<std::string> GroupsOf(const LogicalPlan& plan) {
  std::vector<std::string> out;
  for (const SharedSubPlan& shared : SharedSubPlans(plan)) {
    const std::string line = ExplainNode(*shared.members.front());
    out.push_back(line.substr(0, line.find(' ')) + " " + std::to_string(shared.members.size()) +
                  "x" + std::to_string(shared.fields));
  }
  return out;
}

std::vector<std::string> Groups(std::string_view sql) { return GroupsOf(Planned(sql)); }

// A CTE referenced twice is bound twice (ADR 0022), so the plan holds the subtree twice and this
// finds it, outermost node first. The shape ADR 0030 is built around.
TEST(SharedSubPlansTest, FindsTheSubPlanTwoCteReferencesRepeat) {
  EXPECT_EQ(Groups("WITH g AS (SELECT i64 AS k, COUNT(*) AS n FROM t GROUP BY i64) "
                   "SELECT COUNT(*) FROM g AS a, g AS b WHERE a.k = b.k"),
            (std::vector<std::string>{"Project 2x1"}))
      << "the Project heads it, not the Scan or the GroupAggregate inside it";
}

// The case exact structural equality misses, and the one this ADR was amended for: two sub-plans
// whose scans read different fields. Selecting fields does not change which rows a scan returns, so
// they compute the same rows and the shared one reads the union.
//
// `a.j` is used by the outer query and `b` has no such column, so projection pruning leaves a's
// scan of t reading two fields and b's reading one -- the difference is driven from *outside* the
// repeated sub-plan, which is exactly how it arises in the queries the ADR measures.
TEST(SharedSubPlansTest, FindsASubPlanWhoseScansReadDifferentFields) {
  EXPECT_EQ(Groups("SELECT COUNT(*) FROM "
                   "(SELECT t.i64 AS k, t.i32 AS j FROM t, u WHERE t.i64 = u.i64) AS a, "
                   "(SELECT t.i64 AS k FROM t, u WHERE t.i64 = u.i64) AS b "
                   "WHERE a.k = b.k AND a.j > 0"),
            (std::vector<std::string>{"Join 2x3"}))
      << "the join is shared though one side's scan of t reads a field the other's does not: "
         "two fields of t and one of u";
}

// The half that keeps answers right. In each of these the nodes that differ must not be shared --
// but the scans below them still repeat, and reporting those is correct, so what is asserted is
// *which* node heads the group rather than that there is none.
TEST(SharedSubPlansTest, SharesNoNodeWhoseContentDiffers) {
  EXPECT_EQ(Groups("SELECT COUNT(*) FROM (SELECT i64 FROM t WHERE i32 > 0) AS a, "
                   "(SELECT i64 FROM t WHERE i32 > 1) AS b WHERE a.i64 = b.i64"),
            (std::vector<std::string>{"Scan 2x2"}))
      << "a different constant: the filters are not shared, the scans under them are";
  EXPECT_TRUE(Groups("SELECT COUNT(*) FROM (SELECT i64 FROM t WHERE i32 > 0) AS a, "
                     "(SELECT i64 FROM t WHERE i16 > 0) AS b WHERE a.i64 = b.i64")
                  .empty())
      << "a different column, and nothing at all is reported: the filters differ, and the scans "
         "below them read {i64, i32} and {i64, i16} -- overlapping but neither inside the other, "
         "so no one member's result could serve both and the group is not reported";
  EXPECT_TRUE(Groups("SELECT COUNT(*) FROM t AS a, u AS b WHERE a.i64 = b.i64").empty())
      << "a different table shares nothing at all";
  // A DECIMAL constant against the column's own type and a DOUBLE one, which ADR 0021 rule 11
  // compares differently. These two are told apart by their text alone (ToString renders them
  // "1.50" and "1.5"), so this pins the behaviour without exercising ConstantKey's type component
  // -- see the comment on ConstantKey for why that component is there regardless. No SQL of this
  // subset spells one literal at two types, so the case it guards is not reachable to write a test
  // for: a predicate's constant is typed as its column, and an integer literal has one type.
  EXPECT_EQ(Groups("SELECT COUNT(*) FROM (SELECT p FROM dec WHERE p > 1.5) AS a, "
                   "(SELECT p FROM dec WHERE p > 1.5e0) AS b WHERE a.p = b.p"),
            (std::vector<std::string>{"Scan 2x1"}))
      << "two constants of different types must not be shared";
  // Casts of one column to DECIMALs that differ only in scale or width compute different values
  // (ADR 0037's CT2 rounds); an expression's key is its full type, not the type's kind.
  EXPECT_EQ(
      Groups("SELECT COUNT(*) FROM "
             "(SELECT CAST(p AS DECIMAL(15,1)) AS x, COUNT(*) AS n FROM dec GROUP BY 1) AS a, "
             "(SELECT CAST(p AS DECIMAL(15,0)) AS x, COUNT(*) AS n FROM dec GROUP BY 1) AS b "
             "WHERE a.n = b.n"),
      (std::vector<std::string>{"Scan 2x1"}))
      << "casts to DECIMAL(15,1) and DECIMAL(15,0) must not be shared";
  EXPECT_EQ(
      Groups("SELECT COUNT(*) FROM "
             "(SELECT CAST(p AS DECIMAL(16,2)) AS x, COUNT(*) AS n FROM dec GROUP BY 1) AS a, "
             "(SELECT CAST(p AS DECIMAL(17,2)) AS x, COUNT(*) AS n FROM dec GROUP BY 1) AS b "
             "WHERE a.n = b.n"),
      (std::vector<std::string>{"Scan 2x1"}))
      << "nor casts to DECIMAL(16,2) and DECIMAL(17,2)";
}

// An ordered or limited sub-plan is never a member: a shared result cannot hand two consumers an
// order without deciding whose wins. The scans below them repeat and are reported, which is the
// point -- refusing the Sort costs the report nothing it could have used.
TEST(SharedSubPlansTest, SharesNoOrderedOrLimitedSubPlan) {
  const std::vector<std::string> groups = Groups(
      "SELECT COUNT(*) FROM (SELECT i64 FROM t ORDER BY i64 LIMIT 5) AS a, "
      "(SELECT i64 FROM t ORDER BY i64 LIMIT 5) AS b WHERE a.i64 = b.i64");
  EXPECT_EQ(groups, (std::vector<std::string>{"Scan 2x1"}));
  for (const std::string& group : groups) {
    EXPECT_EQ(group.find("Sort"), std::string::npos);
    EXPECT_EQ(group.find("Limit"), std::string::npos);
  }
}

// Three references give one group of three, not three groups of two.
TEST(SharedSubPlansTest, GroupsEveryMemberTogether) {
  EXPECT_EQ(Groups("WITH g AS (SELECT i64 AS k, COUNT(*) AS n FROM t GROUP BY i64) "
                   "SELECT COUNT(*) FROM g AS a, g AS b, g AS c "
                   "WHERE a.k = b.k AND b.k = c.k"),
            (std::vector<std::string>{"Project 3x1"}));
}

TEST(SharedSubPlansTest, CostsARepeatedIntersectionAgainstItsFullInput) {
  const auto schema = arrow::schema({arrow::field("k", arrow::int64())});
  const auto small = std::make_shared<testing::FakeTable>(schema, 11'000'000);
  for (const int64_t large_rows : {int64_t{100'000'000}, std::numeric_limits<int64_t>::max()}) {
    SCOPED_TRACE(large_rows);
    Catalog catalog;
    ASSERT_TRUE(catalog.Register("s", small).ok());
    ASSERT_TRUE(
        catalog.Register("l", std::make_shared<testing::FakeTable>(schema, large_rows)).ok());
    const auto bound = BindSql(
        "WITH common_keys AS (SELECT k FROM s INTERSECT SELECT k FROM l) "
        "SELECT a.k FROM common_keys AS a JOIN common_keys AS b ON a.k = b.k",
        catalog);
    ASSERT_TRUE(bound.ok()) << bound.status();
    const auto groups = MaterializableSubPlans(Optimized(*bound));
    ASSERT_EQ(groups.size(), 1U) << "the 11-million-row bound fits for one BIGINT column";
    EXPECT_EQ(groups.front().members.size(), 2U);
  }
}

std::string BandCount(std::string_view predicate, std::string_view key = "p.band",
                      std::string_view aggregate = "COUNT(*)",
                      std::string_view from = "packets p") {
  return std::format(
      "SELECT p.slot, {} AS band_key, {} AS n FROM {} WHERE {} "
      "GROUP BY p.slot, {}",
      key, aggregate, from, predicate, key);
}

class SharedSubPlansFilteredGroupsTest : public ::testing::Test {
 protected:
  static constexpr int64_t kRows = 100'000'000;

  void SetUp() override {
    const auto schema = arrow::schema(
        {arrow::field("slot", arrow::int64()), arrow::field("band", arrow::int64()),
         arrow::field("voltage", arrow::float64()), arrow::field("label", arrow::binary())});
    packets_ = std::make_shared<testing::FakeTable>(schema, kRows);
    packets_->WithStats(0, 0, PartStats{.min = 1, .max = 100, .rows = kRows});
    ASSERT_TRUE(catalog_.Register("packets", packets_).ok());
  }

  std::vector<SharedSubPlan> Combined(std::string_view sql, bool enabled = true) const {
    const auto bound = BindSql(sql, catalog_);
    EXPECT_TRUE(bound.ok()) << bound.status() << " with " << sql;
    if (!bound.ok()) {
      return {};
    }
    auto groups = MaterializableSubPlans(*bound, enabled);
    std::erase_if(groups, [](const auto& group) { return group.combined_producer == nullptr; });
    return groups;
  }

  Catalog catalog_;
  std::shared_ptr<testing::FakeTable> packets_;
};

TEST_F(SharedSubPlansFilteredGroupsTest, MapsEachReaderToItsOwnOutputKeyAndLiteral) {
  const auto query = BandCount("p.band = 7") + " UNION ALL " + BandCount("p.band = 3") +
                     " UNION ALL " + BandCount("p.band = 5");
  EXPECT_TRUE(Combined(query, /*enabled=*/false).empty());
  const auto groups = Combined(query);
  ASSERT_EQ(groups.size(), 1U);
  const auto& group = groups.front();
  ASSERT_EQ(group.members.size(), 3U);
  ASSERT_EQ(group.reader_predicates.size(), group.members.size());
  const auto& producer = std::get<GroupAggregateNode>(*group.combined_producer);
  const auto* input = std::get_if<FilterNode>(producer.input.get());
  ASSERT_NE(input, nullptr);
  ASSERT_EQ(input->predicates.size(), 1U);
  const auto& combined = input->predicates.front();
  ASSERT_EQ(combined.kind, Predicate::Kind::kIn);
  EXPECT_EQ(combined.column.value_or(BoundColumn{}).id, producer.keys.at(1).id);
  std::vector<std::string> values;
  values.reserve(combined.values.size());
  for (const auto& value : combined.values) {
    values.push_back(ToString(value));
  }
  EXPECT_EQ(values, (std::vector<std::string>{"3", "5", "7"}));
  values.clear();
  for (std::size_t i = 0; i < group.members.size(); ++i) {
    const auto& reader = group.reader_predicates[i];
    ASSERT_TRUE(IsIntegerOrDateEquality(reader));
    const auto& member = std::get<GroupAggregateNode>(*group.members[i]);
    const auto column = reader.column.value_or(BoundColumn{});
    EXPECT_EQ(column.index, 1);
    EXPECT_EQ(column.id, member.key_ids.at(1));
    EXPECT_NE(column.id, member.keys.at(1).id);
    EXPECT_EQ(group.columns.at(i), (std::vector<int>{0, 1, 2}));
    values.push_back(ToString(reader.constant));
  }
  EXPECT_EQ(values, (std::vector<std::string>{"7", "3", "5"}));
}

TEST_F(SharedSubPlansFilteredGroupsTest, BoundsTheCombinedKeysIncludingOneNullableGroup) {
  const auto query = BandCount("p.band = 1") + " UNION ALL " + BandCount("p.band = 2");
  // Two band values and three BIGINT outputs, including validity for each. The additional
  // NULL slot can occur once in each band; no NULL band can pass the combined predicate.
  constexpr int64_t kSlots = kMaxSharedResultBytes / (int64_t{2} * 27);
  packets_->WithStats(0, 0, PartStats{.min = 1, .max = kSlots, .rows = kRows});
  EXPECT_EQ(Combined(query).size(), 1U);
  packets_->WithStats(0, 0, PartStats{.min = 1, .max = kSlots, .null_count = 1, .rows = kRows});
  EXPECT_TRUE(Combined(query).empty());
}

TEST_F(SharedSubPlansFilteredGroupsTest, KeepsTheDateTypeAndOtherFixedKeyForEachAggregate) {
  const auto days = std::make_shared<testing::FakeTable>(
      arrow::schema({arrow::field("slot", arrow::int64()), arrow::field("band", arrow::date32())}),
      kRows);
  days->WithStats(0, 0, PartStats{.min = 1, .max = 10, .rows = kRows});
  ASSERT_TRUE(catalog_.Register("days", days).ok());
  for (const std::string_view aggregate : {"COUNT(p.slot)", "MIN(p.slot)", "MAX(p.slot)"}) {
    SCOPED_TRACE(aggregate);
    const auto query =
        BandCount("p.slot=1 AND p.band=DATE '2025-01-03'", "p.band", aggregate, "days p") +
        " UNION ALL " +
        BandCount("p.slot=1 AND p.band=DATE '2025-01-07'", "p.band", aggregate, "days p");
    const auto groups = Combined(query);
    ASSERT_EQ(groups.size(), 1U);
    const auto& group = groups.front();
    const auto& input =
        std::get<FilterNode>(*std::get<GroupAggregateNode>(*group.combined_producer).input);
    ASSERT_EQ(input.predicates.size(), 2U);
    EXPECT_EQ(ToString(input.predicates[0].constant), "1");
    ASSERT_EQ(input.predicates[1].kind, Predicate::Kind::kIn);
    ASSERT_EQ(input.predicates[1].values.size(), 2U);
    for (const auto& value : input.predicates[1].values) {
      EXPECT_EQ(value.type, LogicalType::kDate);
    }
    ASSERT_EQ(group.reader_predicates.size(), 2U);
    EXPECT_EQ(ToString(group.reader_predicates[0].constant), "DATE '2025-01-03'");
    EXPECT_EQ(ToString(group.reader_predicates[1].constant), "DATE '2025-01-07'");
  }
}

TEST_F(SharedSubPlansFilteredGroupsTest, RequiresEnoughInputToAmortizeACombinedBuild) {
  for (const int64_t rows : {9'999'999, 10'000'000}) {
    const auto source = std::make_shared<testing::FakeTable>(packets_->schema(), rows);
    source->WithStats(0, 0, PartStats{.min = 1, .max = 100, .rows = rows});
    const auto name = std::format("source_{}", rows);
    ASSERT_TRUE(catalog_.Register(name, source).ok());
    const auto from = name + " p";
    const auto query = BandCount("p.band=1", "p.band", "COUNT(*)", from) + " UNION ALL " +
                       BandCount("p.band=2", "p.band", "COUNT(*)", from);
    EXPECT_EQ(Combined(query).size(), rows == 10'000'000 ? 1U : 0U);
  }
}

TEST_F(SharedSubPlansFilteredGroupsTest, RequiresOverlappingPrunedPartsOfADirectScan) {
  constexpr int64_t kPartRows = kRows / 4;
  packets_->WithParts({kPartRows, kPartRows, kPartRows, kPartRows});
  for (int part = 0; part < 4; ++part) {
    packets_->WithStats(part, 0, {.min = 1, .max = 100, .rows = kPartRows});
  }
  packets_->WithStats(0, 1, {.min = 0, .max = 1, .rows = kPartRows})
      .WithStats(1, 1, {.min = 1, .max = 2, .rows = kPartRows})
      .WithStats(2, 1, {.null_count = kPartRows, .rows = kPartRows});
  const auto a = BandCount("p.band=0");
  const auto query = a + " UNION ALL " + BandCount("p.band=2");
  // Missing stats on the last part leave it eligible for both readers.
  EXPECT_EQ(Combined(query).size(), 1U);
  packets_->WithStats(3, 1, {.min = 5, .max = 6, .rows = kPartRows});
  EXPECT_TRUE(Combined(query).empty());
  // Distinct projections keep the two readers from sharing a larger exact subtree first.
  const auto reversed = [](int band) {
    return std::format("SELECT band,slot,COUNT(*) FROM packets WHERE band={} GROUP BY slot,band",
                       band);
  };
  EXPECT_EQ(Combined(query + " UNION ALL " + reversed(0)).size(), 1U);
  const auto absent = BandCount("p.band=8");
  EXPECT_TRUE(Combined(query + " UNION ALL " + absent + " UNION ALL " + reversed(8)).empty());
  packets_->WithStats(3, 1, {.min = 0, .max = 2, .rows = kPartRows});
  EXPECT_EQ(Combined(query).size(), 1U);
}

TEST_F(SharedSubPlansFilteredGroupsTest, DisjointScanPartsCanStillShareLookupWork) {
  packets_->WithParts({kRows / 2, kRows / 2});
  for (int part = 0; part < 2; ++part) {
    packets_->WithStats(part, 0, {.min = 1, .max = 100, .rows = kRows / 2})
        .WithStats(part, 1, {.min = part, .max = part, .rows = kRows / 2});
  }
  const auto member = [](int band) {
    const auto from = std::format("packets p JOIN packets d ON p.slot=d.slot AND d.band={}", band);
    return BandCount("p.slot IS NOT NULL", "d.band", "COUNT(*)", from);
  };
  EXPECT_EQ(Combined(member(0) + " UNION ALL " + member(1)).size(), 1U);
}

TEST_F(SharedSubPlansFilteredGroupsTest, TracksWhichSideOfALookupChanges) {
  // The lookup stays smaller even after the fact table's equality filter.
  const auto lookup = std::make_shared<testing::FakeTable>(packets_->schema(), 100);
  ASSERT_TRUE(catalog_.Register("lookup", lookup).ok());
  for (const std::string_view band : {"p.band", "d.band"}) {
    const auto member = [&](int value) {
      const auto from =
          std::format("packets p JOIN lookup d ON p.slot=d.slot AND {}={}", band, value);
      return BandCount("p.slot IS NOT NULL", band, "COUNT(*)", from);
    };
    const auto groups = Combined(member(0) + " UNION ALL " + member(1));
    ASSERT_EQ(groups.size(), 1U) << band;
    EXPECT_EQ(groups.front().widens_join_build, band == "d.band") << band;
  }
}

TEST_F(SharedSubPlansFilteredGroupsTest, CapsOtherRetainedKeysAtTheirCommonEquality) {
  const auto readings = std::make_shared<testing::FakeTable>(
      arrow::schema({arrow::field("slot", arrow::int64()), arrow::field("band", arrow::int64()),
                     arrow::field("region", arrow::int64())}),
      kRows);
  readings->WithStats(0, 0, {.min = 1, .max = 257, .rows = kRows})
      .WithStats(0, 2, {.min = 0, .max = kRows, .rows = kRows});
  ASSERT_TRUE(catalog_.Register("readings", readings).ok());
  const auto member = [](int band, std::string_view region) {
    return std::format(
        "SELECT slot,band,region,COUNT(*) FROM readings WHERE band={} {} GROUP BY slot,band,region",
        band, region);
  };
  EXPECT_TRUE(Combined(member(0, "") + " UNION ALL " + member(1, "")).empty());
  const auto shared =
      Combined(member(0, "AND region=0") + " UNION ALL " + member(1, "AND region=0"));
  ASSERT_EQ(shared.size(), 1U);  // at most 257 slots x 2 bands x 1 region
  const auto& group = std::get<GroupAggregateNode>(*shared.front().combined_producer);
  const auto& predicates = std::get<FilterNode>(*group.input).predicates;
  ASSERT_EQ(predicates.size(), 2U);
  EXPECT_EQ(predicates[0].kind, Predicate::Kind::kIn);
  EXPECT_TRUE(IsIntegerOrDateEquality(predicates[1]));
  EXPECT_TRUE(
      Combined(member(0, "AND region=0") + " UNION ALL " + member(1, "AND region=1")).empty());
}

TEST_F(SharedSubPlansFilteredGroupsTest, WidensOnlyAnEqualityOnACommonRetainedKey) {
  const auto refuses = [&](const std::string& first, const std::string& second) {
    const auto query = first + " UNION ALL " + second;
    EXPECT_TRUE(Combined(query).empty()) << query;
  };
  refuses(BandCount("p.band = 1 AND p.slot > 2"), BandCount("p.band = 2 AND p.slot > 3"));
  refuses(BandCount("p.band >= 1"), BandCount("p.band >= 2"));
  refuses(BandCount("p.band = 1", "p.band+1"), BandCount("p.band = 2", "p.band+1"));
  refuses(BandCount("p.band = 1", "p.band", "SUM(p.voltage)"),
          BandCount("p.band = 2", "p.band", "SUM(p.voltage)"));
  for (const std::string_view aggregate : {"MIN(p.label)", "COUNT(DISTINCT p.slot)"}) {
    refuses(BandCount("p.band=1", "p.band", aggregate), BandCount("p.band=2", "p.band", aggregate));
  }
  for (const std::string_view join : {"JOIN", "LEFT JOIN"}) {
    // An unmatched row still forms a NULL band in the outer query, outside either equality.
    const auto branch = [join](int band) {
      const auto from =
          std::format("packets p {} packets d ON p.slot=d.slot AND d.band={}", join, band);
      return BandCount("p.slot IS NOT NULL", "d.band", "COUNT(*)", from);
    };
    const auto query = branch(1) + " UNION ALL " + branch(2);
    EXPECT_EQ(Combined(query).size(), join == "JOIN" ? 1U : 0U) << query;
  }
  constexpr std::string_view kTwoJoins =
      "packets p JOIN packets d ON p.slot=d.slot JOIN packets e ON d.slot=e.slot";
  refuses(BandCount("d.band=1", "d.band", "COUNT(*)", kTwoJoins),
          BandCount("d.band=2", "d.band", "COUNT(*)", kTwoJoins));
}

TEST_F(SharedSubPlansFilteredGroupsTest, RequiresBoundedRawGroupingKeysForEveryMember) {
  // The band equality is a retained raw key in every pair. The remaining key must also have a
  // bounded width and a source table, and there is a bound on the number of keys to cost.
  for (const std::string_view other :
       {"p.voltage", "p.label", "p.slot+1",
        "p.slot, p.slot+1, p.slot+2, p.slot+3, p.slot+4, p.slot+5, p.slot+6, p.slot+7"}) {
    const auto branch = [other](int band) {
      return std::format(
          "SELECT p.band, COUNT(*) AS n FROM packets p WHERE p.band={} "
          "GROUP BY p.band, {}",
          band, other);
    };
    EXPECT_TRUE(Combined(branch(1) + " UNION ALL " + branch(2)).empty()) << other;
  }
  const auto nested = [](int band) {
    return std::format(
        "SELECT q.band, COUNT(*) FROM (SELECT p.band, p.slot, COUNT(*) AS n "
        "FROM packets p GROUP BY p.band,p.slot) AS q WHERE q.band={} "
        "GROUP BY q.band",
        band);
  };
  EXPECT_TRUE(Combined(nested(1) + " UNION ALL " + nested(2)).empty());
}

TEST_F(SharedSubPlansFilteredGroupsTest, UsesPerPartCountsForTheUnrestrictedKey) {
  packets_->WithParts({kRows / 2, kRows / 2});
  for (int part = 0; part < 2; ++part) {
    // The limited key's full domain and the disjoint slot domains together exceed the cap.
    packets_->WithDistinctCount(part, 0, 1'500'000).WithDistinctCount(part, 1, 2'000'000);
  }
  const auto query = BandCount("p.band=1") + " UNION ALL " + BandCount("p.band=2");
  EXPECT_EQ(Combined(query).size(), 1U);
  for (int part = 0; part < 2; ++part) {
    packets_->WithDistinctCount(part, 0, 4'000'000);
  }
  EXPECT_TRUE(Combined(query).empty());
}

TEST_F(SharedSubPlansFilteredGroupsTest, BoundsTheNumberOfDistinctBands) {
  auto query = BandCount("p.band = 1");
  for (int band = 2; band <= 9; ++band) {
    query += " UNION ALL " + BandCount(std::format("p.band = {}", band));
  }
  const auto nine = Combined(query);
  ASSERT_EQ(nine.size(), 1U);
  EXPECT_EQ(nine.front().members.size(), 9U);
  EXPECT_TRUE(Combined(query + " UNION ALL " + BandCount("p.band = 10")).empty());
}

TEST(SharedSubPlansTest, CostsTheJointDomainOfFilteredGroupingKeys) {
  constexpr int64_t kTickets = kMaxSharedResultBytes / (int64_t{5} * 36);
  auto readings =
      std::make_shared<testing::FakeTable>(arrow::schema({arrow::field("ticket", arrow::int64()),
                                                          arrow::field("sensor", arrow::int64())}),
                                           2'000'000'000);
  readings->WithStats(0, 0, PartStats{.min = 1, .max = kTickets, .rows = 2'000'000'000});
  auto sensors = std::make_shared<testing::FakeTable>(
      arrow::schema({arrow::field("id", arrow::int64()), arrow::field("band", arrow::int64()),
                     arrow::field("slot", arrow::int64())}),
      1000);
  sensors->WithStats(0, 0, PartStats{.min = 1, .max = 1000, .rows = 1000})
      .WithStats(0, 1, PartStats{.min = 1, .max = 100, .rows = 1000});
  Catalog catalog;
  ASSERT_TRUE(catalog.Register("readings", readings).ok());
  ASSERT_TRUE(catalog.Register("sensors", sensors).ok());

  // One band admits any of four slots; another admits only one. Five pairs fit the cap
  // with 32 bytes of values and four bytes reserved for validity per group.
  // A sixth pair or one more ticket pushes the result past the admission limit.
  constexpr std::string_view kFivePairs = "d.band = 2 OR (d.band = 4 AND d.slot = 3)";
  constexpr std::string_view kInner = "readings AS r JOIN sensors AS d ON r.sensor = d.id";
  const auto expect_sharing = [&](std::string_view from, std::string_view filter,
                                  int64_t null_slots, bool expected) {
    SCOPED_TRACE(std::format("{} with filter={}, NULL slots={}", from, filter, null_slots));
    sensors->WithStats(0, 2, PartStats{.min = 1, .max = 4, .null_count = null_slots, .rows = 1000});
    const auto bound = BindSql(
        std::format("WITH g AS (SELECT r.ticket, d.band, d.slot, COUNT(*) AS n FROM {} AND ({}) "
                    "GROUP BY r.ticket, d.band, d.slot) "
                    "SELECT a.ticket, a.band, a.slot, a.n, b.n, c.n FROM g AS a "
                    "JOIN g AS b ON a.ticket = b.ticket JOIN g AS c ON a.ticket = c.ticket",
                    from, filter),
        catalog);
    ASSERT_TRUE(bound.ok()) << bound.status().ToString();
    const auto groups = SharedSubPlans(*bound);
    ASSERT_EQ(groups.size(), 1U) << Explain(*bound);
    ASSERT_EQ(groups.front().members.size(), 3U);
    int exhausted = 0;
    EXPECT_FALSE(WorthSharing(groups.front(), exhausted))
        << "the original group estimate exceeds the byte cap";
    EXPECT_EQ(WorthSharing(groups.front()), expected) << Explain(*bound);
  };
  expect_sharing(kInner, kFivePairs, 0, true);
  expect_sharing(kInner, kFivePairs, 1, false);  // NULL in the first arm admits a sixth pair.
  expect_sharing(kInner, "d.band = 2 OR d.slot = 3", 0, false);  // Every band can match.
  expect_sharing("readings AS r LEFT JOIN sensors AS d ON r.sensor = d.id", kFivePairs, 0, false);
  // Padding on a sibling branch can similarly add a NULL ticket.
  expect_sharing(
      "sensors AS p LEFT JOIN readings AS r ON p.id = r.sensor "
      "JOIN sensors AS d ON p.id = d.id",
      kFivePairs, 0, false);
  readings->WithStats(
      0, 0, PartStats{.min = 1, .max = kTickets, .null_count = 10, .rows = 2'000'000'000});
  expect_sharing(kInner, kFivePairs, 0, false);
}

TEST(SharedSubPlansTest, RequiresATableWideBoundForKeysOutsideTheFilter) {
  constexpr int64_t kPartRows = 1'000'000;
  constexpr int kParts = 20;
  auto tickets = std::make_shared<testing::FakeTable>(
      arrow::schema({arrow::field("id", arrow::int64()), arrow::field("label", arrow::int64()),
                     arrow::field("region", arrow::int64())}),
      kPartRows * kParts);
  tickets->WithParts(std::vector<int64_t>(kParts, kPartRows));
  for (int part = 0; part < kParts; ++part) {
    // The twenty parts may have disjoint IDs.
    tickets->WithDistinctCount(part, 0, kPartRows).WithDistinctCount(part, 2, 1);
  }
  auto labels = std::make_shared<testing::FakeTable>(
      arrow::schema({arrow::field("id", arrow::int64()), arrow::field("band", arrow::int64())}),
      20'000'000);
  labels->WithStats(0, 1, PartStats{.min = 1, .max = 100, .rows = 20'000'000});
  Catalog catalog;
  ASSERT_TRUE(catalog.Register("tickets", tickets).ok());
  ASSERT_TRUE(catalog.Register("labels", labels).ok());
  const auto bound = BindSql(
      "WITH g AS (SELECT t.id, t.region, d.band, COUNT(*) AS n FROM tickets t JOIN labels d "
      "ON t.label = d.id AND d.band = 1 GROUP BY t.id, t.region, d.band) "
      "SELECT a.id, a.region, a.band, a.n, b.n FROM g a JOIN g b ON a.id = b.id",
      catalog);
  ASSERT_TRUE(bound.ok()) << bound.status().ToString();
  const auto groups = SharedSubPlans(*bound);
  ASSERT_EQ(groups.size(), 1U);
  int exhausted = 0;
  ASSERT_FALSE(WorthSharing(groups.front(), exhausted)) << "the initial estimate exceeds the cap";
  EXPECT_FALSE(WorthSharing(groups.front()));
  // Each part now contributes at most (150,000 + NULL) IDs times (one region + NULL).
  // Both keys come from the same row, so there are twenty sets of tuples.
  for (int part = 0; part < kParts; ++part) {
    tickets->WithDistinctCount(part, 0, 150'000);
  }
  EXPECT_FALSE(WorthSharing(groups.front(), exhausted));
  EXPECT_TRUE(WorthSharing(groups.front()));
}

TEST(SharedSubPlansTest, ReservesValidityBytesForNullableAggregateResults) {
  constexpr int64_t kRows = 100'000'000;
  constexpr int64_t kGroups = kMaxSharedResultBytes / 32;
  auto table = std::make_shared<testing::FakeTable>(
      arrow::schema({arrow::field("k", arrow::int64()), arrow::field("band", arrow::int64()),
                     arrow::field("v", arrow::int64())}),
      kRows);
  table->WithStats(0, 1, PartStats{.min = 1, .max = 100, .rows = kRows})
      .WithStats(0, 2, PartStats{.min = 1, .max = 10, .null_count = kRows / 2, .rows = kRows});
  Catalog catalog;
  ASSERT_TRUE(catalog.Register("t", table).ok());
  const auto bound = BindSql(
      "WITH g AS (SELECT k, band, MIN(v) AS lo, MAX(v) AS hi FROM t WHERE band = 1 "
      "GROUP BY k, band) SELECT a.k, a.band, a.lo, a.hi, b.lo FROM g a JOIN g b ON a.k = b.k",
      catalog);
  ASSERT_TRUE(bound.ok()) << bound.status().ToString();
  const auto groups = SharedSubPlans(*bound);
  ASSERT_EQ(groups.size(), 1U);
  // The four BIGINT value buffers alone can fill the cap. A group with only NULL v values
  // and another with a value require validity bitmaps for MIN and MAX.
  table->WithStats(0, 0, PartStats{.min = 1, .max = kGroups, .rows = kRows});
  EXPECT_FALSE(WorthSharing(groups.front()));
  table->WithStats(0, 0, PartStats{.min = 1, .max = kGroups / 2, .rows = kRows});
  int exhausted = 0;
  EXPECT_FALSE(WorthSharing(groups.front(), exhausted));
  EXPECT_TRUE(WorthSharing(groups.front())) << "the smaller filtered result fits with validity";
}

TEST(SharedSubPlansTest, RequiresTheWholeDisjunctionWithinThePlanningBudget) {
  const auto catalog = MakeCatalog();
  for (const bool restrict_both_arms : {false, true}) {
    const std::string query =
        "SELECT i64 FROM t WHERE i64 = 17 OR " +
        std::string(restrict_both_arms ? "(i32 IS NULL AND i64 IN (26, 27))" : "i32 IS NULL");
    const auto bound = BindSql(query, catalog);
    ASSERT_TRUE(bound.ok()) << bound.status().ToString();
    const auto* project = std::get_if<ProjectNode>(bound->root.get());
    ASSERT_NE(project, nullptr) << Explain(*bound);
    const auto* filter = std::get_if<FilterNode>(project->input.get());
    ASSERT_NE(filter, nullptr) << Explain(*bound);
    const std::vector keys{
        PredicateDomainKey{.id = project->columns.front().id, .max_values = std::nullopt}};
    int budget = 2;
    EXPECT_EQ(PredicateTupleBound(*filter, keys, budget), std::nullopt);
    EXPECT_EQ(budget, 0);
    budget = kPredicateDomainNodeBudget;
    EXPECT_EQ(PredicateTupleBound(*filter, keys, budget),
              restrict_both_arms ? std::optional<int64_t>(3) : std::nullopt)
        << Explain(*bound);
  }
}

// A bare repeated scan is reported. Whether it is worth materialising is the cost gate's question,
// and the cost gate is E2's: a pass that reports must not also decide.
TEST(SharedSubPlansTest, ReportsABareRepeatedScan) {
  EXPECT_EQ(Groups("SELECT COUNT(*) FROM t AS a, t AS b WHERE a.i64 = b.i64"),
            (std::vector<std::string>{"Scan 2x1"}));
}

TEST(SharedSubPlansTest, FindsNothingWhereNothingRepeats) {
  EXPECT_TRUE(Groups("SELECT i64 FROM t WHERE i32 > 0").empty());
  EXPECT_TRUE(Groups("SELECT COUNT(*) FROM t").empty());
  EXPECT_TRUE(SharedSubPlans(LogicalPlan{}).empty()) << "and no root at all";
}

// ---- UnionAll (ADR 0036) ----

LogicalNodePtr Node(LogicalNode node) {
  return std::make_shared<const LogicalNode>(std::move(node));
}

// GROUP BY field 2 of `table` with COUNT(*), its columns minted from `first`: a union's branch.
LogicalNodePtr Grouped(const std::string& table, std::uint32_t first) {
  return Node(GroupAggregateNode{
      .input = Node(
          ScanNode{.table = std::make_shared<testing::FakeTable>(testing::AllTypesSchema(), 100),
                   .table_name = table,
                   .fields = {2},
                   .ids = {ColumnId{first}},
                   .span = {}}),
      .keys = {BoundColumn{.id = ColumnId{first}, .name = "i64", .type = LogicalType::kBigInt}},
      .key_ids = {ColumnId{first + 1}},
      .aggregates = {AggregateCall{.kind = AggKind::kCountStar, .id = ColumnId{first + 2}}},
      .span = {}});
}

// The union of a branch over t and one over u (u's first with `swapped`), its columns minted from
// `first`: what a CTE whose body is a UNION ALL binds to at each reference.
LogicalNodePtr UnionOfGroups(std::uint32_t first, bool swapped, bool sorted) {
  std::vector<LogicalNodePtr> inputs = {Grouped("t", first), Grouped("u", first + 10)};
  if (sorted) {
    // A Sort has no key, so neither has the union above it.
    inputs[0] =
        Node(SortNode{.input = inputs[0],
                      .keys = {SortKey{.column = BoundColumn{.id = ColumnId{first + 1},
                                                             .type = LogicalType::kBigInt}}},
                      .span = {}});
  }
  if (swapped) {
    std::swap(inputs[0], inputs[1]);
  }
  UnionAllNode node{.inputs = inputs, .columns = {}, .ids = {}, .span = {}};
  for (const LogicalNodePtr& input : inputs) {
    std::vector<BoundColumn>& columns = node.columns.emplace_back();
    for (const ColumnId id : OutputIds(*input)) {
      columns.push_back(BoundColumn{.id = id, .name = "c", .type = LogicalType::kBigInt});
    }
  }
  node.ids = {ColumnId{first + 20}, ColumnId{first + 21}};
  return Node(std::move(node));
}

// Two references to such a CTE, joined on their first columns.
LogicalPlan JoinOfUnions(bool swapped, bool sorted = false) {
  const LogicalNodePtr join = Node(JoinNode{
      .kind = JoinKind::kInner,
      .left = UnionOfGroups(100, false, sorted),
      .right = UnionOfGroups(200, swapped, sorted),
      .keys = {JoinKey{.left = BoundColumn{.id = ColumnId{120}, .type = LogicalType::kBigInt},
                       .right = BoundColumn{.id = ColumnId{220}, .type = LogicalType::kBigInt}}},
      .span = {}});
  std::vector<OutputColumn> output;
  for (const ColumnId id : OutputIds(*join)) {
    output.push_back(OutputColumn{.name = "c", .type = LogicalType::kBigInt, .id = id});
  }
  return ResolvePositions(LogicalPlan{.root = join, .output = std::move(output)});
}

// The structural report groups the repeated union as a whole. Its physical eligibility is
// decided separately by MaterializableSubPlans.
TEST(SharedSubPlansTest, AUnionRepeatedWholeIsOneGroupAndItsBranchesAreNotShared) {
  EXPECT_EQ(GroupsOf(JoinOfUnions(/*swapped=*/false)), (std::vector<std::string>{"UnionAll 2x2"}));
}

// A union's rows come in its inputs' order, so the same branches in another order are not the same
// sub-plan: the unions are not grouped -- and the branches, which do repeat, are not either, since
// nothing under a union is a member.
TEST(SharedSubPlansTest, UnionsOfTheSameBranchesInAnotherOrderAreNotGrouped) {
  EXPECT_TRUE(GroupsOf(JoinOfUnions(/*swapped=*/true)).empty());
}

// A union whose branch holds a node without a key has no key itself, and its branches are still no
// members: what keeps them unshared is where they are, not what the union above them holds.
TEST(SharedSubPlansTest, NothingUnderAUnionWithoutAKeyIsAMember) {
  EXPECT_TRUE(GroupsOf(JoinOfUnions(/*swapped=*/false, /*sorted=*/true)).empty());
}

// A union of one input, as the optimizer leaves a union whose other branches the conditions on it
// rule out (ADR 0036's X7a), columns minted from `first`.
LogicalNodePtr UnionOfOneGroup(std::uint32_t first) {
  const LogicalNodePtr branch = Grouped("t", first);
  UnionAllNode node{.inputs = {branch}, .columns = {{}}, .ids = {}, .span = {}};
  for (const ColumnId id : OutputIds(*branch)) {
    node.columns[0].push_back(BoundColumn{.id = id, .name = "c", .type = LogicalType::kBigInt});
  }
  node.ids = {ColumnId{first + 20}, ColumnId{first + 21}};
  return Node(std::move(node));
}

LogicalPlan JoinOf(LogicalNodePtr left, LogicalNodePtr right, ColumnId left_key,
                   ColumnId right_key) {
  const LogicalNodePtr join = Node(JoinNode{
      .kind = JoinKind::kInner,
      .left = std::move(left),
      .right = std::move(right),
      .keys = {JoinKey{.left = BoundColumn{.id = left_key, .type = LogicalType::kBigInt},
                       .right = BoundColumn{.id = right_key, .type = LogicalType::kBigInt}}},
      .span = {}});
  std::vector<OutputColumn> output;
  for (const ColumnId id : OutputIds(*join)) {
    output.push_back(OutputColumn{.name = "c", .type = LogicalType::kBigInt, .id = id});
  }
  return ResolvePositions(LogicalPlan{.root = join, .output = std::move(output)});
}

// A union pruned to one branch stays a union, so two references of a CTE that keep the same branch
// are still reported as one group headed by the union.
TEST(SharedSubPlansTest, AUnionOfOneInputIsStillTheGroupsHead) {
  EXPECT_EQ(
      GroupsOf(JoinOf(UnionOfOneGroup(100), UnionOfOneGroup(200), ColumnId{120}, ColumnId{220})),
      (std::vector<std::string>{"UnionAll 2x1"}));
}

// The same aggregate-topped branch outside any union is a sub-plan of its own, never one group with
// the branch under a union of one input.
TEST(SharedSubPlansTest, ABranchUnderAUnionOfOneInputIsNoMember) {
  EXPECT_TRUE(
      GroupsOf(JoinOf(UnionOfOneGroup(100), Grouped("t", 200), ColumnId{120}, ColumnId{201}))
          .empty());
  // The control: outside any union, the two are one group.
  EXPECT_FALSE(
      GroupsOf(JoinOf(Grouped("t", 100), Grouped("t", 200), ColumnId{101}, ColumnId{201})).empty());
}

LogicalNodePtr ScanOf(const std::string& table, std::uint32_t id) {
  return Node(
      ScanNode{.table = std::make_shared<testing::FakeTable>(testing::AllTypesSchema(), 100),
               .table_name = table,
               .fields = {2},
               .ids = {ColumnId{id}},
               .span = {}});
}

// An inner join of `left` and `right` on their first columns.
LogicalNodePtr JoinedOnFirst(const LogicalNodePtr& left, const LogicalNodePtr& right) {
  return Node(JoinNode{
      .kind = JoinKind::kInner,
      .left = left,
      .right = right,
      .keys = {JoinKey{
          .left = BoundColumn{.id = OutputIds(*left).front(), .type = LogicalType::kBigInt},
          .right = BoundColumn{.id = OutputIds(*right).front(), .type = LogicalType::kBigInt}}},
      .span = {}});
}

TEST_F(SharedSubPlansFilteredGroupsTest, ChecksRawFieldTypeAndPredicatePositionBeforeWidening) {
  const auto query = BandCount("p.band=1") + " UNION ALL " + BandCount("p.band=2");
  const auto groups = Combined(query);
  ASSERT_EQ(groups.size(), 1U);
  ASSERT_EQ(groups.front().members.size(), 2U);
  for (const bool wrong_type : {false, true}) {
    SCOPED_TRACE(wrong_type);
    const auto malformed = [wrong_type](const LogicalNodePtr& member) {
      auto group = std::get<GroupAggregateNode>(*member);
      auto filter = std::get<FilterNode>(*group.input);
      auto& equality = filter.predicates.front();
      auto column = equality.column.value_or(BoundColumn{});
      if (wrong_type) {
        // The equality and group agree with each other but claim the BIGINT scan field is INTEGER.
        column.type = LogicalType::kInteger;
        equality.constant.type = LogicalType::kInteger;
        group.keys.at(1).type = LogicalType::kInteger;
      } else {
        // The predicate identifies band but executes against slot.
        column.index = group.keys.front().index;
      }
      equality.column = std::move(column);
      group.input = Node(std::move(filter));
      return Node(std::move(group));
    };
    const auto& members = groups.front().members;
    const auto root = JoinedOnFirst(malformed(members.front()), malformed(members.back()));
    const auto candidates = MaterializableSubPlans({.root = root, .output = {}}, true);
    EXPECT_TRUE(std::ranges::none_of(
        candidates, [](const auto& group) { return group.combined_producer != nullptr; }));
  }
}

TEST_F(SharedSubPlansFilteredGroupsTest, AFilterAboveAComputeCannotPruneTheScan) {
  const auto query = BandCount("p.band=1", "p.band", "SUM(p.slot+1)") + " UNION ALL " +
                     BandCount("p.band=2", "p.band", "SUM(p.slot+1)");
  const auto groups = Combined(query);
  ASSERT_EQ(groups.size(), 1U);
  packets_->WithParts({kRows / 2, kRows / 2});
  for (int part = 0; part < 2; ++part) {
    packets_->WithStats(part, 0, {.min = 1, .max = 100, .rows = kRows / 2})
        .WithStats(part, 1, {.min = part + 1, .max = part + 1, .rows = kRows / 2});
  }
  EXPECT_TRUE(Combined(query).empty());  // in the bound queries the filter reads the scan
  const auto above_compute = [](const LogicalNodePtr& member) {
    auto group = std::get<GroupAggregateNode>(*member);
    auto compute = std::get<ComputeNode>(*group.input);
    auto filter = std::get<FilterNode>(*compute.input);
    compute.input = filter.input;
    filter.input = Node(std::move(compute));
    group.input = Node(std::move(filter));
    return Node(std::move(group));
  };
  const auto& members = groups.front().members;
  const auto root = JoinedOnFirst(above_compute(members.front()), above_compute(members.back()));
  const auto candidates = MaterializableSubPlans({.root = root, .output = {}}, true);
  EXPECT_TRUE(std::ranges::any_of(
      candidates, [](const auto& group) { return group.combined_producer != nullptr; }));
}

TEST_F(SharedSubPlansFilteredGroupsTest, DistinguishesTheSlotReadInsideAMemberExpression) {
  const auto query = BandCount("p.band=1", "p.band", "SUM(p.slot+1)") + " UNION ALL " +
                     BandCount("p.band=2", "p.band", "SUM(p.slot+1)");
  const auto groups = Combined(query);
  ASSERT_EQ(groups.size(), 1U);
  const auto& members = groups.front().members;
  ASSERT_EQ(members.size(), 2U);
  auto second = std::get<GroupAggregateNode>(*members.back());
  auto compute = std::get<ComputeNode>(*second.input);
  ASSERT_EQ(compute.exprs.size(), 1U);
  // Its column ID still refers to slot, but this member actually sums band+1.
  compute.exprs.front() =
      MapColumns(compute.exprs.front(), [band = second.keys.at(1).index](ColumnExpr column) {
        column.index = band;
        return column;
      });
  second.input = Node(std::move(compute));
  const auto root = JoinedOnFirst(members.front(), Node(std::move(second)));
  const auto candidates = MaterializableSubPlans({.root = root, .output = {}}, true);
  EXPECT_TRUE(std::ranges::none_of(
      candidates, [](const auto& group) { return group.combined_producer != nullptr; }));
}

TEST_F(SharedSubPlansFilteredGroupsTest, LeavesInvalidKeySlotsAndScanFieldsToOrdinaryPlanning) {
  const auto query = BandCount("p.band=1") + " UNION ALL " + BandCount("p.band=2");
  const auto groups = Combined(query);
  ASSERT_EQ(groups.size(), 1U);
  const auto& members = groups.front().members;
  ASSERT_EQ(members.size(), 2U);
  const auto refuses = [&](const auto& change) {
    const auto changed = [&](const LogicalNodePtr& node) {
      auto group = std::get<GroupAggregateNode>(*node);
      auto filter = std::get<FilterNode>(*group.input);
      auto scan = std::get<ScanNode>(*filter.input);
      change(group, filter, scan);
      filter.input = Node(std::move(scan));
      group.input = Node(std::move(filter));
      return Node(std::move(group));
    };
    const auto root = JoinedOnFirst(changed(members.front()), changed(members.back()));
    const auto candidates = MaterializableSubPlans({.root = root, .output = {}}, true);
    EXPECT_TRUE(std::ranges::none_of(
        candidates, [](const auto& group) { return group.combined_producer != nullptr; }));
  };
  for (const int invalid : {-1, 100}) {
    SCOPED_TRACE(invalid);
    refuses([invalid](auto& group, auto&, auto&) { group.keys.front().index = invalid; });
    refuses([invalid](auto&, auto&, auto& scan) { scan.fields.front() = invalid; });
  }
  refuses([](auto& group, auto&, auto&) { group.keys.front().id = kNoColumnId; });
  refuses([](auto& group, auto&, auto&) { group.key_ids.pop_back(); });
  refuses([](auto&, auto&, auto& scan) { scan.fields.pop_back(); });
  // Limit the number of alternative equality fingerprints a single group can request.
  refuses([](auto&, auto& filter, auto&) {
    filter.predicates.resize(kMaxPredicateDomainKeys + 1, filter.predicates.front());
  });
}

// GROUP BY the first column of `input` with COUNT(*), its columns minted from `first`.
LogicalNodePtr GroupedOn(const LogicalNodePtr& input, std::uint32_t first) {
  return Node(GroupAggregateNode{
      .input = input,
      .keys = {BoundColumn{
          .id = OutputIds(*input).front(), .name = "c", .type = LogicalType::kBigInt}},
      .key_ids = {ColumnId{first}},
      .aggregates = {AggregateCall{.kind = AggKind::kCountStar, .id = ColumnId{first + 1}}},
      .span = {}});
}

LogicalPlan PlanOf(const LogicalNodePtr& root) {
  std::vector<OutputColumn> output;
  for (const ColumnId id : OutputIds(*root)) {
    output.push_back(OutputColumn{.name = "c", .type = LogicalType::kBigInt, .id = id});
  }
  return ResolvePositions(LogicalPlan{.root = root, .output = std::move(output)});
}

// A copy of a repeated sub-plan under a union neither is a member nor reports a group: the
// sub-plan's other two copies lie inside the members of a larger group, which is the one reported,
// and a group nested wholly in it would be computed and held for no consumer.
TEST(SharedSubPlansTest, ACopyUnderAUnionReportsNoNestedGroup) {
  // X groups a scan of big; G groups X joined to a scan of small, and is referenced twice; a third
  // copy of X is a branch of a union that occurs once.
  const auto g = [](std::uint32_t first) {
    return GroupedOn(JoinedOnFirst(Grouped("big", first), ScanOf("small", first + 5)), first + 10);
  };
  UnionAllNode u{.inputs = {Grouped("big", 300), Grouped("other", 310)},
                 .columns = {},
                 .ids = {ColumnId{320}, ColumnId{321}},
                 .span = {}};
  for (const LogicalNodePtr& input : u.inputs) {
    std::vector<BoundColumn>& columns = u.columns.emplace_back();
    for (const ColumnId id : OutputIds(*input)) {
      columns.push_back(BoundColumn{.id = id, .name = "c", .type = LogicalType::kBigInt});
    }
  }
  const LogicalNodePtr root = JoinedOnFirst(JoinedOnFirst(g(100), g(200)), Node(std::move(u)));
  EXPECT_EQ(GroupsOf(PlanOf(root)), (std::vector<std::string>{"GroupAggregate 2x2"}));
}

TEST(SharedSubPlansTest, SharesTheUncoveredCopiesOfANestedAggregate) {
  const auto table = std::make_shared<testing::FakeTable>(testing::AllTypesSchema(), 20'000'000);
  const auto count = [](LogicalNodePtr input, std::uint32_t id) {
    return Node(AggregateNode{
        .input = std::move(input),
        .aggregates = {AggregateCall{.kind = AggKind::kCountStar, .id = ColumnId{id}}},
        .span = {}});
  };
  const auto inner = [&](std::uint32_t id) {
    return count(
        Node(ScanNode{
            .table = table, .table_name = "t", .fields = {2}, .ids = {ColumnId{id}}, .span = {}}),
        id + 1);
  };
  const std::vector outer = {count(inner(10), 12), count(inner(20), 22)};
  const std::vector outside = {inner(30), inner(40)};
  const auto root =
      JoinedOnFirst(JoinedOnFirst(outer[0], outer[1]), JoinedOnFirst(outside[0], outside[1]));

  const auto groups = MaterializableSubPlans({.root = root, .output = {}});
  ASSERT_EQ(groups.size(), 2U);
  EXPECT_EQ(groups[0].members, outer);
  EXPECT_EQ(groups[1].members, outside);
  EXPECT_EQ(groups[1].columns, (std::vector<std::vector<int>>{{0}, {0}}));
}

// A group above a union is found as any other: an aggregation of a union, repeated, is one group
// headed by the aggregation -- which the gate may hold, since it reduces.
TEST(SharedSubPlansTest, AGroupAboveAUnionIsFound) {
  const LogicalNodePtr root = JoinedOnFirst(GroupedOn(UnionOfGroups(100, false, false), 400),
                                            GroupedOn(UnionOfGroups(200, false, false), 500));
  EXPECT_EQ(GroupsOf(PlanOf(root)), (std::vector<std::string>{"GroupAggregate 2x2"}));
}

// **The two sides of a self-join are not one place.** A table's field is one origin wherever it
// is read, so these two sub-plans -- one join, grouped by `a.i64` in one and by `b.i64` in the
// other, which give different groups -- once keyed as one, and exec answered one from the other's
// result. Each reference now carries its path, which tells the two sides apart.
TEST(SharedSubPlansTest, TheSidesOfASelfJoinAreNotOnePlace) {
  const std::vector<std::string> groups = Groups(
      "SELECT COUNT(*) FROM "
      "(SELECT a.i64 AS g, COUNT(*) AS n FROM t AS a, t AS b WHERE a.i32 = b.i32 AND b.i16 > 3 "
      "GROUP BY a.i64) AS s1, "
      "(SELECT b.i64 AS g, COUNT(*) AS n FROM t AS a, t AS b WHERE a.i32 = b.i32 AND b.i16 > 3 "
      "GROUP BY b.i64) AS s2 "
      "WHERE s1.g = s2.g");
  // Not even the joins under them: one outputs `a.i64` and the other `b.i64`, so neither's output
  // holds the other's, and one result could not serve both. Matched by origin alone, `b.i64`
  // would have been read from `a.i64`'s place.
  EXPECT_TRUE(groups.empty()) << groups.front();
}

// **Nor are two aggregations' counts one column.** COUNT(*) has no argument, so every COUNT(*)
// has one origin whatever it counts: a sum of one side's count and a sum of the other's once
// keyed as one. The path of each read tells them apart, and the join they share is what is
// reported.
TEST(SharedSubPlansTest, TwoAggregationsCountsAreNotOneColumn) {
  constexpr std::string_view kCounts =
      "(SELECT i64 AS k, COUNT(*) AS n FROM t GROUP BY i64) AS x, "
      "(SELECT i64 AS k, COUNT(*) AS n FROM u GROUP BY i64) AS y WHERE x.k = y.k GROUP BY x.k";
  const std::vector<std::string> groups =
      Groups(std::format("SELECT COUNT(*) FROM (SELECT x.k, SUM(x.n) AS s FROM {}) AS p, "
                         "(SELECT x.k, SUM(y.n) AS s FROM {}) AS q WHERE p.k = q.k",
                         kCounts, kCounts));
  ASSERT_EQ(groups.size(), 1U);
  EXPECT_EQ(groups[0].substr(0, groups[0].find(' ')), "Join") << groups[0];
}

// A join on a NULL-equal key matches NULL to NULL, which the same join on an `=` key does not
// (ADR 0037's OJ5): the two compute different rows and are not one sub-plan, while the scans under
// them still are. The control: two joins whose keys agree are one group, headed by the join.
TEST(SharedSubPlansTest, AJoinOnANullEqualKeyIsNotOneWithTheJoinOnAnEqualOne) {
  const auto joined = [](std::uint32_t first, bool null_equal) {
    JoinNode join = std::get<JoinNode>(*JoinedOnFirst(ScanOf("t", first), ScanOf("u", first + 1)));
    join.keys.front().null_equal = null_equal;
    return Node(std::move(join));
  };
  for (const bool first : {false, true}) {
    for (const bool second : {false, true}) {
      SCOPED_TRACE(std::format("NULL-equal: {} and {}", first, second));
      const std::vector<std::string> groups =
          GroupsOf(PlanOf(JoinedOnFirst(joined(10, first), joined(20, second))));
      ASSERT_FALSE(groups.empty());
      const bool joins = std::ranges::any_of(
          groups, [](const std::string& group) { return group.starts_with("Join "); });
      EXPECT_EQ(joins, first == second) << groups.front();
    }
  }
}

// A mark join's mark (ADR 0041's SQ7a) has its origin in the join's keys: a CTE that computes over
// one, referenced twice, is one sub-plan up to its Project, while two marks over other probes of
// the same tables are not one column, so what reads them is not shared.
TEST(SharedSubPlansTest, AMarkIsOneColumnOnlyOverTheSameKeys) {
  EXPECT_EQ(Groups("WITH g AS (SELECT i64, CASE WHEN i32 IN (SELECT i16 FROM u) THEN 1 ELSE 0 END "
                   "AS m FROM t) SELECT COUNT(*) FROM g AS a, g AS b WHERE a.i64 = b.i64 AND "
                   "a.m = b.m"),
            (std::vector<std::string>{"Project 2x3"}))
      << "the CTE's Project heads it, its mark join and the Compute that reads the mark inside";
  EXPECT_EQ(Groups("SELECT COUNT(*) FROM (SELECT i64, CASE WHEN i32 IN (SELECT i16 FROM u) THEN 1 "
                   "ELSE 0 END AS m FROM t) AS a, (SELECT i64, CASE WHEN i16 IN (SELECT i16 FROM "
                   "u) THEN 1 ELSE 0 END AS m FROM t) AS b WHERE a.i64 = b.i64 AND a.m = b.m"),
            (std::vector<std::string>{"Project 2x1"}))
      << "only the set's sub-plan, read by both marks, is one";
}

// A correlated EXISTS's mark (ADR 0041's SQ8a) has its origin in the mark join's kind, keys and
// residuals: a CTE that computes over one, referenced twice, is one sub-plan up to its Project,
// while two marks of one subtree whose residuals differ are not one column, so what reads them is
// not shared.
TEST(SharedSubPlansTest, ACorrelatedMarkIsOneColumnOnlyOverTheSameResiduals) {
  EXPECT_EQ(
      Groups("WITH g AS (SELECT i64, CASE WHEN EXISTS (SELECT 1 FROM u WHERE u.i64 = t.i64 AND "
             "u.i32 > t.i32) THEN 1 ELSE 0 END AS m FROM t) SELECT COUNT(*) FROM g AS a, g AS b "
             "WHERE a.i64 = b.i64 AND a.m = b.m"),
      (std::vector<std::string>{"Project 2x4"}))
      << "the CTE's Project heads it, its mark join and the Compute that reads the mark inside";
  EXPECT_EQ(
      Groups("SELECT COUNT(*) FROM (SELECT i64, CASE WHEN EXISTS (SELECT 1 FROM u WHERE u.i64 = "
             "t.i64 AND u.i32 > t.i32) THEN 1 ELSE 0 END AS m FROM t) AS a, (SELECT i64, CASE "
             "WHEN EXISTS (SELECT 1 FROM u WHERE u.i64 = t.i64 AND u.i32 < t.i32) THEN 1 ELSE 0 "
             "END AS m FROM t) AS b WHERE a.i64 = b.i64 AND a.m = b.m"),
      (std::vector<std::string>{"Scan 2x2", "Scan 2x2"}))
      << "only the scans under the two marks are one";
}

// The same plan gives the same groups in the same order, which EXPLAIN's goldens depend on.
TEST(SharedSubPlansTest, IsDeterministic) {
  static constexpr std::string_view kSql =
      "WITH g AS (SELECT i64 AS k, COUNT(*) AS n FROM t GROUP BY i64) "
      "SELECT COUNT(*) FROM g AS a, g AS b WHERE a.k = b.k";
  const std::vector<std::string> first = Groups(kSql);
  ASSERT_FALSE(first.empty());
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(Groups(kSql), first);
  }
}

}  // namespace
}  // namespace antb1::plan
