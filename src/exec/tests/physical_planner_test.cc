#include "antb1/exec/physical_planner.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <gtest/gtest.h>

#include "antb1/exec/limit.h"
#include "antb1/exec/profile.h"
#include "antb1/exec/scalar_aggregate.h"
#include "antb1/exec/sort.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/sql_status.h"

#include "../parallel_compute.h"
#include "../part_operators.h"
#include "exec_test_util.h"

namespace antb1::exec {
namespace {

using plan::LogicalType;
using testing::BigInt;
using testing::Column;
using testing::Int64Column;
using testing::Int64s;
using testing::MemoryTable;

class PhysicalPlannerTest : public testing::ExecTest {
 protected:
  // x = 0..9 in batches of 4, 4 and 2; y = 10 * x, NULL where x is a multiple of 3. With `split`,
  // each batch is a part.
  static std::shared_ptr<MemoryTable> Table(bool split = false) {
    const auto schema =
        arrow::schema({arrow::field("x", arrow::int64()), arrow::field("y", arrow::int64())});
    arrow::RecordBatchVector batches;
    int64_t next = 0;
    for (const int64_t rows : {4, 4, 2}) {
      std::vector<std::optional<int64_t>> x;
      std::vector<std::optional<int64_t>> y;
      for (int64_t i = 0; i < rows; ++i, ++next) {
        x.emplace_back(next);
        y.emplace_back(next % 3 == 0 ? std::nullopt : std::optional(next * 10));
      }
      batches.push_back(arrow::RecordBatch::Make(schema, rows, {Int64s(x), Int64s(y)}));
    }
    return std::make_shared<MemoryTable>(schema, std::move(batches), split);
  }

  static plan::LogicalNodePtr Node(plan::LogicalNode node) {
    return std::make_shared<const plan::LogicalNode>(std::move(node));
  }

  static plan::LogicalPlan PlanOf(plan::LogicalNodePtr root, std::size_t width = 1) {
    plan::LogicalPlan plan{.root = std::move(root), .output = {}};
    for (std::size_t i = 0; i < width; ++i) {
      plan.output.push_back({.name = "c" + std::to_string(i), .type = LogicalType::kBigInt});
    }
    return plan;
  }

  static std::shared_ptr<arrow::Table> Run(const plan::LogicalPlan& plan, int64_t batch_size = 3) {
    auto op = BuildPhysicalPlan(plan);
    EXPECT_TRUE(op.ok()) << op.status().ToString();
    if (!op.ok()) {
      return nullptr;
    }
    ExecContext ctx{.pool = arrow::default_memory_pool(), .batch_size = batch_size};
    auto table = Drain(**op, ctx);
    EXPECT_TRUE(table.ok()) << table.status().ToString();
    return table.ok() ? *table : nullptr;
  }
};

TEST_F(PhysicalPlannerTest, RowCountIsAnsweredFromMetadata) {
  const auto table = Table();
  const auto result = Run(PlanOf(Node(plan::RowCountNode{.table = table, .table_name = "t"})));
  EXPECT_EQ(testing::SingleInt64(*result), 10);
  EXPECT_EQ(table->scans(), 0);
}

TEST_F(PhysicalPlannerTest, LimitOverProjectOverFilterOverScan) {
  const auto table = Table();
  const auto scan = Node(plan::ScanNode{.table = table, .table_name = "t", .fields = {1, 0}});
  const auto filter =
      Node(plan::FilterNode{.input = scan,
                            .predicates = {testing::Compare(Column(1, "x", LogicalType::kBigInt),
                                                            plan::CompareOp::kGe, BigInt(2))}});
  const auto project =
      Node(plan::ProjectNode{.input = filter, .columns = {Column(1, "x", LogicalType::kBigInt)}});
  const auto limit = Node(plan::LimitNode{.input = project, .limit = 5});
  EXPECT_EQ(Int64Column(*Run(PlanOf(limit))), (std::vector<std::optional<int64_t>>{2, 3, 4, 5, 6}));
}

// Limit(Sort) with a positive limit is one top-N SortOperator; LIMIT 0 and OFFSET alone keep a
// LimitOperator (over a full SortOperator for OFFSET).
TEST_F(PhysicalPlannerTest, LimitOverSortIsATopN) {
  const auto table = Table();
  const auto scan = Node(plan::ScanNode{.table = table, .table_name = "t", .fields = {0, 1}});
  const auto sort = Node(
      plan::SortNode{.input = scan,
                     .keys = {{.column = Column(1, "y", LogicalType::kBigInt), .descending = true},
                              {.column = Column(0, "x", LogicalType::kBigInt)}}});
  const auto limited = [&](std::optional<int64_t> limit, int64_t offset) {
    const auto node = Node(plan::LimitNode{.input = sort, .limit = limit, .offset = offset});
    return Node(
        plan::ProjectNode{.input = node, .columns = {Column(0, "x", LogicalType::kBigInt)}});
  };
  using Ids = std::vector<std::optional<int64_t>>;
  // y DESC (NULLs last): x 8 7 5 4 2 1, then 0 3 6 9 (y NULL) by x.
  EXPECT_EQ(Int64Column(*Run(PlanOf(limited(3, 2)))), (Ids{5, 4, 2}));
  EXPECT_EQ(Int64Column(*Run(PlanOf(limited(std::nullopt, 6)))), (Ids{0, 3, 6, 9}));
  EXPECT_EQ(Int64Column(*Run(PlanOf(limited(0, 1)))), Ids{});
  EXPECT_EQ(Int64Column(*Run(PlanOf(sort, 2))), (Ids{8, 7, 5, 4, 2, 1, 0, 3, 6, 9}));

  // The root operator of Limit(Sort).
  const auto root = [&](std::optional<int64_t> limit, int64_t offset) {
    const auto node = Node(plan::LimitNode{.input = sort, .limit = limit, .offset = offset});
    auto op = BuildPhysicalPlan(PlanOf(node, 2));
    EXPECT_TRUE(op.ok()) << op.status().ToString();
    return op.ok() ? *std::move(op) : nullptr;
  };
  // Over a part pipeline (here a scan), a top-N keeps each part's first rows: PartTopNOperator.
  EXPECT_EQ(dynamic_cast<const LimitOperator*>(root(3, 2).get()), nullptr);
  EXPECT_EQ(dynamic_cast<const SortOperator*>(root(3, 2).get()), nullptr);
  EXPECT_EQ(root(3, 2)->output_schema()->num_fields(), 2);
  EXPECT_NE(dynamic_cast<const LimitOperator*>(root(0, 0).get()), nullptr);
  EXPECT_NE(dynamic_cast<const LimitOperator*>(root(std::nullopt, 4).get()), nullptr);
}

// A global aggregation of only COUNT(DISTINCT) of one column over a scan is planned as COUNT over a
// GROUP BY of that column (a ScalarAggregateOperator root); one mixed with other calls or columns
// keeps the per-part aggregation (not ScalarAggregateOperator).
TEST_F(PhysicalPlannerTest, CountDistinctAloneBecomesAGroupBy) {
  const auto table = Table();
  const auto scan = Node(plan::ScanNode{.table = table, .table_name = "t", .fields = {0, 1}});
  const auto x = Column(0, "x", LogicalType::kBigInt);
  const auto y = Column(1, "y", LogicalType::kBigInt);
  const auto root = [&](std::vector<plan::AggregateCall> calls) {
    const std::size_t width = calls.size();
    auto op = BuildPhysicalPlan(
        PlanOf(Node(plan::AggregateNode{.input = scan, .aggregates = std::move(calls)}), width));
    EXPECT_TRUE(op.ok()) << op.status().ToString();
    return op.ok() ? *std::move(op) : nullptr;
  };
  const plan::AggregateCall distinct_x{
      .kind = plan::AggKind::kCountDistinct, .arg = x, .type = LogicalType::kBigInt};
  const plan::AggregateCall distinct_y{
      .kind = plan::AggKind::kCountDistinct, .arg = y, .type = LogicalType::kBigInt};
  const plan::AggregateCall count_x{
      .kind = plan::AggKind::kCount, .arg = x, .type = LogicalType::kBigInt};
  EXPECT_NE(dynamic_cast<const ScalarAggregateOperator*>(root({distinct_x}).get()), nullptr);
  EXPECT_NE(dynamic_cast<const ScalarAggregateOperator*>(root({distinct_x, distinct_x}).get()),
            nullptr);
  EXPECT_EQ(dynamic_cast<const ScalarAggregateOperator*>(root({distinct_x, distinct_y}).get()),
            nullptr);
  EXPECT_EQ(dynamic_cast<const ScalarAggregateOperator*>(root({distinct_x, count_x}).get()),
            nullptr);
  // The same counts either way: x = 0..9, y with NULLs every third row (6 distinct values).
  const auto counts = [&](std::vector<plan::AggregateCall> calls) {
    const std::size_t width = calls.size();
    return Run(
        PlanOf(Node(plan::AggregateNode{.input = scan, .aggregates = std::move(calls)}), width));
  };
  EXPECT_EQ(Int64Column(*counts({distinct_x}), 0), (std::vector<std::optional<int64_t>>{10}));
  EXPECT_EQ(Int64Column(*counts({distinct_y}), 0), (std::vector<std::optional<int64_t>>{6}));
  EXPECT_EQ(Int64Column(*counts({distinct_x, distinct_y}), 1),
            (std::vector<std::optional<int64_t>>{6}));
}

// An aggregation with COUNT(DISTINCT) over a scan runs in two levels (ADR 0014) when its calls
// allow it, grouped or global (but a global COUNT(DISTINCT) of one column alone, which is a GROUP
// BY); otherwise the per-part GROUP BY or aggregate: a DOUBLE key, a DOUBLE SUM, a distinct
// column that is a key.
TEST_F(PhysicalPlannerTest, CountDistinctRunsInTwoLevels) {
  const auto schema =
      arrow::schema({arrow::field("k", arrow::int64()), arrow::field("x", arrow::int64()),
                     arrow::field("d", arrow::float64())});
  arrow::DoubleBuilder d;
  ASSERT_TRUE(d.AppendValues({0.5, -0.0, 0.0, 1.5}).ok());
  const auto table = std::make_shared<MemoryTable>(
      schema,
      arrow::RecordBatchVector{arrow::RecordBatch::Make(
          schema, 4, {Int64s({1, 1, 2, std::nullopt}), Int64s({5, 6, 5, 7}), *d.Finish()})},
      true);
  const auto scan = Node(plan::ScanNode{.table = table, .table_name = "t", .fields = {0, 1, 2}});
  const auto k = Column(0, "k", LogicalType::kBigInt);
  const auto x = Column(1, "x", LogicalType::kBigInt);
  const auto dbl = Column(2, "d", LogicalType::kDouble);
  const plan::AggregateCall distinct_x{
      .kind = plan::AggKind::kCountDistinct, .arg = x, .type = LogicalType::kBigInt};
  const plan::AggregateCall distinct_d{
      .kind = plan::AggKind::kCountDistinct, .arg = dbl, .type = LogicalType::kBigInt};
  const plan::AggregateCall distinct_k{
      .kind = plan::AggKind::kCountDistinct, .arg = k, .type = LogicalType::kBigInt};
  const plan::AggregateCall sum_d{
      .kind = plan::AggKind::kSum, .arg = dbl, .type = LogicalType::kDouble};
  const plan::AggregateCall max_x{
      .kind = plan::AggKind::kMax, .arg = x, .type = LogicalType::kBigInt};
  const auto grouped = [&](std::vector<plan::BoundColumn> keys,
                           std::vector<plan::AggregateCall> calls) {
    const std::size_t width = keys.size() + calls.size();
    auto op = BuildPhysicalPlan(
        PlanOf(Node(plan::GroupAggregateNode{
                   .input = scan, .keys = std::move(keys), .aggregates = std::move(calls)}),
               width));
    EXPECT_TRUE(op.ok()) << op.status().ToString();
    return op.ok() ? *std::move(op) : nullptr;
  };
  const auto global = [&](std::vector<plan::AggregateCall> calls) {
    const std::size_t width = calls.size();
    auto op = BuildPhysicalPlan(
        PlanOf(Node(plan::AggregateNode{.input = scan, .aggregates = std::move(calls)}), width));
    EXPECT_TRUE(op.ok()) << op.status().ToString();
    return op.ok() ? *std::move(op) : nullptr;
  };
  const auto two_level = [](const std::unique_ptr<Operator>& op) {
    return dynamic_cast<const PartTwoLevelAggregateOperator*>(op.get()) != nullptr;
  };
  EXPECT_TRUE(two_level(grouped({k}, {distinct_x})));
  EXPECT_TRUE(two_level(grouped({k}, {distinct_x, distinct_d, max_x})));
  EXPECT_TRUE(two_level(global({distinct_x, distinct_d})));
  EXPECT_TRUE(two_level(global({distinct_x, max_x})));
  EXPECT_FALSE(two_level(global({distinct_x})));  // a GROUP BY x, then COUNT
  EXPECT_FALSE(two_level(grouped({dbl}, {distinct_x})));
  EXPECT_FALSE(two_level(grouped({k}, {distinct_x, sum_d})));
  EXPECT_FALSE(two_level(grouped({k}, {distinct_k})));
  EXPECT_NE(dynamic_cast<const PartGroupAggregateOperator*>(grouped({k}, {distinct_k}).get()),
            nullptr);
  EXPECT_NE(dynamic_cast<const PartAggregateOperator*>(global({distinct_x, sum_d}).get()), nullptr);
  // COUNT(DISTINCT d) counts -0.0 with 0.0: 3 values; 3 values of x; MAX(x) = 7.
  const auto result = Run(PlanOf(
      Node(plan::AggregateNode{.input = scan, .aggregates = {distinct_x, distinct_d, max_x}}), 3));
  EXPECT_EQ(Int64Column(*result, 0), (std::vector<std::optional<int64_t>>{3}));
  EXPECT_EQ(Int64Column(*result, 1), (std::vector<std::optional<int64_t>>{3}));
  EXPECT_EQ(Int64Column(*result, 2), (std::vector<std::optional<int64_t>>{7}));
}

// A top-N reads late the columns that no filter, computation or key uses (ADR 0016), when
// limit + offset is at most half of the parts; not with a Project inside the pipeline, nor when
// every column is read.
TEST_F(PhysicalPlannerTest, TopNReadsUnusedColumnsLate) {
  const auto schema =
      arrow::schema({arrow::field("x", arrow::int64()), arrow::field("y", arrow::int64()),
                     arrow::field("z", arrow::int64())});
  arrow::RecordBatchVector batches;
  for (int64_t part = 0; part < 10; ++part) {
    batches.push_back(arrow::RecordBatch::Make(
        schema, 1, {Int64s({part}), Int64s({part * 10}), Int64s({part * 100})}));
  }
  const auto table = std::make_shared<MemoryTable>(schema, std::move(batches), true);
  const auto x = Column(0, "x", LogicalType::kBigInt);
  const auto y = Column(1, "y", LogicalType::kBigInt);
  const auto z = Column(2, "z", LogicalType::kBigInt);
  const auto scan = Node(plan::ScanNode{.table = table, .table_name = "t", .fields = {0, 1, 2}});
  const auto detail = [&](const plan::LogicalNodePtr& input, const plan::BoundColumn& key,
                          int64_t limit, std::size_t width = 3) {
    ProfileNode root;
    auto op = BuildPhysicalPlan(
        PlanOf(Node(plan::LimitNode{
                   .input = Node(plan::SortNode{.input = input, .keys = {{.column = key}}}),
                   .limit = limit,
                   .offset = 0}),
               width),
        &root);
    EXPECT_TRUE(op.ok()) << op.status().ToString();
    return root.detail();
  };
  EXPECT_EQ(detail(scan, x, 2), "Sort x ASC NULLS LAST Limit 2 late=2 columns");
  EXPECT_EQ(detail(scan, x, 5), "Sort x ASC NULLS LAST Limit 5 late=2 columns");
  EXPECT_EQ(detail(scan, x, 6), "Sort x ASC NULLS LAST Limit 6") << "more than half of the parts";
  const auto filter = Node(plan::FilterNode{
      .input = scan,
      .predicates = {testing::Compare(z, plan::CompareOp::kGe, testing::BigInt(0))}});
  EXPECT_EQ(detail(filter, y, 2), "Sort y ASC NULLS LAST Limit 2 late=1 columns");
  const auto compute = Node(
      plan::ComputeNode{.input = filter,
                        .exprs = {std::make_shared<const plan::Expr>(plan::Expr{
                            .node = plan::ColumnExpr{.index = 0}, .type = LogicalType::kBigInt})}});
  EXPECT_EQ(detail(compute, y, 2, 4), "Sort y ASC NULLS LAST Limit 2") << "every column is read";
  const auto project = Node(plan::ProjectNode{.input = scan, .columns = {z, x}});
  EXPECT_EQ(detail(project, Column(1, "x", LogicalType::kBigInt), 2, 2),
            "Sort x ASC NULLS LAST Limit 2")
      << "a Project renumbers the columns";
  // The rows are those of the plain top-N.
  const auto rows = Run(PlanOf(
      Node(plan::LimitNode{
          .input = Node(plan::SortNode{.input = scan, .keys = {{.column = y, .descending = true}}}),
          .limit = 3,
          .offset = 1}),
      3));
  ASSERT_NE(rows, nullptr);
  EXPECT_EQ(Int64Column(*rows, 0), (std::vector<std::optional<int64_t>>{8, 7, 6}));
  EXPECT_EQ(Int64Column(*rows, 2), (std::vector<std::optional<int64_t>>{800, 700, 600}));
}

TEST_F(PhysicalPlannerTest, AggregateOverFilteredScanForEveryBatchSize) {
  const auto table = Table();
  const auto scan = Node(plan::ScanNode{.table = table, .table_name = "t", .fields = {0, 1}});
  const auto filter =
      Node(plan::FilterNode{.input = scan,
                            .predicates = {testing::Compare(Column(0, "x", LogicalType::kBigInt),
                                                            plan::CompareOp::kLt, BigInt(7))}});
  const auto y = Column(1, "y", LogicalType::kBigInt);
  const auto aggregate = Node(plan::AggregateNode{
      .input = filter,
      .aggregates = {{.kind = plan::AggKind::kCountStar, .arg = {}, .type = LogicalType::kBigInt},
                     {.kind = plan::AggKind::kCount, .arg = y, .type = LogicalType::kBigInt},
                     {.kind = plan::AggKind::kMax, .arg = y, .type = LogicalType::kBigInt}}});
  for (const int64_t batch_size : {1, 3, 64}) {
    const auto result = Run(PlanOf(aggregate, 3), batch_size);
    ASSERT_EQ(result->num_rows(), 1);
    EXPECT_EQ(Int64Column(*result, 0), (std::vector<std::optional<int64_t>>{7}));
    EXPECT_EQ(Int64Column(*result, 1), (std::vector<std::optional<int64_t>>{4}));  // 1 2 4 5
    EXPECT_EQ(Int64Column(*result, 2), (std::vector<std::optional<int64_t>>{50}));
  }
}

// A Compute over a whole input (here a sort) computes its batches in parallel; one inside a part
// pipeline (over the scan) stays ComputeOperator: the parts are the parallelism there.
TEST_F(PhysicalPlannerTest, ComputeOverAWholeInputIsParallel) {
  const auto table = Table();
  const auto scan = Node(plan::ScanNode{.table = table, .table_name = "t", .fields = {0}});
  const auto plus_one = std::make_shared<const plan::Expr>(
      plan::Expr{.node = plan::ArithExpr{.op = plan::ArithOp::kAdd,
                                         .left = std::make_shared<const plan::Expr>(
                                             plan::Expr{.node = plan::ColumnExpr{.index = 0},
                                                        .type = LogicalType::kBigInt,
                                                        .name = "x"}),
                                         .right = std::make_shared<const plan::Expr>(plan::Expr{
                                             .node = plan::ConstantExpr{.value = BigInt(1)},
                                             .type = LogicalType::kBigInt,
                                             .name = "1"})},
                 .type = LogicalType::kBigInt,
                 .name = "x + 1"});
  const auto sort = Node(plan::SortNode{
      .input = scan, .keys = {plan::SortKey{.column = Column(0, "x", LogicalType::kBigInt)}}});
  const auto above = Node(plan::ComputeNode{.input = sort, .exprs = {plus_one}});
  const auto parallel = BuildPhysicalPlan(PlanOf(above, 2));
  ASSERT_TRUE(parallel.ok()) << parallel.status().ToString();
  EXPECT_NE(dynamic_cast<const ParallelComputeOperator*>(parallel->get()), nullptr);
  const auto over_scan =
      BuildPhysicalPlan(PlanOf(Node(plan::ComputeNode{.input = scan, .exprs = {plus_one}}), 2));
  ASSERT_TRUE(over_scan.ok()) << over_scan.status().ToString();
  EXPECT_EQ(dynamic_cast<const ParallelComputeOperator*>(over_scan->get()), nullptr);
  const auto result = Run(PlanOf(above, 2));
  EXPECT_EQ(Int64Column(*result, 1),
            (std::vector<std::optional<int64_t>>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10}));
}

// The predicates of a Filter directly over the scan of a part pipeline that read one column are
// applied by the scan (ADR 0020), when the table supports it; the others stay in the Filter. The
// rows are the same either way.
TEST_F(PhysicalPlannerTest, FilterPredicatesArePushedIntoTheScan) {
  const auto x = Column(0, "x", LogicalType::kBigInt);
  const auto y = Column(1, "y", LogicalType::kBigInt);
  plan::Predicate below = testing::Compare(x, plan::CompareOp::kLt, BigInt(0));
  below.kind = plan::Predicate::Kind::kCompareColumns;  // x < y
  below.other = y;
  const plan::Predicate not_null{
      .kind = plan::Predicate::Kind::kIsNotNull, .column = y, .span = {}};
  const auto plan_of = [&](const std::shared_ptr<MemoryTable>& table,
                           std::vector<plan::Predicate> predicates) {
    const auto scan = Node(plan::ScanNode{.table = table, .table_name = "t", .fields = {0, 1}});
    return PlanOf(Node(plan::FilterNode{.input = scan, .predicates = std::move(predicates)}), 2);
  };
  const std::vector<plan::Predicate> predicates = {
      testing::Compare(x, plan::CompareOp::kGe, BigInt(2)), below, not_null};
  for (const bool pushdown : {true, false}) {
    const auto table = Table(/*split=*/true);
    table->set_scan_filter(pushdown);
    for (const int64_t batch_size : {1, 3, 64}) {
      const auto rows = Run(plan_of(table, predicates), batch_size);
      ASSERT_NE(rows, nullptr);
      EXPECT_EQ(Int64Column(*rows, 0), (std::vector<std::optional<int64_t>>{2, 4, 5, 7, 8}))
          << "pushdown=" << pushdown << " batch_size=" << batch_size;
    }
    EXPECT_EQ(table->filtered_scans(), pushdown ? 9 : 0) << "3 parts, 3 batch sizes";
    ProfileNode root;
    ASSERT_TRUE(BuildPhysicalPlan(plan_of(table, predicates), &root).ok());
    ASSERT_EQ(root.children().size(), 1U);
    const ProfileNode& filter = *root.children()[0];
    EXPECT_EQ(filter.name(), "Filter");
    ASSERT_EQ(filter.children().size(), 1U);
    EXPECT_EQ(filter.children()[0]->name(), "Scan");
    EXPECT_EQ(filter.children()[0]->detail().ends_with(", 2 pushed predicates"), pushdown)
        << filter.children()[0]->detail();
  }
  // One predicate: singular. Only a comparison of two columns: nothing to push.
  auto table = Table(/*split=*/true);
  ProfileNode one;
  ASSERT_TRUE(BuildPhysicalPlan(plan_of(table, {not_null}), &one).ok());
  EXPECT_TRUE(one.children()[0]->children()[0]->detail().ends_with(", 1 pushed predicate"));
  ASSERT_NE(Run(plan_of(table, {below})), nullptr);
  EXPECT_EQ(table->filtered_scans(), 0);
  // A FALSE predicate ends the stream before anything is read: nothing is pushed, nor scanned.
  const plan::Predicate never{.kind = plan::Predicate::Kind::kFalse, .span = {}};
  table = Table(/*split=*/true);
  const auto none = Run(plan_of(table, {not_null, never}));
  ASSERT_NE(none, nullptr);
  EXPECT_EQ(none->num_rows(), 0);
  EXPECT_EQ(table->filtered_scans(), 0);
  EXPECT_TRUE(table->scanned_parts().empty());
}

// The narrow scans of late materialization (ADR 0016) apply the pushed predicates on their early
// columns too, and take their row ids from the positions the table reports: the late columns
// fetched are those of the rows that passed, whatever batches the filter emptied.
TEST_F(PhysicalPlannerTest, LateScansAreFiltered) {
  const auto x = Column(0, "x", LogicalType::kBigInt);
  // x >= 6 AND x <> 6: the first match, x = 7, is the last row of part 1.
  const std::vector<plan::Predicate> predicates = {
      testing::Compare(x, plan::CompareOp::kGe, BigInt(6)),
      testing::Compare(x, plan::CompareOp::kNe, BigInt(6))};
  for (const bool pushdown : {true, false}) {
    const auto table = Table(/*split=*/true);
    table->set_scan_filter(pushdown);
    const auto scan = Node(plan::ScanNode{.table = table, .table_name = "t", .fields = {0, 1}});
    const auto filter = Node(plan::FilterNode{.input = scan, .predicates = predicates});
    for (const bool descending : {false, true}) {
      const auto top =
          PlanOf(Node(plan::LimitNode{
                     .input = Node(plan::SortNode{
                         .input = filter, .keys = {{.column = x, .descending = descending}}}),
                     .limit = 1,
                     .offset = 0}),
                 2);
      ProfileNode root;
      ASSERT_TRUE(BuildPhysicalPlan(top, &root).ok());
      ASSERT_TRUE(root.detail().ends_with("late=1 columns")) << root.detail();
      ASSERT_EQ(root.children().size(), 1U);
      const ProfileNode& scan_slot = *root.children()[0]->children()[0];
      EXPECT_EQ(scan_slot.name(), "Scan");
      EXPECT_EQ(scan_slot.detail().ends_with(", 2 pushed predicates"), pushdown)
          << scan_slot.detail();
      for (const int64_t batch_size : {1, 3, 64}) {
        const auto rows = Run(top, batch_size);
        ASSERT_NE(rows, nullptr);
        EXPECT_EQ(Int64Column(*rows, 0), (std::vector<std::optional<int64_t>>{descending ? 9 : 7}))
            << "pushdown=" << pushdown << " batch_size=" << batch_size;
        EXPECT_EQ(
            Int64Column(*rows, 1),
            (std::vector<std::optional<int64_t>>{descending ? std::nullopt : std::optional(70)}))
            << "pushdown=" << pushdown << " batch_size=" << batch_size;
      }
    }
    EXPECT_EQ(table->filtered_scans() > 0, pushdown);
  }
}

TEST_F(PhysicalPlannerTest, MalformedPlansAreInvalidNotUnsupported) {
  const auto table = Table();
  EXPECT_TRUE(BuildPhysicalPlan(plan::LogicalPlan{}).status().IsInvalid());
  plan::LogicalPlan no_output{.root = Node(plan::RowCountNode{.table = table}), .output = {}};
  EXPECT_TRUE(BuildPhysicalPlan(no_output).status().IsInvalid());
  const std::vector<plan::LogicalNode> broken = {
      plan::RowCountNode{},  plan::ScanNode{},  plan::FilterNode{}, plan::ProjectNode{},
      plan::AggregateNode{}, plan::LimitNode{}, plan::SortNode{},
  };
  for (const plan::LogicalNode& node : broken) {
    const auto status = BuildPhysicalPlan(PlanOf(Node(node))).status();
    EXPECT_TRUE(status.IsInvalid()) << plan::NodeName(node) << ": " << status.ToString();
    EXPECT_EQ(plan::GetSqlError(status), nullptr) << "a malformed plan is a bug, not unsupported";
  }
  // The root's width must match the output columns.
  const auto scan = Node(plan::ScanNode{.table = table, .table_name = "t", .fields = {0, 1}});
  EXPECT_TRUE(BuildPhysicalPlan(PlanOf(scan, 1)).status().IsInvalid());
  EXPECT_TRUE(BuildPhysicalPlan(PlanOf(scan, 2)).ok());
}

}  // namespace
}  // namespace antb1::exec
