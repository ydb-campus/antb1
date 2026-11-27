// Computing a repeated sub-plan once (docs/adr/0030-shared-sub-plans.md, E2).
//
// These bind SQL rather than hand-building plans, which is unusual for an exec test and necessary:
// the pass that finds repeated sub-plans matches columns by where they come from, and that is
// derived from the ColumnIds the binder mints. A hand-built plan leaves them unset, so nothing
// would ever be found to share and the test would pass while testing nothing.

#include "../shared_builds.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <arrow/api.h>
#include <gtest/gtest.h>

#include "antb1/exec/key_filter.h"
#include "antb1/exec/memory_budget.h"
#include "antb1/exec/operator.h"
#include "antb1/exec/physical_planner.h"
#include "antb1/exec/profile.h"
#include "antb1/plan/binder.h"
#include "antb1/plan/catalog.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/optimizer.h"
#include "antb1/plan/plan_options.h"
#include "antb1/plan/shared_sub_plans.h"
#include "antb1/sql/parser.h"

#include "exec_test_util.h"

namespace antb1::exec {
namespace {

using testing::MemoryTable;

// As AggregateFilterTest: the kernels need arrow::compute::Initialize, which ExecTest does.
class SharedBuildsTest : public testing::ExecTest {};

// Three batches of `k` (a BIGINT with repeats, so a GROUP BY reduces) and `v`.
std::shared_ptr<MemoryTable> KeyedTable() {
  const auto schema =
      arrow::schema({arrow::field("k", arrow::int64()), arrow::field("v", arrow::int64())});
  arrow::RecordBatchVector batches;
  for (int64_t part = 0; part < 3; ++part) {
    std::vector<std::optional<int64_t>> keys;
    std::vector<std::optional<int64_t>> values;
    for (int64_t r = 0; r < 20; ++r) {
      keys.emplace_back(((part * 20) + r) % 7);  // 7 groups over 60 rows
      values.emplace_back(r);
    }
    batches.push_back(arrow::RecordBatch::Make(
        schema, 20, arrow::ArrayVector{testing::Int64s(keys), testing::Int64s(values)}));
  }
  return std::make_shared<MemoryTable>(schema, std::move(batches), /*split=*/true);
}

// The plan of `sql` over `table` registered as "t", optimized as the engine would.
arrow::Result<plan::LogicalPlan> PlanOf(std::string_view sql,
                                        const std::shared_ptr<MemoryTable>& table) {
  plan::Catalog catalog;
  ARROW_RETURN_NOT_OK(catalog.Register("t", table));
  const auto parsed = sql::Parse(sql);
  if (!parsed.has_value()) {
    return arrow::Status::Invalid("parse failed");
  }
  ARROW_ASSIGN_OR_RAISE(const plan::LogicalPlan bound,
                        plan::Bind(*parsed, catalog, plan::BindOptions{}));
  return plan::Optimize(bound);
}

struct ExecutionResult {
  std::shared_ptr<arrow::Table> rows;
  int parts_scanned = 0;               // how many part scans the base table served
  std::optional<int64_t> shared_rows;  // the profile's count, absent when nothing was shared
  int64_t shared_fallbacks = 0;
};

arrow::Result<ExecutionResult> ExecutePlan(const plan::LogicalPlan& plan,
                                           const std::shared_ptr<MemoryTable>& table) {
  ProfileNode profile;
  ARROW_ASSIGN_OR_RAISE(std::unique_ptr<Operator> op, BuildPhysicalPlan(plan, &profile));
  MemoryBudget budget(int64_t{1024} * 1024 * 1024);
  ExecContext ctx{.pool = arrow::default_memory_pool(), .budget = &budget};
  ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::Table> rows, Drain(*op, ctx));
  std::optional<int64_t> shared_rows;
  int64_t shared_fallbacks = 0;
  for (const ProfileMetric& metric : profile.metrics()) {
    if (metric.name == "shared_rows") {
      shared_rows = metric.value;
    } else if (metric.name == "shared_fallbacks") {
      shared_fallbacks = metric.value;
    }
  }
  return ExecutionResult{.rows = std::move(rows),
                         .parts_scanned = static_cast<int>(table->scanned_parts().size()),
                         .shared_rows = shared_rows,
                         .shared_fallbacks = shared_fallbacks};
}

arrow::Result<ExecutionResult> Execute(std::string_view sql,
                                       const std::shared_ptr<MemoryTable>& table) {
  ARROW_ASSIGN_OR_RAISE(const plan::LogicalPlan plan, PlanOf(sql, table));
  return ExecutePlan(plan, table);
}

// Two branches read the same event row groups through different slices of a lookup. Keep
// a double subtotal per original row group, including when a lookup key matches both slices.
struct PeriodSums {
  std::shared_ptr<MemoryTable> events;
  plan::Catalog catalog;

  PeriodSums() {
    const auto schema =
        arrow::schema({arrow::field("account", arrow::int64()), arrow::field("day", arrow::int64()),
                       arrow::field("amount", arrow::float64())});
    arrow::RecordBatchVector parts;
    for (const double leading : {1e16, -1e16}) {
      parts.push_back(arrow::RecordBatch::Make(
          schema, 5,
          {testing::Int64s({0, 0, 1, 2, 3}), testing::Int64s({1, 1, 2, 2, 9}),
           testing::ArrayOf<arrow::DoubleBuilder, double>(arrow::float64(),
                                                          {leading, 1., 2., std::nullopt, 3.})}));
    }
    events = std::make_shared<MemoryTable>(schema, std::move(parts), true);
    const auto lookup =
        arrow::schema({arrow::field("day", arrow::int64()), arrow::field("period", arrow::int64()),
                       arrow::field("weight", arrow::float64())});
    auto rows = arrow::RecordBatch::Make(
        lookup, 5,
        {testing::Int64s({1, 1, 2, 2, 2}), testing::Int64s({5, 7, 5, 7, 5}),
         testing::ArrayOf<arrow::DoubleBuilder, double>(arrow::float64(), {1., 1., 1., 1., 1.})});
    EXPECT_TRUE(catalog.Register("events", events).ok());
    EXPECT_TRUE(catalog
                    .Register("bands", std::make_shared<MemoryTable>(
                                           lookup, arrow::RecordBatchVector{rows}, true))
                    .ok());
  }

  static std::string Branch(int period, std::string_view lookup) {
    return std::format(
        "SELECT e.account AS k, b.period AS p, SUM(e.amount * b.weight) AS amount "
        "FROM events e JOIN {} b ON e.day=b.day WHERE b.period={} GROUP BY e.account,b.period",
        lookup, period);
  }

  arrow::Result<plan::LogicalPlan> Query(std::string_view sql, bool large) const {
    events->set_claimed_rows(large ? 20'000'000 : 100);
    const auto parsed = sql::Parse(sql);
    if (!parsed) {
      return arrow::Status::Invalid("parse failed");
    }
    ARROW_ASSIGN_OR_RAISE(auto bound, plan::Bind(*parsed, catalog, {}));
    return plan::Optimize(bound);
  }

  arrow::Result<plan::LogicalPlan> Plan(int right_period, bool large,
                                        std::string_view lookup = "bands") const {
    return Query(std::format("SELECT a.k AS k, a.amount AS x, b.amount AS y FROM ({}) a JOIN "
                             "({}) b ON a.k=b.k ORDER BY k,x,y",
                             Branch(5, lookup), Branch(right_period, lookup)),
                 large);
  }
};

int64_t MetricSum(const ProfileNode& node, std::string_view name) {
  int64_t result = 0;
  for (const auto& metric : node.metrics()) {
    if (metric.name == name) {
      result += metric.value;
    }
  }
  for (const auto& child : node.children()) {
    result += MetricSum(*child, name);
  }
  return result;
}

arrow::Result<std::shared_ptr<arrow::Table>> RunPeriods(const plan::LogicalPlan& plan,
                                                        ProfileNode& profile,
                                                        MemoryBudget& budget) {
  ARROW_ASSIGN_OR_RAISE(auto op, BuildPhysicalPlan(plan, &profile));
  ExecContext ctx{.pool = &budget, .batch_size = 1, .budget = &budget};
  return Drain(*op, ctx);
}

TEST_F(SharedBuildsTest, OneScanKeepsEachPartitionSumNullsAndJoinMultiplicity) {
  PeriodSums fixture;
  MemoryBudget budget(std::nullopt);
  ProfileNode ordinary;
  auto small = fixture.Plan(7, false);
  ASSERT_TRUE(small.ok()) << small.status();
  auto expected = RunPeriods(*small, ordinary, budget);
  ASSERT_TRUE(expected.ok()) << expected.status();
  EXPECT_EQ(fixture.events->scanned_parts().size(), 4U);
  ProfileNode shared;
  auto plan = fixture.Plan(7, true);
  ASSERT_TRUE(plan.ok()) << plan.status();
  auto got = RunPeriods(*plan, shared, budget);
  ASSERT_TRUE(got.ok()) << got.status();
  EXPECT_EQ(fixture.events->scanned_parts().size(), 6U);
  EXPECT_EQ(MetricSum(shared, "split_scan_parts"), 2);
  const auto first_rows = MetricSum(shared, "split_rows_0");
  const auto second_rows = MetricSum(shared, "split_rows_1");
  EXPECT_EQ(std::min(first_rows, second_rows), 8);
  EXPECT_EQ(std::max(first_rows, second_rows), 12);
  EXPECT_EQ(MetricSum(shared, "shared_rows"), 6);
  ASSERT_TRUE((*got)->Equals(**expected));
  // Per-part cancellation gives zero; two matches in the first band count twice.
  const auto combined = (*got)->CombineChunks();
  ASSERT_TRUE(combined.ok()) << combined.status();
  ASSERT_EQ((*combined)->num_rows(), 3);
  const auto& x = static_cast<const arrow::DoubleArray&>(*(*combined)->column(1)->chunk(0));
  const auto& y = static_cast<const arrow::DoubleArray&>(*(*combined)->column(2)->chunk(0));
  EXPECT_EQ(x.Value(0), 0.);
  EXPECT_EQ(y.Value(0), 0.);
  EXPECT_EQ(x.Value(1), 8.);
  EXPECT_EQ(y.Value(1), 4.);
  EXPECT_TRUE(x.IsNull(2));
  EXPECT_TRUE(y.IsNull(2));
}

TEST_F(SharedBuildsTest, EmptyJoinedPartitionsRetryWithTheOriginalScans) {
  PeriodSums fixture;
  const auto bands = std::dynamic_pointer_cast<MemoryTable>(fixture.catalog.Find("bands"));
  ASSERT_NE(bands, nullptr);
  bands->set_stats(false);
  auto plan = fixture.Plan(9, true);
  ASSERT_TRUE(plan.ok()) << plan.status();
  MemoryBudget budget(std::nullopt);
  ProfileNode profile;
  auto rows = RunPeriods(*plan, profile, budget);
  ASSERT_TRUE(rows.ok()) << rows.status();
  EXPECT_EQ((*rows)->num_rows(), 0);
  EXPECT_EQ(MetricSum(profile, "shared_fallbacks"), 1);
}

TEST_F(SharedBuildsTest, MissingLookupPeriodAvoidsTheSharedFactScan) {
  PeriodSums fixture;
  auto plan = fixture.Plan(9, true);
  ASSERT_TRUE(plan.ok()) << plan.status();
  MemoryBudget budget(std::nullopt);
  ProfileNode profile;
  auto rows = RunPeriods(*plan, profile, budget);
  ASSERT_TRUE(rows.ok()) << rows.status();
  EXPECT_EQ((*rows)->num_rows(), 0);
  EXPECT_TRUE(fixture.events->scanned_parts().empty());
  EXPECT_NE(profile.name(), "SharedGroupPartitions");
  EXPECT_EQ(MetricSum(profile, "shared_fallbacks"), 0);
}

TEST_F(SharedBuildsTest, SeparateStatesRetryWhenOnlyIndependentGroupsFit) {
  PeriodSums fixture;
  int64_t original_peak = 0;
  {
    MemoryBudget budget(std::nullopt);
    ProfileNode profile;
    auto original = fixture.Plan(7, false);
    ASSERT_TRUE(original.ok()) << original.status();
    auto rows = RunPeriods(*original, profile, budget);
    ASSERT_TRUE(rows.ok()) << rows.status();
    original_peak = budget.max_memory();
  }
  auto plan = fixture.Plan(7, true);
  ASSERT_TRUE(plan.ok()) << plan.status();
  MemoryBudget budget(original_peak);
  ProfileNode profile;
  auto rows = RunPeriods(*plan, profile, budget);
  ASSERT_TRUE(rows.ok()) << rows.status();
  EXPECT_EQ((*rows)->num_rows(), 3);
  ASSERT_EQ(profile.children().size(), 2U);
  EXPECT_EQ(MetricSum(*profile.children().front(), "shared_fallbacks"), 0);
  EXPECT_EQ(MetricSum(profile, "shared_fallbacks"), 1);
}

arrow::Status RegisterWideBands(PeriodSums& fixture) {
  const auto schema =
      arrow::schema({arrow::field("day", arrow::int64()), arrow::field("period", arrow::int64()),
                     arrow::field("weight", arrow::float64())});
  // Each original lookup fits, but the two homogeneous parts together exceed the exact-key cap.
  constexpr int64_t kRows = (KeyFilter::kMaxPublishedKeys / 2) + 1;
  std::vector<std::optional<int64_t>> days;
  days.reserve(kRows);
  for (int64_t i = 0; i < kRows; ++i) {
    days.emplace_back(i);
  }
  const auto keys = testing::Int64s(days);
  ARROW_ASSIGN_OR_RAISE(auto weights, arrow::MakeArrayFromScalar(arrow::DoubleScalar(1.), kRows));
  arrow::RecordBatchVector batches;
  for (const int64_t value : {5, 7}) {
    ARROW_ASSIGN_OR_RAISE(auto period,
                          arrow::MakeArrayFromScalar(arrow::Int64Scalar(value), kRows));
    batches.push_back(arrow::RecordBatch::Make(schema, kRows, {keys, period, weights}));
  }
  return fixture.catalog.Register("wide_bands",
                                  std::make_shared<MemoryTable>(schema, std::move(batches), true));
}

TEST_F(SharedBuildsTest, DeclinedLookupUsesOrdinaryPhysicalRoot) {
  PeriodSums fixture;
  ASSERT_TRUE(RegisterWideBands(fixture).ok());
  const auto plan = fixture.Plan(7, true, "wide_bands");
  ASSERT_TRUE(plan.ok()) << plan.status();
  ASSERT_TRUE(std::ranges::any_of(plan::MaterializableSubPlans(*plan),
                                  &plan::SharedSubPlan::separate_partition_states));
  ProfileNode profile;
  auto op = BuildPhysicalPlan(*plan, &profile);
  ASSERT_TRUE(op.ok()) << op.status();
  EXPECT_NE(profile.name(), "SharedGroupPartitions");
  MemoryBudget budget(std::nullopt);
  auto rows = Drain(**op, ExecContext{.pool = &budget, .budget = &budget});
  ASSERT_TRUE(rows.ok()) << rows.status();
  EXPECT_EQ((*rows)->num_rows(), 4);
  EXPECT_EQ(MetricSum(profile, "shared_rows"), 0);
  EXPECT_EQ(MetricSum(profile, "split_scan_parts"), 0);
}

TEST_F(SharedBuildsTest, DeclinedLookupKeepsExactSharingAlongsideAnAcceptedSplit) {
  PeriodSums fixture;
  ASSERT_TRUE(RegisterWideBands(fixture).ok());
  // Only the two wide branches outside the union are candidates for a split. Declining that
  // split frees the period-7 group to share with its identical copy inside the union.
  const auto wide = PeriodSums::Branch(7, "wide_bands");
  const auto sql = std::format(
      "SELECT a.amount AS v,b.amount AS w,c.amount AS x,d.amount AS y,e.amount AS z "
      "FROM ({}) a JOIN ({}) b ON a.k=b.k JOIN ({} UNION ALL {}) c ON b.k=c.k "
      "JOIN ({}) d ON c.k=d.k JOIN ({}) e ON d.k=e.k ORDER BY v,w,x,y,z",
      PeriodSums::Branch(5, "wide_bands"), wide, wide, PeriodSums::Branch(9, "wide_bands"),
      PeriodSums::Branch(5, "bands"), PeriodSums::Branch(7, "bands"));
  const auto plan = fixture.Query(sql, true);
  ASSERT_TRUE(plan.ok()) << plan.status();
  ProfileNode profile;
  auto op = BuildPhysicalPlan(*plan, &profile);
  ASSERT_TRUE(op.ok()) << op.status();
  MemoryBudget budget(std::nullopt);
  const auto rows = Drain(**op, ExecContext{.pool = &budget, .budget = &budget});
  ASSERT_TRUE(rows.ok()) << rows.status();
  EXPECT_EQ((*rows)->num_rows(), 3);
  EXPECT_EQ(MetricSum(profile, "split_scan_parts"), 2);
  // The narrow producer emits three groups for each period. The four wide period-7 groups
  // must be materialized once for their two readers.
  EXPECT_EQ(MetricSum(profile, "shared_rows"), 10);
  EXPECT_EQ(fixture.events->scanned_parts().size(), 6U);
  EXPECT_EQ(MetricSum(profile, "shared_fallbacks"), 0);
}

// A CTE referenced twice: the sub-plan is a grouped aggregate, which reduces, so with a table large
// enough to be worth not reading twice the plan computes it once. The control is the *same* plan
// over the *same* rows, differing only in the row count the table claims -- so the two arms differ
// in the sharing and in nothing else.
constexpr std::string_view kTwiceReferencedCte =
    "WITH g AS (SELECT k, COUNT(*) AS n FROM t GROUP BY k) "
    "SELECT COUNT(*) FROM g AS a, g AS b WHERE a.k = b.k";

TEST_F(SharedBuildsTest, ComputesARepeatedSubPlanOnceAndKeepsTheAnswer) {
  const auto shared = KeyedTable();
  shared->set_claimed_rows(20'000'000);  // past the gate, while holding 60 rows
  auto with = Execute(kTwiceReferencedCte, shared);
  ASSERT_TRUE(with.ok()) << with.status().ToString();

  const auto control = KeyedTable();  // claims its 60 rows: too cheap to be worth sharing
  auto without = Execute(kTwiceReferencedCte, control);
  ASSERT_TRUE(without.ok()) << without.status().ToString();

  EXPECT_EQ(with->parts_scanned, 3) << "the three parts read once, for one shared result";
  EXPECT_EQ(with->shared_rows, 7) << "and the profile says what was shared: one row a group";
  EXPECT_EQ(without->shared_rows, std::nullopt) << "nothing shared, nothing reported";
  EXPECT_EQ(without->parts_scanned, 6) << "and twice when the sub-plan is recomputed";
  ASSERT_NE(with->rows, nullptr);
  ASSERT_NE(without->rows, nullptr);
  EXPECT_TRUE(with->rows->Equals(*without->rows))
      << "sharing changes what is computed, never what is answered\n"
      << with->rows->ToString() << "\nagainst\n"
      << without->rows->ToString();
}

TEST_F(SharedBuildsTest, ReleasesOversizedResultAndReopensWithTheOriginalPlan) {
  const auto schema = arrow::schema({arrow::field("k", arrow::int64())});
  const auto values =
      testing::Int64s(std::vector<std::optional<int64_t>>(std::size_t{128} * 1024, 7));
  const auto large = arrow::RecordBatch::Make(schema, values->length(), {values});
  const auto small = large->Slice(0, 1);
  // Reuse one buffer behind many batches. The table charges each batch it retains.
  int producer_builds = 0;
  const auto make_producer = [&] {
    ++producer_builds;
    return std::make_unique<testing::ScriptedSource>(schema,
                                                     std::vector<Batch>(257, Batch{.data = large}));
  };
  auto shared_scan = std::make_unique<testing::ScriptedSource>(schema, std::vector<Batch>{});
  const auto* shared_scan_calls = shared_scan.get();
  std::vector<SharedBuild> builds;
  builds.push_back({.table = std::make_shared<MaterializedTable>(schema),
                    .first_producer = make_producer(),
                    .make_producer = make_producer});
  MemoryBudget budget(int64_t{512} * 1024 * 1024);
  int fallback_builds = 0;
  SharedBuildsOperator op(std::move(shared_scan), std::move(builds), [&](ProfileNode*) {
    EXPECT_EQ(budget.bytes_allocated(), 0);
    ++fallback_builds;
    return std::make_unique<testing::ScriptedSource>(schema, std::vector<Batch>{{.data = small}});
  });
  ExecContext ctx{.budget = &budget};
  for (int run = 0; run < 2; ++run) {
    const auto rows = Drain(op, ctx);
    ASSERT_TRUE(rows.ok()) << rows.status();
    EXPECT_EQ(testing::SingleInt64(**rows), 7);
    EXPECT_EQ(budget.bytes_allocated(), 0);
  }
  EXPECT_EQ(producer_builds, 2);
  EXPECT_EQ(shared_scan_calls->opens(), 0);
  EXPECT_EQ(fallback_builds, 1);
}

TEST_F(SharedBuildsTest, ReleasesBothProducersAndTheirReservationsBeforeBudgetFallback) {
  const auto schema = arrow::schema({arrow::field("k", arrow::int64())});
  constexpr int64_t kRows = 8;
  constexpr int64_t kBatchBytes = kRows * 8;
  MemoryBudget budget(4 * kBatchBytes);
  const auto make_producer = [&](std::size_t batches) -> arrow::Result<std::unique_ptr<Operator>> {
    // The source itself retains its buffer after Close, just as an aggregate can retain its
    // grouper.
    ARROW_ASSIGN_OR_RAISE(auto buffer, arrow::AllocateBuffer(kBatchBytes, &budget));
    std::fill_n(buffer->mutable_data(), kBatchBytes, uint8_t{0});
    auto values = std::make_shared<arrow::Int64Array>(kRows, std::move(buffer));
    const auto batch = arrow::RecordBatch::Make(schema, kRows, {std::move(values)});
    return std::make_unique<testing::ScriptedSource>(schema,
                                                     std::vector<Batch>(batches, {.data = batch}));
  };
  const auto first_table = std::make_shared<MaterializedTable>(schema);
  const auto second_table = std::make_shared<MaterializedTable>(schema);
  std::vector<SharedBuild> builds;
  builds.push_back({.table = first_table, .make_producer = [&] { return make_producer(1); }});
  builds.push_back({.table = second_table, .make_producer = [&] {
                      EXPECT_EQ(first_table->exact_row_count(), kRows);
                      EXPECT_EQ(budget.bytes_allocated(), 2 * kBatchBytes);
                      return make_producer(2);
                    }});
  auto consumer = std::make_unique<testing::ScriptedSource>(schema, std::vector<Batch>{});
  const auto* consumer_calls = consumer.get();
  int fallbacks = 0;
  SharedBuildsOperator op(std::move(consumer), std::move(builds), [&](ProfileNode*) {
    ++fallbacks;
    // Both 64-byte buffers and their first table reservations fit. The second table's next
    // reservation is refused, even though each table is well below the per-table cap.
    EXPECT_EQ(budget.max_memory(), 4 * kBatchBytes);
    EXPECT_EQ(budget.bytes_allocated(), 0);
    EXPECT_FALSE(first_table->exact_row_count().has_value());
    EXPECT_FALSE(second_table->exact_row_count().has_value());
    const auto answer = arrow::RecordBatch::Make(schema, 1, {testing::Int64s({42})});
    return std::make_unique<testing::ScriptedSource>(schema, std::vector<Batch>{{.data = answer}});
  });
  const auto rows = Drain(op, ExecContext{.budget = &budget});
  ASSERT_TRUE(rows.ok()) << rows.status();
  EXPECT_EQ(testing::SingleInt64(**rows), 42);
  EXPECT_EQ(consumer_calls->opens(), 0);
  EXPECT_EQ(fallbacks, 1);
  EXPECT_EQ(budget.bytes_allocated(), 0);
}

TEST_F(SharedBuildsTest, UnionsEveryReadersKeyValuesBeforeMaterializing) {
  const auto schema =
      arrow::schema({arrow::field("k", arrow::int64()), arrow::field("v", arrow::int64())});
  const auto batch = arrow::RecordBatch::Make(
      schema, 6,
      {testing::Int64s({1, 2, 3, 3, 5, std::nullopt}), testing::Int64s({9, 9, 9, 9, 9, 9})});
  const auto table = std::make_shared<MemoryTable>(schema, arrow::RecordBatchVector{batch}, true);
  const std::string query =
      "WITH g AS (SELECT k, COUNT(*) AS n FROM (SELECT k,v FROM t UNION SELECT k,v FROM t) u "
      "GROUP BY k) SELECT a.k,b.k FROM g a JOIN g b ON a.n=b.n WHERE a.k IN (1,3) ";
  for (const auto& [condition, shared_rows] :
       std::vector<std::pair<std::string, int>>{{" AND b.k IN (3,5)", 3}, {"", 5}}) {
    const auto sql = query + condition + " ORDER BY a.k,b.k";
    table->set_claimed_rows(20'000'000);
    const auto shared = Execute(sql, table);
    ASSERT_TRUE(shared.ok()) << shared.status();
    EXPECT_EQ(shared->shared_rows, shared_rows);
    table->set_claimed_rows(6);
    const auto independent = Execute(sql, table);
    ASSERT_TRUE(independent.ok()) << independent.status();
    EXPECT_EQ(independent->shared_rows, std::nullopt);
    EXPECT_TRUE(independent->rows->Equals(*shared->rows));
  }
}

TEST_F(SharedBuildsTest, KeepsReaderKeyRestrictionsAlongsideLikeAndNotLike) {
  const auto schema =
      arrow::schema({arrow::field("k", arrow::int64()), arrow::field("tag", arrow::binary())});
  const auto batch = arrow::RecordBatch::Make(
      schema, 5,
      {testing::Int64s({1, 2, 3, 4, 5}),
       testing::Strings({"tag\xff", "other", "tagx", std::nullopt, "more"})});
  const auto table = std::make_shared<MemoryTable>(schema, arrow::RecordBatchVector{batch}, true);
  table->set_claimed_rows(20'000'000);
  const auto run = Execute(
      "WITH labels AS (SELECT k, MAX(tag) AS label, COUNT(*) AS n FROM t GROUP BY k) "
      "SELECT a.k,b.k FROM labels a JOIN labels b ON a.n=b.n "
      "WHERE a.k IN (1,2) AND a.label LIKE 'tag_' AND b.k IN (2,3) AND b.label NOT LIKE 'tag_'",
      table);
  ASSERT_TRUE(run.ok()) << run.status();
  EXPECT_EQ(run->shared_rows, 3);
  ASSERT_EQ(run->rows->num_rows(), 1);
  EXPECT_TRUE(run->rows->column(0)->Equals(arrow::ChunkedArray(testing::Int64s({1}))));
  EXPECT_TRUE(run->rows->column(1)->Equals(arrow::ChunkedArray(testing::Int64s({2}))));
}

TEST_F(SharedBuildsTest, AppliesOnlyShortCombinedListsBeforeGrouping) {
  const auto table = KeyedTable();
  table->set_claimed_rows(20'000'000);
  const std::string query =
      "WITH g AS (SELECT k+1 AS bucket, COUNT(*) AS n FROM t GROUP BY k+1) "
      "SELECT a.bucket,b.bucket FROM g a JOIN g b ON a.n=b.n "
      "WHERE a.bucket IN (1,11,12,13,14) AND b.bucket IN ";
  // Only keys 1 and 2 exist in these lists. One overlap keeps the combined size at nine.
  for (const auto& [list, shared_rows] :
       std::vector<std::pair<std::string, int>>{{"(2,15,16,17,11)", 2},
                                                {"(2,15,16,17,18)", 7},
                                                {"(2,11,12,13,14,15,16,17,18,19)", 7},
                                                {"(2,15,16,17,18) AND b.bucket IN (2,14)", 2}}) {
    SCOPED_TRACE(list);
    const auto run = Execute(query + list, table);
    ASSERT_TRUE(run.ok()) << run.status();
    EXPECT_EQ(run->shared_rows, shared_rows);
  }
}

TEST_F(SharedBuildsTest, CollectsReaderKeysThroughAComputedScoreAndAPostAggregateFilter) {
  const auto schema =
      arrow::schema({arrow::field("k", arrow::int64()), arrow::field("v", arrow::int64())});
  const auto batch =
      arrow::RecordBatch::Make(schema, 8,
                               {testing::Int64s({1, 1, 3, 3, 7, 7, std::nullopt, std::nullopt}),
                                testing::Int64s({1, 3, 8, std::nullopt, 6, 6, -2, 0})});
  const auto table = std::make_shared<MemoryTable>(schema, arrow::RecordBatchVector{batch}, true);
  const std::string query =
      "WITH scores AS (SELECT k AS band, COUNT(*) AS n, "
      "CASE WHEN AVG(v)>0e0 THEN AVG(v)*2e0 ELSE 0e0 END AS points FROM t GROUP BY k "
      "HAVING STDDEV_POP(v)>=0e0), "
      "published AS (SELECT points AS score, n AS samples, band AS channel FROM scores) "
      "SELECT a.channel,b.channel,a.score,b.score FROM published a JOIN published b "
      "ON a.samples=b.samples WHERE a.channel=1 ";
  struct Case {
    std::string_view right;
    int64_t shared_groups;
    int64_t pairs;
  };
  for (const Case test : {Case{.right = "AND b.channel=3", .shared_groups = 2, .pairs = 1},
                          Case{.right = "AND b.channel>1", .shared_groups = 4, .pairs = 2},
                          Case{.right = "AND b.channel IS NULL", .shared_groups = 4, .pairs = 1},
                          Case{.right = "", .shared_groups = 4, .pairs = 4}}) {
    SCOPED_TRACE(test.right);
    const auto sql = query + std::string(test.right) + " ORDER BY a.channel,b.channel";
    table->set_claimed_rows(20'000'000);
    const auto shared = Execute(sql, table);
    ASSERT_TRUE(shared.ok()) << shared.status();
    EXPECT_EQ(shared->shared_rows, test.shared_groups);
    EXPECT_EQ(shared->shared_fallbacks, 0);
    table->set_claimed_rows(batch->num_rows());
    const auto control = Execute(sql, table);
    ASSERT_TRUE(control.ok()) << control.status();
    EXPECT_EQ(control->shared_rows, std::nullopt);
    EXPECT_EQ(control->rows->num_rows(), test.pairs);
    EXPECT_TRUE(shared->rows->Equals(*control->rows));
  }
}

std::string ReadTwoScores(std::string_view score) {
  return std::format(
      "WITH scores AS (SELECT k, COUNT(*) AS n, {} AS score FROM t GROUP BY k "
      "HAVING STDDEV_POP(v)>=0e0) SELECT a.score,b.score FROM scores a JOIN scores b "
      "ON a.n=b.n WHERE a.k=1 AND b.k=2",
      score);
}

TEST_F(SharedBuildsTest, WidensBothCoalesceFallbacksForASelectedNullAverage) {
  const auto schema =
      arrow::schema({arrow::field("k", arrow::int64()), arrow::field("v", arrow::int64()),
                     arrow::field("reserve", arrow::decimal128(38, 2))});
  const auto batch =
      arrow::RecordBatch::Make(schema, 6,
                               {testing::Int64s({1, 1, 2, 2, 3, 3}),
                                testing::Int64s({std::nullopt, std::nullopt, 2, 5, 6, 8}),
                                testing::Decimals(38, 2, {"325", "425", "900", "900", "0", "0"})});
  const auto table = std::make_shared<MemoryTable>(schema, arrow::RecordBatchVector{batch}, true);
  table->set_claimed_rows(20'000'000);
  // The HAVING input is present even in the selected group whose average is NULL.
  const auto shared = Execute(
      "WITH g AS (SELECT k, COUNT(*) AS n, COALESCE(AVG(v),MAX(k)) AS whole, "
      "COALESCE(AVG(v),MAX(reserve)) AS scaled FROM t GROUP BY k HAVING STDDEV_POP(k)>=0e0) "
      "SELECT a.whole,a.scaled,b.whole,b.scaled FROM g a JOIN g b ON a.n=b.n "
      "WHERE a.k=1 AND b.k=2",
      table);
  ASSERT_TRUE(shared.ok()) << shared.status();
  EXPECT_EQ(shared->shared_rows, 2);
  EXPECT_EQ(shared->shared_fallbacks, 0);
  ASSERT_EQ(shared->rows->num_rows(), 1);
  ASSERT_EQ(shared->rows->num_columns(), 4);
  const auto value = [&](int col) { return shared->rows->column(col)->GetScalar(0).ValueOrDie(); };
  EXPECT_TRUE(value(0)->Equals(arrow::DoubleScalar(1.0)));
  EXPECT_TRUE(value(1)->Equals(arrow::DoubleScalar(4.25)));
  EXPECT_TRUE(value(2)->Equals(arrow::DoubleScalar(3.5)));
  EXPECT_TRUE(value(3)->Equals(arrow::DoubleScalar(3.5)));
}

TEST_F(SharedBuildsTest, PreservesSelectedValuesThroughNullableAndWidenedCaseArms) {
  const auto table = KeyedTable();
  struct Case {
    std::string_view score;
    int64_t groups = 2;
  };
  for (const auto& [score, groups] :
       {Case{.score = "CASE WHEN MAX(v) IS NULL THEN 17 ELSE MAX(v) END"},
        // The binder represents IS NOT NULL in an expression as a boolean NOT around IS NULL.
        // General boolean expressions are outside the sharing rule's skip-safety proof.
        Case{.score = "CASE WHEN MAX(v) IS NOT NULL THEN COUNT(*) END", .groups = 7},
        Case{.score = "CASE WHEN MAX(v) IN (2,3) THEN COUNT(*) ELSE NULL END"},
        Case{.score = "CASE WHEN MAX(v) NOT IN (1,2) THEN MAX(v) END"},
        Case{.score = "CASE WHEN MAX(v)>MIN(v) THEN MAX(v) END"},
        Case{.score = "CASE WHEN MAX(v) IS DISTINCT FROM MIN(v) THEN 4 ELSE MAX(k) END"},
        Case{.score = "CASE WHEN MAX(v) IS NOT DISTINCT FROM MIN(v) THEN 4 ELSE MAX(k) END"}}) {
    SCOPED_TRACE(score);
    const auto sql = ReadTwoScores(score);
    table->set_claimed_rows(60);
    const auto control = Execute(sql, table);
    ASSERT_TRUE(control.ok()) << control.status();
    ASSERT_EQ(control->rows->num_rows(), 1);
    table->set_claimed_rows(20'000'000);
    const auto shared = Execute(sql, table);
    ASSERT_TRUE(shared.ok()) << shared.status();
    EXPECT_EQ(shared->shared_rows, groups);
    EXPECT_TRUE(shared->rows->Equals(*control->rows));
  }
}

TEST_F(SharedBuildsTest, AReaderKeyCannotHideACheckedCastInAnUnusedGroup) {
  const auto schema =
      arrow::schema({arrow::field("k", arrow::int64()), arrow::field("v", arrow::int64())});
  const auto batch = arrow::RecordBatch::Make(
      schema, 3, {testing::Int64s({1, 2, 3}), testing::Int64s({5, 9, 999'999'999})});
  const auto table = std::make_shared<MemoryTable>(schema, arrow::RecordBatchVector{batch}, true);
  // The first conversion is explicit. The second comes from the CASE result type.
  for (const std::string_view score :
       {"CASE WHEN AVG(v)>0e0 THEN CAST(MAX(v) AS SMALLINT) ELSE 0 END",
        "CASE WHEN CAST(MAX(v) AS SMALLINT)>0 THEN 1 ELSE 0 END",
        "CASE WHEN k=3 THEN MAX(v) ELSE CAST(0 AS DECIMAL(38,30)) END"}) {
    SCOPED_TRACE(score);
    const auto query = ReadTwoScores(score);
    table->set_claimed_rows(3);
    const auto control = Execute(query, table);
    ASSERT_FALSE(control.ok());
    ASSERT_TRUE(control.status().message().contains("cast")) << control.status();
    table->set_claimed_rows(20'000'000);
    const auto shared = Execute(query, table);
    ASSERT_FALSE(shared.ok());
    EXPECT_EQ(shared.status(), control.status());
  }
}

TEST_F(SharedBuildsTest, ReaderKeysAboveAnOrderedLimitCannotChangeItsFirstGroups) {
  const auto table = KeyedTable();  // Keys 0 through 6.
  table->set_claimed_rows(20'000'000);
  const auto shared = Execute(
      "WITH first_two AS (SELECT k, COUNT(*) AS n FROM t GROUP BY k ORDER BY k LIMIT 2) "
      "SELECT a.n,b.n FROM first_two a JOIN first_two b ON a.k=b.k WHERE a.k=5 AND b.k=5",
      table);
  ASSERT_TRUE(shared.ok()) << shared.status();
  EXPECT_EQ(shared->shared_rows, 7);
  EXPECT_EQ(shared->rows->num_rows(), 0);
}

TEST_F(SharedBuildsTest, RejectsIncompleteUnionColumnMapsBeforeSharing) {
  const auto table = KeyedTable();
  table->set_claimed_rows(20'000'000);
  for (const std::string_view op : {"UNION", "UNION ALL"}) {
    const auto valid = PlanOf(
        std::format("WITH g AS (SELECT k, COUNT(*) AS n FROM (SELECT k,v FROM t {} SELECT k,v "
                    "FROM t) u GROUP BY k) SELECT a.k,b.k FROM g a JOIN g b ON a.n=b.n "
                    "WHERE a.k IN (1,3) AND b.k IN (3,5)",
                    op),
        table);
    ASSERT_TRUE(valid.ok()) << valid.status();
    const auto groups = plan::SharedSubPlans(*valid);
    ASSERT_FALSE(groups.empty());
    ASSERT_TRUE(plan::WorthSharing(groups.front()));

    for (const bool missing_list : {true, false}) {
      SCOPED_TRACE(std::format("{} missing list={}", op, missing_list));
      const auto omit = [&](const auto& self,
                            const plan::LogicalNodePtr& node) -> plan::LogicalNodePtr {
        const auto omit_columns = [&](auto set) {
          if (missing_list) {
            set.columns.pop_back();
          } else {
            set.columns.back().clear();
          }
          return std::make_shared<const plan::LogicalNode>(std::move(set));
        };
        if (const auto* set = std::get_if<plan::SetOperationNode>(node.get())) {
          return omit_columns(*set);
        }
        if (const auto* set = std::get_if<plan::UnionAllNode>(node.get())) {
          return omit_columns(*set);
        }
        auto inputs = plan::InputsOf(*node);
        for (auto& input : inputs) {
          input = self(self, input);
        }
        return plan::WithInputs(*node, std::move(inputs));
      };
      auto malformed = *valid;
      malformed.root = omit(omit, malformed.root);
      const auto result = BuildPhysicalPlan(malformed);
      ASSERT_TRUE(result.status().IsInvalid()) << result.status();
      EXPECT_TRUE(
          result.status().message().contains(missing_list ? "column lists for" : "columns for"))
          << result.status();
    }
  }
}

TEST_F(SharedBuildsTest, SharedKeyRestrictionPreservesOverflowInAnUnusedGroup) {
  const auto schema =
      arrow::schema({arrow::field("k", arrow::int64()), arrow::field("v", arrow::int64())});
  const auto batch = arrow::RecordBatch::Make(
      schema, 3, {testing::Int64s({1, 2, 3}), testing::Int64s({2, 3, INT64_MAX})});
  const auto table = std::make_shared<MemoryTable>(schema, arrow::RecordBatchVector{batch}, true);
  table->set_claimed_rows(20'000'000);
  const auto result = Execute(
      "WITH g AS (SELECT k, MAX(x) AS n FROM (SELECT k,v+1 AS x FROM t UNION "
      "SELECT k,v FROM t) u GROUP BY k) SELECT a.k,b.k FROM g a JOIN g b ON a.n=b.n "
      "WHERE a.k=1 AND b.k=2",
      table);
  ASSERT_FALSE(result.ok());
  const auto message = result.status().message();
  EXPECT_TRUE(message.contains("overflow") || message.contains("Overflow")) << result.status();
}

// A reducing shape alone does not make its output small. Sparse, multi-column key domains can
// predict a result beyond the materialization cap, without allocating a large test table.
TEST_F(SharedBuildsTest, RecomputesWhenTheGroupEstimateExceedsTheSharedResultCap) {
  const auto schema =
      arrow::schema({arrow::field("k", arrow::int64()), arrow::field("v", arrow::int64())});
  std::vector<std::optional<int64_t>> keys;
  keys.reserve(8192);
  for (int i = 0; i < 8192; ++i) {
    keys.emplace_back(i % 2 == 0 ? 0 : 100'000'000);
  }
  auto table = std::make_shared<MemoryTable>(
      schema,
      arrow::RecordBatchVector{arrow::RecordBatch::Make(
          schema, 8192, arrow::ArrayVector{testing::Int64s(keys), testing::Int64s(keys)})},
      /*split=*/true);
  table->set_claimed_rows(100'000'000);
  auto run = Execute(
      "WITH g AS (SELECT k, v, COUNT(*) AS n FROM t GROUP BY k, v) "
      "SELECT COUNT(*) FROM g AS a, g AS b WHERE a.k = b.k AND a.v = b.v",
      table);
  ASSERT_TRUE(run.ok()) << run.status().ToString();
  EXPECT_EQ(run->shared_rows, std::nullopt);
  EXPECT_EQ(run->parts_scanned, 2);
  ASSERT_NE(run->rows, nullptr);
  ASSERT_EQ(run->rows->num_rows(), 1);
  EXPECT_EQ(static_cast<const arrow::Int64Array&>(*run->rows->column(0)->chunk(0)).Value(0), 2);
}

TEST_F(SharedBuildsTest, SpendsOneRefinementBudgetAcrossRepeatedAggregates) {
  const auto schema =
      arrow::schema({arrow::field("k", arrow::int64()), arrow::field("v", arrow::int64())});
  std::vector<std::optional<int64_t>> values(8192, 0);
  values.back() = 100'000'000;
  const auto keys = testing::Int64s(values);
  const auto batch = arrow::RecordBatch::Make(schema, keys->length(), {keys, keys});

  // Each disjunction fits the per-filter budget, but eight of them exceed the stage budget.
  std::string filter = "k = 0";
  for (int i = 1; i <= 12; ++i) {
    filter += std::format(" OR (v = {0} AND k <> {0})", i);
  }
  plan::Catalog catalog;
  std::vector<std::shared_ptr<MemoryTable>> tables;
  std::string ctes;
  std::string joins;
  for (int i = 0; i < 8; ++i) {
    auto table = std::make_shared<MemoryTable>(schema, arrow::RecordBatchVector{batch}, true);
    table->set_claimed_rows(100'000'000);
    ASSERT_TRUE(catalog.Register(std::format("t{}", i), table).ok());
    tables.push_back(std::move(table));
    if (i > 0) {
      ctes += ", ";
      joins += std::format(" JOIN g{0} a{0} ON a0.k = a{0}.k", i);
    } else {
      joins = "g0 a0";
    }
    ctes += std::format("g{0} AS (SELECT k, v FROM t{0} WHERE {1} GROUP BY k, v)", i, filter);
    joins += std::format(" JOIN g{0} b{0} ON a{0}.k = b{0}.k", i);
  }
  const auto parsed = sql::Parse(std::format("WITH {} SELECT COUNT(*) FROM {}", ctes, joins));
  ASSERT_TRUE(parsed.has_value());
  const auto bound = plan::Bind(*parsed, catalog, plan::BindOptions{});
  ASSERT_TRUE(bound.ok()) << bound.status().ToString();
  const auto groups = plan::SharedSubPlans(*bound);
  ASSERT_EQ(groups.size(), tables.size());
  for (const auto& group : groups) {
    int exhausted = 0;
    ASSERT_FALSE(plan::WorthSharing(group, exhausted));
    ASSERT_TRUE(plan::WorthSharing(group)) << "every candidate would fit with a fresh budget";
  }
  const auto run = ExecutePlan(*bound, tables.front());
  ASSERT_TRUE(run.ok()) << run.status().ToString();
  EXPECT_EQ(tables.front()->scanned_parts().size(), 1U) << "the first aggregate was shared";
  EXPECT_EQ(tables.back()->scanned_parts().size(), 2U) << "the last aggregate was recomputed";
}

TEST_F(SharedBuildsTest, SharedCostDoesNotExposeOverflowInExcludedGroups) {
  const auto schema = arrow::schema(
      {arrow::field("k", arrow::int64()), arrow::field("v", arrow::decimal128(38, 0))});
  const std::string largest(38, '9');
  const auto batch = arrow::RecordBatch::Make(
      schema, 4,
      arrow::ArrayVector{testing::Int64s({1, 2, 3, 3}),
                         testing::Decimals(38, 0, {"1", "1", largest, largest})});
  const auto table = std::make_shared<MemoryTable>(schema, arrow::RecordBatchVector{batch});
  constexpr std::string_view kFiltered =
      "WITH g AS (SELECT k, SUM(v) AS n FROM t GROUP BY k) "
      "SELECT a.k, b.k FROM g AS a JOIN g AS b ON a.n = b.n WHERE a.k = 1 AND b.k = ";
  for (const int64_t claimed : {4, 20'000'000}) {
    table->set_claimed_rows(claimed);
    auto run = Execute(std::string(kFiltered) + "2", table);
    ASSERT_TRUE(run.ok()) << run.status().ToString();
    ASSERT_EQ(run->rows->num_rows(), 1);
    EXPECT_EQ(static_cast<const arrow::Int64Array&>(*run->rows->column(0)->chunk(0)).Value(0), 1);
    EXPECT_EQ(static_cast<const arrow::Int64Array&>(*run->rows->column(1)->chunk(0)).Value(0), 2);
    auto overflow = Execute(std::string(kFiltered) + "3", table);
    ASSERT_FALSE(overflow.ok());
    EXPECT_NE(overflow.status().ToString().find("overflow"), std::string::npos);

    // IN lists stay on the readers. Pushing their union through SUM would hide key 3's overflow.
    const auto unused = Execute(
        "WITH g AS (SELECT k, SUM(v) AS n FROM t GROUP BY k) "
        "SELECT a.k,b.k FROM g a JOIN g b ON a.n=b.n WHERE a.k IN (1,4) AND b.k IN (2,5)",
        table);
    ASSERT_FALSE(unused.ok());
    EXPECT_TRUE(unused.status().message().contains("SUM overflow")) << unused.status();
  }
}

TEST_F(SharedBuildsTest, SharedCostDoesNotReadAPrunedUnionBranch) {
  const auto schema = arrow::schema({arrow::field("k", arrow::int64())});
  const auto batch =
      arrow::RecordBatch::Make(schema, 1, arrow::ArrayVector{testing::Int64s({4'000'000'000})});
  const auto table =
      std::make_shared<MemoryTable>(schema, arrow::RecordBatchVector{batch}, /*split=*/true);
  table->set_claimed_rows(20'000'000);
  auto run = Execute(
      "WITH g AS (SELECT tag, MAX(v) AS n FROM (SELECT 1 AS tag, k AS v FROM t "
      "UNION ALL SELECT 2 AS tag, k AS v FROM t UNION ALL SELECT 3 AS tag, k*k AS v FROM t) u "
      "GROUP BY tag) SELECT a.tag, b.tag FROM g a JOIN g b ON a.n = b.n "
      "WHERE a.tag = 1 AND b.tag = 2",
      table);
  ASSERT_TRUE(run.ok()) << run.status().ToString();
  EXPECT_EQ(run->rows->num_rows(), 1);
  EXPECT_EQ(run->parts_scanned, 2) << "only the two selected constant-tag branches run";
}

// The gate's second half, which the measurement had to teach: a sub-plan must be *dear* as well as
// reducing. Reading little is cheaper to do twice than to hold -- measured at 1.017x on the one
// query of the workload whose shared join read about a million rows.
TEST_F(SharedBuildsTest, RefusesASubPlanTooCheapToBeWorthHolding) {
  const auto table = KeyedTable();
  table->set_claimed_rows(1'000);
  auto run = Execute(kTwiceReferencedCte, table);
  ASSERT_TRUE(run.ok()) << run.status().ToString();
  EXPECT_EQ(run->parts_scanned, 6) << "read twice: holding this would cost more than it saves";
}

// And the first half: a sub-plan that does not reduce. Two references to a bare table repeat a
// scan, and holding its rows to avoid reading them is the trade the gate exists to refuse -- the
// shape that would otherwise hold 379 million rows of the workload's largest query.
TEST_F(SharedBuildsTest, RefusesARepeatedSubPlanThatDoesNotReduce) {
  const auto table = KeyedTable();
  table->set_claimed_rows(20'000'000);
  auto run = Execute("SELECT COUNT(*) FROM t AS a, t AS b WHERE a.k = b.k", table);
  ASSERT_TRUE(run.ok()) << run.status().ToString();
  EXPECT_EQ(run->parts_scanned, 6) << "a scan is not a sub-plan worth holding the output of";
}

// **The wrong answer this pass once gave.** Two sub-plans over one self-join, one grouping by
// `a.k` and the other by `b.k`, compute different groups -- but a table's field was one origin
// wherever it was read, so they keyed as one, and the second was answered from the first's result:
// SUM(s2.n) came out 72 where it is 48. Each reference's path now tells the two sides apart; what
// the two sub-plans really repeat is the join under them, which reduces nothing and is not held.
TEST_F(SharedBuildsTest, AnswersTwoSidesOfASelfJoinEachFromItsOwnRows) {
  constexpr std::string_view kQuery =
      "SELECT COUNT(*), SUM(s1.n), SUM(s2.n) FROM "
      "(SELECT a.k AS g, COUNT(*) AS n FROM t AS a, t AS b WHERE a.k = b.v AND b.v > 3 "
      "GROUP BY a.k) AS s1, "
      "(SELECT b.k AS g, COUNT(*) AS n FROM t AS a, t AS b WHERE a.k = b.v AND b.v > 3 "
      "GROUP BY b.k) AS s2 "
      "WHERE s1.g = s2.g";
  const auto shared = KeyedTable();
  shared->set_claimed_rows(20'000'000);  // past the gate, while holding 60 rows
  auto with = Execute(kQuery, shared);
  ASSERT_TRUE(with.ok()) << with.status().ToString();
  auto without = Execute(kQuery, KeyedTable());
  ASSERT_TRUE(without.ok()) << without.status().ToString();
  ASSERT_NE(with->rows, nullptr);
  ASSERT_NE(without->rows, nullptr);
  EXPECT_EQ(with->shared_rows, std::nullopt) << "the two sub-plans differ; their join is a join";
  EXPECT_TRUE(with->rows->Equals(*without->rows)) << with->rows->ToString() << "\nagainst\n"
                                                  << without->rows->ToString();
  const auto& sums = static_cast<const arrow::Decimal128Array&>(*with->rows->column(2)->chunk(0));
  EXPECT_EQ(sums.FormatValue(0), "48") << "SUM(s2.n): each b row with v in 4..6 meets 8 a rows";
}

// Nothing shared, nothing changed: the plan of a query with no repetition is the plan it was.
TEST_F(SharedBuildsTest, LeavesAPlanWithNoRepetitionAlone) {
  const auto table = KeyedTable();
  table->set_claimed_rows(20'000'000);
  auto run = Execute("SELECT k, COUNT(*) AS n FROM t GROUP BY k", table);
  ASSERT_TRUE(run.ok()) << run.status().ToString();
  EXPECT_EQ(run->parts_scanned, 3);
}

// ---- UnionAll (ADR 0036): hand-built, since no query reaches a union before its binder does ----

plan::LogicalNodePtr Node(plan::LogicalNode node) {
  return std::make_shared<const plan::LogicalNode>(std::move(node));
}

plan::BoundColumn Bigint(plan::ColumnId id) {
  return plan::BoundColumn{.id = id, .name = "c", .type = plan::LogicalType::kBigInt};
}

plan::ColumnId Id(std::uint32_t id) { return plan::ColumnId{id}; }

// GROUP BY k of `table` with COUNT(*), or MIN(v) with `min`, under a Project: an aggregate-topped
// sub-plan, the shape the gate admits. Its columns are minted from `first` on.
plan::LogicalNodePtr Grouped(const std::shared_ptr<MemoryTable>& table, std::uint32_t first,
                             bool min = false) {
  const plan::AggregateCall call =
      min ? plan::AggregateCall{.kind = plan::AggKind::kMin,
                                .arg = Bigint(Id(first + 1)),
                                .type = plan::LogicalType::kBigInt,
                                .id = Id(first + 3)}
          : plan::AggregateCall{.kind = plan::AggKind::kCountStar, .id = Id(first + 3)};
  return Node(
      plan::ProjectNode{.input = Node(plan::GroupAggregateNode{
                            .input = Node(plan::ScanNode{.table = table,
                                                         .table_name = "t",
                                                         .fields = {0, 1},
                                                         .ids = {Id(first), Id(first + 1)}}),
                            .keys = {Bigint(Id(first))},
                            .key_ids = {Id(first + 2)},
                            .aggregates = {call}}),
                        .columns = {Bigint(Id(first + 2)), Bigint(Id(first + 3))},
                        .constants = {},
                        .ids = {Id(first + 4), Id(first + 5)}});
}

TEST_F(SharedBuildsTest, AReaderAliasThatReadsTheCountKeepsEveryMatchingGroup) {
  const auto schema =
      arrow::schema({arrow::field("k", arrow::int64()), arrow::field("v", arrow::int64())});
  const auto keys = testing::Int64s({1, 1, 2, 3});
  auto table = std::make_shared<MemoryTable>(
      schema, arrow::RecordBatchVector{arrow::RecordBatch::Make(schema, 4, {keys, keys})}, true);
  const auto group = [&](std::uint32_t first) {
    return std::get<plan::ProjectNode>(*Grouped(table, first)).input;
  };
  const auto select_one = [](plan::LogicalNodePtr input, plan::ColumnId id) {
    return Node(plan::FilterNode{
        .input = std::move(input),
        .predicates = {{.column = Bigint(id),
                        .constant = {.type = plan::LogicalType::kBigInt, .value = Int128{1}},
                        .span = {}}}});
  };
  auto alias = Bigint(Id(12));  // The ID names k, but the physical slot reads COUNT(*).
  alias.index = 1;
  const auto projected =
      Node(plan::ProjectNode{.input = group(10), .columns = {alias}, .ids = {Id(14)}});
  const auto join = Node(plan::JoinNode{.kind = plan::JoinKind::kInner,
                                        .left = select_one(projected, Id(14)),
                                        .right = select_one(group(20), Id(22)),
                                        .keys = {{.left = Bigint(Id(14)), .right = Bigint(Id(22))}},
                                        .build = plan::BuildSide::kRight});
  const auto root =
      Node(plan::ProjectNode{.input = join, .columns = {Bigint(Id(14))}, .ids = {Id(30)}});
  const plan::LogicalPlan query{.root = root, .output = {{.name = "alias", .id = Id(30)}}};
  const auto control = ExecutePlan(query, table);
  ASSERT_TRUE(control.ok()) << control.status();
  ASSERT_EQ(control->rows->num_rows(), 2);  // Keys 2 and 3 each occur once.
  table->set_claimed_rows(20'000'000);
  const auto shared = ExecutePlan(query, table);
  ASSERT_TRUE(shared.ok()) << shared.status();
  EXPECT_EQ(shared->shared_rows, 3);
  EXPECT_TRUE(shared->rows->Equals(*control->rows));
}

// The union of `a` and `b`, its two columns #first and #first + 1.
plan::LogicalNodePtr UnionOf(const plan::LogicalNodePtr& a, const plan::LogicalNodePtr& b,
                             std::uint32_t first) {
  plan::UnionAllNode node{.inputs = {a, b}, .columns = {}, .ids = {Id(first), Id(first + 1)}};
  for (const plan::LogicalNodePtr& input : node.inputs) {
    std::vector<plan::BoundColumn>& columns = node.columns.emplace_back();
    for (const plan::ColumnId id : plan::OutputIds(*input)) {
      columns.push_back(Bigint(id));
    }
  }
  return Node(std::move(node));
}

// `node` sorted by its first column: a node with no key, which a shared sub-plan never holds.
plan::LogicalNodePtr Sorted(const plan::LogicalNodePtr& node) {
  return Node(plan::SortNode{
      .input = node, .keys = {plan::SortKey{.column = Bigint(plan::OutputIds(*node).front())}}});
}

// COUNT(*) of `left` joined to `right` on their first columns.
plan::LogicalPlan Joined(const plan::LogicalNodePtr& left, const plan::LogicalNodePtr& right) {
  const auto join =
      Node(plan::JoinNode{.kind = plan::JoinKind::kInner,
                          .left = left,
                          .right = right,
                          .keys = {plan::JoinKey{.left = Bigint(plan::OutputIds(*left).front()),
                                                 .right = Bigint(plan::OutputIds(*right).front())}},
                          .build = plan::BuildSide::kRight});
  const auto count = Node(plan::AggregateNode{
      .input = join,
      .aggregates = {plan::AggregateCall{.kind = plan::AggKind::kCountStar, .id = Id(900)}}});
  return plan::ResolvePositions(
      plan::LogicalPlan{.root = count,
                        .output = {plan::OutputColumn{
                            .name = "n", .type = plan::LogicalType::kBigInt, .id = Id(900)}}});
}

TEST_F(SharedBuildsTest, ASharedAggregateProbeKeepsASemiOrAntiJoinsLeftBuild) {
  for (const plan::JoinKind kind : {plan::JoinKind::kSemi, plan::JoinKind::kAnti}) {
    SCOPED_TRACE(static_cast<int>(kind));
    const auto table = KeyedTable();
    table->set_claimed_rows(20'000'000);  // sharing is worth it, while the fixture holds 60 rows
    const auto schema =
        arrow::schema({arrow::field("k", arrow::int64()), arrow::field("v", arrow::int64())});
    const auto outer = std::make_shared<MemoryTable>(
        schema, arrow::RecordBatchVector{arrow::RecordBatch::Make(
                    schema, 5,
                    arrow::ArrayVector{testing::Int64s({0, 0, 6, 7, std::nullopt}),
                                       testing::Int64s({10, 11, 60, 70, 99})})});
    const auto scan = Node(plan::ScanNode{
        .table = outer, .table_name = "outer", .fields = {0, 1}, .ids = {Id(100), Id(101)}});
    const auto join = [kind](const plan::LogicalNodePtr& input, const plan::LogicalNodePtr& probe) {
      return Node(
          plan::JoinNode{.kind = kind,
                         .left = input,
                         .right = probe,
                         .keys = {plan::JoinKey{.left = Bigint(Id(100)),
                                                .right = Bigint(plan::OutputIds(*probe).front())}},
                         .build = plan::BuildSide::kLeft});
    };
    const auto root = join(join(scan, Grouped(table, 1)), Grouped(table, 11));
    const auto logical_plan = plan::ResolvePositions(plan::LogicalPlan{
        .root = root,
        .output = {{.name = "k", .type = plan::LogicalType::kBigInt, .id = Id(100)},
                   {.name = "v", .type = plan::LogicalType::kBigInt, .id = Id(101)}}});
    ProfileNode profile;
    auto op = BuildPhysicalPlan(logical_plan, &profile);
    ASSERT_TRUE(op.ok()) << op.status().ToString();
    int left_builds = 0;
    const auto inspect = [&left_builds](const ProfileNode& node, auto&& self) -> void {
      if (node.name() == "RightJoin") {
        EXPECT_TRUE(node.detail().contains("build=left"));
        ++left_builds;
      }
      for (const ProfileNode* child : node.children()) {
        self(*child, self);
      }
    };
    inspect(profile, inspect);
    EXPECT_EQ(left_builds, 2) << "normalize after aggregation probes become shared scans";

    MemoryBudget budget(int64_t{1024} * 1024 * 1024);
    ExecContext ctx{.pool = arrow::default_memory_pool(), .budget = &budget};
    const auto rows = Drain(**op, ctx);
    ASSERT_TRUE(rows.ok()) << rows.status().ToString();
    std::optional<int64_t> shared_rows;
    for (const ProfileMetric& metric : profile.metrics()) {
      if (metric.name == "shared_rows") {
        shared_rows = metric.value;
      }
    }
    EXPECT_EQ(shared_rows, 7);
    EXPECT_EQ(table->scanned_parts().size(), 3U) << "the repeated aggregation is computed once";

    const auto keys = testing::Int64Column(**rows, 0);
    const auto values = testing::Int64Column(**rows, 1);
    using Row = std::pair<std::optional<int64_t>, std::optional<int64_t>>;
    std::vector<Row> actual;
    actual.reserve(keys.size());
    for (std::size_t i = 0; i < keys.size(); ++i) {
      actual.emplace_back(keys[i], values[i]);
    }
    std::vector<Row> expected = kind == plan::JoinKind::kSemi
                                    ? std::vector<Row>{{0, 10}, {0, 11}, {6, 60}}
                                    : std::vector<Row>{{7, 70}, {std::nullopt, 99}};
    std::ranges::sort(actual);
    std::ranges::sort(expected);
    EXPECT_EQ(actual, expected);
  }
}

TEST_F(SharedBuildsTest, SharesEligibleProducersInsideUnionBranches) {
  const auto table = KeyedTable();
  table->set_claimed_rows(20'000'000);  // past the gate, while holding 60 rows
  auto control = ExecutePlan(Joined(Grouped(table, 1), Grouped(table, 11)), table);
  ASSERT_TRUE(control.ok()) << control.status().ToString();
  EXPECT_EQ(control->shared_rows, 7) << "outside a union the sub-plan is shared";
  ASSERT_NE(control->rows, nullptr);
  EXPECT_EQ(testing::SingleInt64(*control->rows), 7);

  struct Case {
    std::string name;
    plan::LogicalPlan plan;
  };
  const std::vector<Case> cases = {
      {.name = "repeated whole",
       .plan = Joined(UnionOf(Grouped(table, 100), Grouped(table, 110, true), 120),
                      UnionOf(Grouped(table, 200), Grouped(table, 210, true), 220))},
      {.name = "the branches in another order",
       .plan = Joined(UnionOf(Grouped(table, 100), Grouped(table, 110, true), 120),
                      UnionOf(Grouped(table, 210, true), Grouped(table, 200), 220))},
      {.name = "a branch under a node without a key",
       .plan = Joined(UnionOf(Sorted(Grouped(table, 100)), Grouped(table, 110, true), 120),
                      UnionOf(Sorted(Grouped(table, 200)), Grouped(table, 210, true), 220))},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.name);
    auto run = ExecutePlan(c.plan, table);
    ASSERT_TRUE(run.ok()) << run.status().ToString();
    EXPECT_EQ(run->shared_rows, 14);
    ASSERT_NE(run->rows, nullptr);
    EXPECT_EQ(testing::SingleInt64(*run->rows), 28) << "each key twice on either side";
  }
}

// The union of `a` alone, its two columns #first and #first + 1: a union whose other branches the
// conditions on it ruled out (ADR 0036's X7a).
plan::LogicalNodePtr UnionOfOne(const plan::LogicalNodePtr& a, std::uint32_t first) {
  plan::UnionAllNode node{.inputs = {a}, .columns = {{}}, .ids = {Id(first), Id(first + 1)}};
  for (const plan::ColumnId id : plan::OutputIds(*a)) {
    node.columns[0].push_back(Bigint(id));
  }
  return Node(std::move(node));
}

TEST_F(SharedBuildsTest, SharesAUnionOfOneInputOrItsRepeatedBranch) {
  const auto table = KeyedTable();
  table->set_claimed_rows(20'000'000);
  for (const plan::LogicalPlan& plan :
       {Joined(UnionOfOne(Grouped(table, 100), 120), UnionOfOne(Grouped(table, 200), 220)),
        Joined(UnionOfOne(Grouped(table, 100), 120), Grouped(table, 200))}) {
    auto run = ExecutePlan(plan, table);
    ASSERT_TRUE(run.ok()) << run.status().ToString();
    EXPECT_EQ(run->shared_rows, 7);
    ASSERT_NE(run->rows, nullptr);
    EXPECT_EQ(testing::SingleInt64(*run->rows), 7) << "each key once on either side";
  }
}

TEST_F(SharedBuildsTest, ThePrunedReferencesOfAUnionCteShareTheirSurvivingBranch) {
  const auto table = KeyedTable();
  table->set_claimed_rows(20'000'000);
  auto run = Execute(
      "WITH v AS (SELECT k, 'a' AS tag FROM t GROUP BY k UNION ALL SELECT k, 'b' AS tag FROM t "
      "GROUP BY k) SELECT COUNT(*) FROM v AS p, v AS q WHERE p.k = q.k AND p.tag = 'a' AND "
      "q.tag = 'a'",
      table);
  ASSERT_TRUE(run.ok()) << run.status().ToString();
  EXPECT_EQ(run->shared_rows, 7);
  EXPECT_EQ(run->parts_scanned, 3);
  ASSERT_NE(run->rows, nullptr);
  EXPECT_EQ(testing::SingleInt64(*run->rows), 7);
}

TEST_F(SharedBuildsTest, SharesAnIntersectJoinAndScalarAggregateAcrossUnionBranches) {
  constexpr std::string_view kQuery =
      "WITH m AS (SELECT k FROM t INTERSECT SELECT v FROM t), "
      "g AS (SELECT d.k FROM d JOIN m ON d.k = m.k), "
      "s AS (SELECT MAX(v) AS cutoff FROM t) "
      "SELECT g.k FROM g, s WHERE g.k < s.cutoff "
      "UNION ALL SELECT g.k FROM g, s WHERE g.k >= s.cutoff";
  const auto parsed = sql::Parse(kQuery);
  ASSERT_TRUE(parsed.has_value());

  const auto run = [&](const std::shared_ptr<MemoryTable>& fact) -> arrow::Result<ExecutionResult> {
    plan::Catalog catalog;
    ARROW_RETURN_NOT_OK(catalog.Register("t", fact));
    ARROW_RETURN_NOT_OK(catalog.Register("d", KeyedTable()));
    ARROW_ASSIGN_OR_RAISE(const auto bound, plan::Bind(*parsed, catalog, plan::BindOptions{}));
    ARROW_ASSIGN_OR_RAISE(const auto optimized, plan::Optimize(bound));
    return ExecutePlan(optimized, fact);
  };
  const auto fact = KeyedTable();
  fact->set_claimed_rows(100'000'000);
  const auto shared = run(fact);
  const auto control = run(KeyedTable());
  ASSERT_TRUE(shared.ok()) << shared.status();
  ASSERT_TRUE(control.ok()) << control.status();
  EXPECT_EQ(shared->shared_rows, 61);  // 60 projected dimension rows and one scalar
  EXPECT_EQ(shared->parts_scanned, 9);
  EXPECT_EQ(control->parts_scanned, 18);
  auto shared_keys = testing::Int64Column(*shared->rows, 0);
  auto control_keys = testing::Int64Column(*control->rows, 0);
  std::ranges::sort(shared_keys);
  std::ranges::sort(control_keys);
  EXPECT_EQ(shared_keys, control_keys);
}

TEST_F(SharedBuildsTest, LimitStopsBeforeALaterBatchWithAFailingCast) {
  const auto schema = arrow::schema({arrow::field("k", arrow::int64())});
  const auto batch = arrow::RecordBatch::Make(schema, 2, {testing::Int64s({7, 100'000})});
  const auto table =
      std::make_shared<MemoryTable>(schema, arrow::RecordBatchVector{batch}, /*split=*/true);
  table->set_claimed_rows(20'000'000);
  constexpr std::string_view kQuery =
      "WITH g AS (SELECT MIN(k) AS v FROM t UNION ALL SELECT MAX(k) AS v FROM t) "
      "SELECT n FROM (SELECT CAST(v AS SMALLINT) AS n FROM g UNION ALL SELECT v FROM g) u";
  const auto limited = Execute(std::string(kQuery) + " LIMIT 1", table);
  ASSERT_TRUE(limited.ok()) << limited.status();
  EXPECT_EQ(testing::SingleInt64(*limited->rows), 7);
  EXPECT_EQ(limited->parts_scanned, 1);

  // Reading the second batch really does evaluate the narrowing cast.
  const auto unlimited = Execute(kQuery, table);
  ASSERT_FALSE(unlimited.ok());
  EXPECT_TRUE(unlimited.status().message().contains("out of range")) << unlimited.status();
}

TEST_F(SharedBuildsTest, LimitLeavesSharedProducersInLaterUnionBranchesUnopened) {
  const auto table = KeyedTable();
  table->set_claimed_rows(20'000'000);
  table->set_track_scan_contexts(true);
  constexpr std::string_view kQuery =
      "WITH g AS (SELECT k, MAX(v) AS n FROM t GROUP BY k) "
      "SELECT k FROM (SELECT k FROM t UNION ALL SELECT k FROM g WHERE n > 0 "
      "UNION ALL SELECT k FROM g WHERE n < 0) u";
  const auto limited = Execute(std::string(kQuery) + " LIMIT 1", table);
  ASSERT_TRUE(limited.ok()) << limited.status();
  ASSERT_EQ(limited->rows->num_rows(), 1);
  ASSERT_FALSE(table->scan_calls().empty());
  for (const auto& scan : table->scan_calls()) {
    EXPECT_EQ(scan.fields, std::vector<int>({0})) << "only the first branch may open its scans";
  }

  // Even an ordered LIMIT 0 can skip the sort and every input it would read.
  table->ClearScanCalls();
  const auto empty = Execute(std::string(kQuery) + " ORDER BY k LIMIT 0", table);
  ASSERT_TRUE(empty.ok()) << empty.status();
  EXPECT_EQ(empty->rows->num_rows(), 0);
  EXPECT_TRUE(table->scan_calls().empty());

  // A positive ordered limit needs every union branch to determine its first row.
  const auto ordered = Execute(std::string(kQuery) + " ORDER BY k LIMIT 1", table);
  ASSERT_TRUE(ordered.ok()) << ordered.status();
  EXPECT_EQ(ordered->shared_rows, 7);
}

TEST_F(SharedBuildsTest, OnlyADemandedUnionBranchCanReportItsOverflow) {
  const auto schema = arrow::schema(
      {arrow::field("k", arrow::int64()), arrow::field("v", arrow::decimal128(38, 0))});
  const std::string largest(38, '9');
  const auto batch = arrow::RecordBatch::Make(
      schema, 2, {testing::Int64s({7, 7}), testing::Decimals(38, 0, {largest, largest})});
  const auto table =
      std::make_shared<MemoryTable>(schema, arrow::RecordBatchVector{batch}, /*split=*/true);
  table->set_claimed_rows(20'000'000);
  constexpr std::string_view kQuery =
      "WITH g AS (SELECT k, SUM(v) AS n FROM t GROUP BY k) "
      "SELECT k FROM (SELECT k FROM t UNION ALL SELECT k FROM g WHERE n > 0 "
      "UNION ALL SELECT k FROM g WHERE n < 0) u";
  const auto limited = Execute(std::string(kQuery) + " LIMIT 1", table);
  ASSERT_TRUE(limited.ok()) << limited.status();
  EXPECT_EQ(testing::SingleInt64(*limited->rows), 7);
  EXPECT_EQ(limited->shared_fallbacks, 0);
  EXPECT_EQ(limited->parts_scanned, 1);
  const auto unlimited = Execute(kQuery, table);
  ASSERT_FALSE(unlimited.ok());
  EXPECT_TRUE(unlimited.status().message().contains("overflow"));
}

}  // namespace
}  // namespace antb1::exec
