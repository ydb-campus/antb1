// Computing a repeated sub-plan once (docs/adr/0030-shared-sub-plans.md, E2).
//
// These bind SQL rather than hand-building plans, which is unusual for an exec test and necessary:
// the pass that finds repeated sub-plans matches columns by where they come from, and that is
// derived from the ColumnIds the binder mints. A hand-built plan leaves them unset, so nothing
// would ever be found to share and the test would pass while testing nothing.

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

struct Run {
  std::shared_ptr<arrow::Table> rows;
  int parts_scanned = 0;               // how many part scans the base table served
  std::optional<int64_t> shared_rows;  // the profile's count, absent when nothing was shared
};

arrow::Result<Run> ExecutePlan(const plan::LogicalPlan& plan,
                               const std::shared_ptr<MemoryTable>& table) {
  ProfileNode profile;
  ARROW_ASSIGN_OR_RAISE(std::unique_ptr<Operator> op, BuildPhysicalPlan(plan, &profile));
  MemoryBudget budget(int64_t{1024} * 1024 * 1024);
  ExecContext ctx{.pool = arrow::default_memory_pool(), .budget = &budget};
  ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::Table> rows, Drain(*op, ctx));
  std::optional<int64_t> shared_rows;
  for (const ProfileMetric& metric : profile.metrics()) {
    if (metric.name == "shared_rows") {
      shared_rows = metric.value;
    }
  }
  return Run{.rows = std::move(rows),
             .parts_scanned = static_cast<int>(table->scanned_parts().size()),
             .shared_rows = shared_rows};
}

arrow::Result<Run> Execute(std::string_view sql, const std::shared_ptr<MemoryTable>& table) {
  ARROW_ASSIGN_OR_RAISE(const plan::LogicalPlan plan, PlanOf(sql, table));
  return ExecutePlan(plan, table);
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

// **Nothing under a union is held** (ADR 0036). A union repeated whole is one group, which the gate
// refuses, and a branch is never held on its own -- however the two copies list their branches, and
// whatever they hold that has no key. A branch's result held alone can outgrow
// MaterializedTable::kMaxBytes at a large scale factor, and a result past the cap fails the query.
//
// The control is the point of the hand-built ids: the same aggregate-topped sub-plan repeated
// outside a union *is* shared, so the ids are what the pass follows and the union cases test it.
TEST_F(SharedBuildsTest, HoldsNothingUnderAUnion) {
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
    EXPECT_EQ(run->shared_rows, std::nullopt) << "nothing under a union is held";
    ASSERT_NE(run->rows, nullptr);
    EXPECT_EQ(testing::SingleInt64(*run->rows), 28) << "each key twice on either side";
  }
}

}  // namespace
}  // namespace antb1::exec
