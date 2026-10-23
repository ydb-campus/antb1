// What the plan-time build of a physical plan records, which is everything `explain --physical`
// reports (engine::FormatPhysicalPlan): the pushed predicates of ADR 0020 and the key filter
// candidates of ADR 0029. Nothing here runs a query -- BuildPhysicalPlan alone names every profile
// node and writes its detail -- and that is the subject: a *candidate* is what a plan can say,
// against the `key_filter`, `key_pruned` and `key_dropped` metrics a run measures. Three of the
// conditions behind a filter are not plan-time facts (whether the build published one at all,
// whether it stayed under D1's share gate, whether a part's statistics exclude its keys), so a
// report that claimed more than candidacy would be claiming what it cannot know.
//
// These bind SQL rather than hand-building plans, for the reason aggregate_filter_test.cc gives:
// one of the two paths a candidate comes from carries the key by ColumnId, and a hand-built plan
// leaves ids unset, so nothing would match and the test would pass while testing nothing.

#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <gtest/gtest.h>

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

class PlanReportTest : public testing::ExecTest {};

// A fact table over three parts whose `k` repeats, so a GROUP BY on it reduces.
std::shared_ptr<MemoryTable> Facts() {
  const auto schema =
      arrow::schema({arrow::field("k", arrow::int64()), arrow::field("v", arrow::int64()),
                     arrow::field("ks", arrow::binary())});
  arrow::RecordBatchVector batches;
  for (int64_t part = 0; part < 3; ++part) {
    std::vector<std::optional<int64_t>> keys;
    std::vector<std::optional<int64_t>> values;
    std::vector<std::optional<std::string>> names;
    for (int64_t r = 0; r < 30; ++r) {
      keys.emplace_back(((part * 30) + r) % 10);
      values.emplace_back(r);
      names.emplace_back(std::format("k{}", ((part * 30) + r) % 10));
    }
    batches.push_back(
        arrow::RecordBatch::Make(schema, 30,
                                 arrow::ArrayVector{testing::Int64s(keys), testing::Int64s(values),
                                                    testing::Strings(names)}));
  }
  return std::make_shared<MemoryTable>(schema, std::move(batches), /*split=*/true);
}

// A dimension small enough that every plan here builds on it: `dk` joins the fact's `k`.
std::shared_ptr<MemoryTable> Dimension() {
  const auto schema =
      arrow::schema({arrow::field("dk", arrow::int64()), arrow::field("flag", arrow::int64()),
                     arrow::field("dks", arrow::binary())});
  std::vector<std::optional<int64_t>> keys;
  std::vector<std::optional<int64_t>> flags;
  std::vector<std::optional<std::string>> names;
  for (int64_t i = 0; i < 10; ++i) {
    keys.emplace_back(i);
    flags.emplace_back(i < 2 ? 1 : 0);
    names.emplace_back(std::format("k{}", i));
  }
  arrow::RecordBatchVector batches{arrow::RecordBatch::Make(
      schema, 10,
      arrow::ArrayVector{testing::Int64s(keys), testing::Int64s(flags), testing::Strings(names)})};
  return std::make_shared<MemoryTable>(schema, std::move(batches), /*split=*/true);
}

// `sql` planned into `profile` and not run: the operator tree is built and dropped, which is what
// Session::ExplainPhysical does.
arrow::Status PlanOnly(std::string_view sql, const std::shared_ptr<MemoryTable>& facts,
                       const std::shared_ptr<MemoryTable>& dimension, ProfileNode* profile) {
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
  ARROW_ASSIGN_OR_RAISE(const std::unique_ptr<Operator> op, BuildPhysicalPlan(plan, profile));
  return arrow::Status::OK();
}

// The detail of the Scan of `table`, empty when the plan has none.
std::string ScanDetail(const ProfileNode& node, std::string_view table) {
  const std::string prefix = std::format("Scan table={} ", table);
  if (node.name() == "Scan" && node.detail().starts_with(prefix)) {
    return node.detail();
  }
  for (const ProfileNode* child : node.children()) {
    if (std::string found = ScanDetail(*child, table); !found.empty()) {
      return found;
    }
  }
  return {};
}

// The join builds on the ten-row dimension, so the fact table is the probe: its scan can be asked
// about the keys that build publishes, and says so. The dimension's own pipeline has no join above
// it, so it names no candidate -- only the predicate pushed into it.
TEST_F(PlanReportTest, AProbeScanNamesTheKeyFilterItCouldTake) {
  ProfileNode profile;
  ASSERT_TRUE(PlanOnly("SELECT COUNT(*) FROM f, d WHERE f.k = d.dk AND d.flag = 1", Facts(),
                       Dimension(), &profile)
                  .ok());
  const std::string facts = ScanDetail(profile, "f");
  EXPECT_TRUE(facts.ends_with(", key filter candidate on k")) << facts;
  const std::string dimension = ScanDetail(profile, "d");
  EXPECT_TRUE(dimension.ends_with(", 1 pushed predicate")) << dimension;
}

// Pushed predicates no longer stand in the way of the key filter: the scan applies the conjunction
// of the two (MakeBothScanFilters), so the annotation says the same thing here as on a scan with no
// predicates of its own. Before this it said "part skip candidate", the part test being all that
// could still apply.
TEST_F(PlanReportTest, PushedPredicatesAndAKeyFilterBothReachTheScan) {
  ProfileNode profile;
  ASSERT_TRUE(PlanOnly("SELECT COUNT(*) FROM f, d WHERE f.k = d.dk AND f.v > 1", Facts(),
                       Dimension(), &profile)
                  .ok());
  const std::string facts = ScanDetail(profile, "f");
  EXPECT_TRUE(facts.ends_with(", 1 pushed predicate, key filter candidate on k")) << facts;
}

// A left outer join that builds on its right input emits an unmatched probe row, so dropping one
// would lose an answer: D1's rule refuses it, and so must the report of it.
TEST_F(PlanReportTest, NoCandidateWhereTheJoinEmitsUnmatchedProbeRows) {
  ProfileNode profile;
  ASSERT_TRUE(
      PlanOnly("SELECT COUNT(*) FROM f LEFT JOIN d ON f.k = d.dk", Facts(), Dimension(), &profile)
          .ok());
  const std::string facts = ScanDetail(profile, "f");
  EXPECT_FALSE(facts.contains("candidate")) << facts;
}

// The filter is asked about one column, so a join of two keys offers the scan nothing.
TEST_F(PlanReportTest, NoCandidateWithMoreThanOneKey) {
  ProfileNode profile;
  ASSERT_TRUE(PlanOnly("SELECT COUNT(*) FROM f, d WHERE f.k = d.dk AND f.v = d.flag", Facts(),
                       Dimension(), &profile)
                  .ok());
  const std::string facts = ScanDetail(profile, "f");
  EXPECT_FALSE(facts.contains("candidate")) << facts;
}

// The other source of a candidate: a join above the grouped aggregate this scan feeds, whose
// published keys can restrict the aggregate's own input (ADR 0029's D5). The two arms differ only
// in what the fact table claims to hold, which is what D1's share gate reads -- and the gate is a
// run-time test against the build's finished table, so **the candidate is named either way**. That
// is the whole distinction the word carries: the report says where a filter could go, and only a
// run says whether one went there.
TEST_F(PlanReportTest, AScanBelowAGroupedAggregateNamesTheJoinsKey) {
  constexpr std::string_view kSql =
      "SELECT COUNT(*) FROM (SELECT k, SUM(v) AS s FROM f GROUP BY k) AS g, d "
      "WHERE g.k = d.dk AND d.flag = 1";
  for (const int64_t claimed : {int64_t{20'000'000}, int64_t{16}}) {
    SCOPED_TRACE(std::format("claimed rows {}", claimed));
    const auto facts = Facts();
    facts->set_claimed_rows(claimed);  // past the share gate, then far short of it
    ProfileNode profile;
    ASSERT_TRUE(PlanOnly(kSql, facts, Dimension(), &profile).ok());
    const std::string detail = ScanDetail(profile, "f");
    EXPECT_TRUE(detail.ends_with(", key filter candidate on k")) << detail;
  }
}

// No build can publish a filter for a key a Parquet scan would not decode to the values the build
// hashed, which is one key of an integer or DATE type (CanPublishKeyFilter) -- so a VARCHAR key has
// no candidate at all, neither mechanism. This is **not** one of the three run-time facts the word
// "candidate" covers: it is settled by the plan, and the old code enforced it only by accident,
// through the null filter that ProbeKeyStructure no longer tests.
TEST_F(PlanReportTest, NoCandidateForAKeyNoBuildCanPublish) {
  for (const std::string_view sql :
       {"SELECT COUNT(*) FROM f, d WHERE f.ks = d.dks",
        "SELECT COUNT(*) FROM f, d WHERE f.ks = d.dks AND f.v > 1",
        // The grouped-aggregate arm reads its filter from a build as well, so the same key leaves
        // it with nothing to offer. Its own arm, because it reaches the scan by ColumnId rather
        // than through the pipeline's joins, and so tests the condition separately.
        "SELECT COUNT(*) FROM (SELECT ks, SUM(v) AS s FROM f GROUP BY ks) AS g, d "
        "WHERE g.ks = d.dks AND d.flag = 1"}) {
    SCOPED_TRACE(sql);
    ProfileNode profile;
    ASSERT_TRUE(PlanOnly(sql, Facts(), Dimension(), &profile).ok());
    const std::string facts = ScanDetail(profile, "f");
    EXPECT_FALSE(facts.contains("candidate")) << facts;
  }
}

// The two mechanisms are not interchangeable and the line says which one it means: without a
// filtered decoder over the fields this scan reads, the per-row filter can never be attached
// (CanFilterRows asks the table first), and all a published key set can do is skip a part whole.
// The arms are one table with that support turned off and on, so nothing else differs.
TEST_F(PlanReportTest, WithoutAFilteredDecoderOnlyAPartCanBeSkipped) {
  for (const bool decoder : {false, true}) {
    SCOPED_TRACE(decoder ? "a filtered decoder" : "no filtered decoder");
    const auto facts = Facts();
    facts->set_scan_filter(decoder);
    ProfileNode profile;
    ASSERT_TRUE(
        PlanOnly("SELECT COUNT(*) FROM f, d WHERE f.k = d.dk", facts, Dimension(), &profile).ok());
    const std::string detail = ScanDetail(profile, "f");
    EXPECT_TRUE(
        detail.ends_with(decoder ? ", key filter candidate on k" : ", part skip candidate on k"))
        << detail;
  }
}

}  // namespace
}  // namespace antb1::exec
