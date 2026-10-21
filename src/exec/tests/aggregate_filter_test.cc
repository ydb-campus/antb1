// A key filter published by a join, applied below the grouped aggregate it reads
// (docs/adr/0029-dynamic-join-filters.md, D5).
//
// These bind SQL rather than hand-building plans, for the reason shared_builds_test.cc gives: the
// filter is carried down by ColumnId, and a hand-built plan leaves ids unset, so nothing would
// match and the test would pass while testing nothing.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <gtest/gtest.h>

#include "antb1/exec/memory_budget.h"
#include "antb1/exec/operator.h"
#include "antb1/exec/physical_planner.h"
#include "antb1/exec/profile.h"
#include "antb1/plan/binder.h"
#include "antb1/plan/catalog.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/optimizer.h"
#include "antb1/plan/plan_options.h"
#include "antb1/sql/parser.h"

#include "exec_test_util.h"

namespace antb1::exec {
namespace {

using testing::MemoryTable;

// Operators call Arrow compute kernels, so the suite needs arrow::compute::Initialize (ExecTest).
// Without the fixture these pass only when another test in the binary has already initialized it.
class AggregateFilterTest : public testing::ExecTest {};

// A fact table: `k` repeats, so a GROUP BY reduces, and `v` is what gets aggregated.
std::shared_ptr<MemoryTable> Facts() {
  const auto schema =
      arrow::schema({arrow::field("k", arrow::int64()), arrow::field("v", arrow::int64())});
  arrow::RecordBatchVector batches;
  for (int64_t part = 0; part < 3; ++part) {
    std::vector<std::optional<int64_t>> keys;
    std::vector<std::optional<int64_t>> values;
    for (int64_t r = 0; r < 30; ++r) {
      keys.emplace_back(((part * 30) + r) % 10);  // 10 groups over 90 rows
      values.emplace_back(r);
    }
    batches.push_back(arrow::RecordBatch::Make(
        schema, 30, arrow::ArrayVector{testing::Int64s(keys), testing::Int64s(values)}));
  }
  return std::make_shared<MemoryTable>(schema, std::move(batches), /*split=*/true);
}

// A dimension: `dk` joins the fact's `k`, and `flag` selects two of its ten rows.
std::shared_ptr<MemoryTable> Dimension() {
  const auto schema =
      arrow::schema({arrow::field("dk", arrow::int64()), arrow::field("flag", arrow::int64())});
  std::vector<std::optional<int64_t>> keys;
  std::vector<std::optional<int64_t>> flags;
  for (int64_t i = 0; i < 10; ++i) {
    keys.emplace_back(i);
    flags.emplace_back(i < 2 ? 1 : 0);
  }
  arrow::RecordBatchVector batches{arrow::RecordBatch::Make(
      schema, 10, arrow::ArrayVector{testing::Int64s(keys), testing::Int64s(flags)})};
  return std::make_shared<MemoryTable>(schema, std::move(batches), /*split=*/true);
}

struct Run {
  std::shared_ptr<arrow::Table> rows;
  int64_t dropped = 0;  // the rows every key filter of the plan rejected
};

// Every `key_dropped` of the profile, summed: the filter under the aggregate is the only one these
// plans can attach, so a non-zero total means it fired.
int64_t DroppedIn(const ProfileNode& node) {
  int64_t dropped = 0;
  for (const ProfileMetric& metric : node.metrics()) {
    if (metric.name == "key_dropped") {
      dropped += metric.value;
    }
  }
  for (const ProfileNode* child : node.children()) {
    dropped += DroppedIn(*child);
  }
  return dropped;
}

arrow::Result<Run> Execute(std::string_view sql, const std::shared_ptr<MemoryTable>& facts,
                           const std::shared_ptr<MemoryTable>& dimension) {
  plan::Catalog catalog;
  ARROW_RETURN_NOT_OK(catalog.Register("f", facts));
  ARROW_RETURN_NOT_OK(catalog.Register("d", dimension));
  const auto parsed = sql::Parse(sql);
  if (!parsed.has_value()) {
    return arrow::Status::Invalid("parse failed");
  }
  ARROW_ASSIGN_OR_RAISE(const plan::LogicalPlan bound,
                        plan::Bind(*parsed, catalog, plan::BindOptions{}));
  ARROW_ASSIGN_OR_RAISE(const plan::LogicalPlan plan, plan::Optimize(bound));
  ProfileNode profile;
  ARROW_ASSIGN_OR_RAISE(std::unique_ptr<Operator> op, BuildPhysicalPlan(plan, &profile));
  MemoryBudget budget(int64_t{1024} * 1024 * 1024);
  ExecContext ctx{.pool = arrow::default_memory_pool(), .budget = &budget};
  ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::Table> rows, Drain(*op, ctx));
  return Run{.rows = std::move(rows), .dropped = DroppedIn(profile)};
}

// The shape: a grouped aggregate joined to a filtered dimension on the key it groups by. Two of
// the dimension's ten keys pass, so eight of the aggregate's ten groups are built and discarded --
// unless the join's published keys reach the aggregate's own input first.
constexpr std::string_view kAggregateUnderJoin =
    "SELECT COUNT(*) FROM (SELECT k, SUM(v) AS s FROM f GROUP BY k) AS g, d "
    "WHERE g.k = d.dk AND d.flag = 1";

// The two arms are the same query over the same rows, with the fact table claiming enough rows for
// the share gate to admit the filter and then too few. What is compared is the **answer**: the
// planner is free to choose differently between the arms, which is the point -- filtering the
// aggregate's input must not change what any plan answers.
TEST_F(AggregateFilterTest, FiltersTheAggregatesInputAndKeepsTheAnswer) {
  const auto facts = Facts();
  facts->set_claimed_rows(20'000'000);  // past the build-share gate, while holding 90 rows
  auto with = Execute(kAggregateUnderJoin, facts, Dimension());
  ASSERT_TRUE(with.ok()) << with.status().ToString();

  const auto few = Facts();
  few->set_claimed_rows(16);  // a two-row build is not a sixteenth of this: the gate refuses
  auto without = Execute(kAggregateUnderJoin, few, Dimension());
  ASSERT_TRUE(without.ok()) << without.status().ToString();

  EXPECT_GT(with->dropped, 0) << "the filter reached the aggregate's input";
  EXPECT_EQ(without->dropped, 0) << "and is not attached when the build is too large a share";
  ASSERT_NE(with->rows, nullptr);
  ASSERT_NE(without->rows, nullptr);
  EXPECT_TRUE(with->rows->Equals(*without->rows))
      << "filtering the input changes which groups are built, never the answer\n"
      << with->rows->ToString() << "\nagainst\n"
      << without->rows->ToString();
}

// A Filter between the aggregate and its scan. The field carrying the offer says a Filter passes it
// on "as it is", and it must: a Filter only removes rows, so it neither mints nor moves a ColumnId
// and the key the aggregate groups by is still the one the join published. The predicate here is
// over two columns, which the scan cannot apply, so the Filter stays an operator of its own -- and
// it keeps every row, so both arms answer exactly what they answer without it.
TEST_F(AggregateFilterTest, CarriesThroughAFilterItCannotPush) {
  constexpr std::string_view kQuery =
      "SELECT COUNT(*) FROM (SELECT k, SUM(v) AS s FROM f WHERE v >= v GROUP BY k) AS g, d "
      "WHERE g.k = d.dk AND d.flag = 1";
  const auto facts = Facts();
  facts->set_claimed_rows(20'000'000);
  auto with = Execute(kQuery, facts, Dimension());
  ASSERT_TRUE(with.ok()) << with.status().ToString();

  const auto few = Facts();
  few->set_claimed_rows(16);
  auto without = Execute(kQuery, few, Dimension());
  ASSERT_TRUE(without.ok()) << without.status().ToString();

  EXPECT_GT(with->dropped, 0) << "the offer did not survive the Filter between the two";
  EXPECT_EQ(without->dropped, 0);
  ASSERT_NE(with->rows, nullptr);
  ASSERT_NE(without->rows, nullptr);
  EXPECT_TRUE(with->rows->Equals(*without->rows)) << with->rows->ToString() << "\nagainst\n"
                                                  << without->rows->ToString();
}

// The limitation the carry above stops at, pinned so that lifting it is a visible change. When the
// Filter's predicates go into the scan instead (ADR 0020), they hold the one filter slot a scan
// takes, and the published keys have nowhere to attach. Lifting this needs a scan filter that is
// the conjunction of two -- the predicates and the keys -- which is a decision of its own, not a
// consequence of this one. The answer is right either way, which is why only the metric moves.
TEST_F(AggregateFilterTest, YieldsToPredicatesPushedIntoTheScan) {
  constexpr std::string_view kQuery =
      "SELECT COUNT(*) FROM (SELECT k, SUM(v) AS s FROM f WHERE v >= 0 GROUP BY k) AS g, d "
      "WHERE g.k = d.dk AND d.flag = 1";
  const auto facts = Facts();
  facts->set_claimed_rows(20'000'000);  // the share gate would admit it
  auto run = Execute(kQuery, facts, Dimension());
  ASSERT_TRUE(run.ok()) << run.status().ToString();
  EXPECT_EQ(run->dropped, 0) << "a scan takes one filter and the pushed predicate holds it";

  auto control = Execute(kQuery, Facts(), Dimension());
  ASSERT_TRUE(control.ok()) << control.status().ToString();
  ASSERT_NE(run->rows, nullptr);
  ASSERT_NE(control->rows, nullptr);
  EXPECT_TRUE(run->rows->Equals(*control->rows));
}

// The condition the whole rewrite rests on: the filter may only be applied to a column the
// aggregate **groups by**, because then it drops whole groups. Here the join reads the aggregate's
// *value* instead, so the key is not among its grouping keys and nothing may be attached -- on any
// other column the filter would drop rows inside surviving groups and corrupt their sums.
TEST_F(AggregateFilterTest, RefusesAKeyTheAggregateDoesNotGroupBy) {
  const auto facts = Facts();
  facts->set_claimed_rows(20'000'000);
  auto run = Execute(
      "SELECT COUNT(*) FROM (SELECT k, SUM(v) AS s FROM f GROUP BY k) AS g, d "
      "WHERE g.s = d.dk AND d.flag = 1",
      facts, Dimension());
  ASSERT_TRUE(run.ok()) << run.status().ToString();
  EXPECT_EQ(run->dropped, 0) << "a join on the aggregate's value must reach nothing below it";
  ASSERT_NE(run->rows, nullptr);

  auto control = Execute(
      "SELECT COUNT(*) FROM (SELECT k, SUM(v) AS s FROM f GROUP BY k) AS g, d "
      "WHERE g.s = d.dk AND d.flag = 1",
      Facts(), Dimension());
  ASSERT_TRUE(control.ok()) << control.status().ToString();
  EXPECT_TRUE(run->rows->Equals(*control->rows)) << "and the sums are the sums";
}

// The refusal above, now that a Filter no longer hides it. Before the carry, a Filter between the
// join and the aggregate stopped every offer, so this combination never reached the check that the
// key must be one the aggregate groups by; it does now, and it must still refuse. On the
// aggregate's value the filter would drop rows inside surviving groups and corrupt their sums,
// which the Filter in between does nothing to make safe.
TEST_F(AggregateFilterTest, RefusesAKeyItDoesNotGroupByThroughAFilterToo) {
  constexpr std::string_view kQuery =
      "SELECT COUNT(*) FROM (SELECT k, SUM(v) AS s FROM f WHERE v >= v GROUP BY k) AS g, d "
      "WHERE g.s = d.dk AND d.flag = 1";
  const auto facts = Facts();
  facts->set_claimed_rows(20'000'000);
  auto run = Execute(kQuery, facts, Dimension());
  ASSERT_TRUE(run.ok()) << run.status().ToString();
  EXPECT_EQ(run->dropped, 0) << "a join on the aggregate's value must reach nothing below it";

  auto control = Execute(kQuery, Facts(), Dimension());
  ASSERT_TRUE(control.ok()) << control.status().ToString();
  ASSERT_NE(run->rows, nullptr);
  ASSERT_NE(control->rows, nullptr);
  EXPECT_TRUE(run->rows->Equals(*control->rows)) << "and the sums are the sums";
}

// A join that emits an unmatched probe row keeps every one of them, so dropping rows from the
// aggregate's input would drop rows from the answer. D1's rule, applied one level further down.
TEST_F(AggregateFilterTest, RefusesAJoinThatKeepsUnmatchedRows) {
  const auto facts = Facts();
  facts->set_claimed_rows(20'000'000);
  auto run = Execute(
      "SELECT COUNT(*) FROM (SELECT k, SUM(v) AS s FROM f GROUP BY k) AS g "
      "LEFT JOIN d ON g.k = d.dk AND d.flag = 1",
      facts, Dimension());
  ASSERT_TRUE(run.ok()) << run.status().ToString();
  EXPECT_EQ(run->dropped, 0) << "a left join keeps its unmatched rows, so none may be dropped";
}

// Nothing to carry, nothing carried: no join over the aggregate at all.
TEST_F(AggregateFilterTest, AttachesNothingWithoutAJoinAbove) {
  const auto facts = Facts();
  facts->set_claimed_rows(20'000'000);
  auto run = Execute("SELECT k, SUM(v) AS s FROM f GROUP BY k", facts, Dimension());
  ASSERT_TRUE(run.ok()) << run.status().ToString();
  EXPECT_EQ(run->dropped, 0);
}

}  // namespace
}  // namespace antb1::exec
