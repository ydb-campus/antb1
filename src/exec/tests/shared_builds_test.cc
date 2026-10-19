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
  int parts_scanned = 0;  // how many part scans the base table served
};

arrow::Result<Run> Execute(std::string_view sql, const std::shared_ptr<MemoryTable>& table) {
  ARROW_ASSIGN_OR_RAISE(const plan::LogicalPlan plan, PlanOf(sql, table));
  ARROW_ASSIGN_OR_RAISE(std::unique_ptr<Operator> op, BuildPhysicalPlan(plan));
  MemoryBudget budget(int64_t{1024} * 1024 * 1024);
  ExecContext ctx{.pool = arrow::default_memory_pool(), .budget = &budget};
  ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::Table> rows, Drain(*op, ctx));
  return Run{.rows = std::move(rows),
             .parts_scanned = static_cast<int>(table->scanned_parts().size())};
}

// A CTE referenced twice: the sub-plan is a grouped aggregate, which reduces, so with a table large
// enough to be worth not reading twice the plan computes it once. The control is the *same* plan
// over the *same* rows, differing only in the row count the table claims -- so the two arms differ
// in the sharing and in nothing else.
constexpr std::string_view kTwiceReferencedCte =
    "WITH g AS (SELECT k, COUNT(*) AS n FROM t GROUP BY k) "
    "SELECT COUNT(*) FROM g AS a, g AS b WHERE a.k = b.k";

TEST(SharedBuildsTest, ComputesARepeatedSubPlanOnceAndKeepsTheAnswer) {
  const auto shared = KeyedTable();
  shared->set_claimed_rows(20'000'000);  // past the gate, while holding 60 rows
  auto with = Execute(kTwiceReferencedCte, shared);
  ASSERT_TRUE(with.ok()) << with.status().ToString();

  const auto control = KeyedTable();  // claims its 60 rows: too cheap to be worth sharing
  auto without = Execute(kTwiceReferencedCte, control);
  ASSERT_TRUE(without.ok()) << without.status().ToString();

  EXPECT_EQ(with->parts_scanned, 3) << "the three parts read once, for one shared result";
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
TEST(SharedBuildsTest, RefusesASubPlanTooCheapToBeWorthHolding) {
  const auto table = KeyedTable();
  table->set_claimed_rows(1'000);
  auto run = Execute(kTwiceReferencedCte, table);
  ASSERT_TRUE(run.ok()) << run.status().ToString();
  EXPECT_EQ(run->parts_scanned, 6) << "read twice: holding this would cost more than it saves";
}

// And the first half: a sub-plan that does not reduce. Two references to a bare table repeat a
// scan, and holding its rows to avoid reading them is the trade the gate exists to refuse -- the
// shape that would otherwise hold 379 million rows of the workload's largest query.
TEST(SharedBuildsTest, RefusesARepeatedSubPlanThatDoesNotReduce) {
  const auto table = KeyedTable();
  table->set_claimed_rows(20'000'000);
  auto run = Execute("SELECT COUNT(*) FROM t AS a, t AS b WHERE a.k = b.k", table);
  ASSERT_TRUE(run.ok()) << run.status().ToString();
  EXPECT_EQ(run->parts_scanned, 6) << "a scan is not a sub-plan worth holding the output of";
}

// Nothing shared, nothing changed: the plan of a query with no repetition is the plan it was.
TEST(SharedBuildsTest, LeavesAPlanWithNoRepetitionAlone) {
  const auto table = KeyedTable();
  table->set_claimed_rows(20'000'000);
  auto run = Execute("SELECT k, COUNT(*) AS n FROM t GROUP BY k", table);
  ASSERT_TRUE(run.ok()) << run.status().ToString();
  EXPECT_EQ(run->parts_scanned, 3);
}

}  // namespace
}  // namespace antb1::exec
