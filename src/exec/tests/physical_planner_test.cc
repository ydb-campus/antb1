#include "antb1/exec/physical_planner.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <arrow/api.h>
#include <gtest/gtest.h>

#include "antb1/exec/limit.h"
#include "antb1/exec/profile.h"
#include "antb1/exec/scalar_aggregate.h"
#include "antb1/exec/sort.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/sql_status.h"

#include "../hash_join.h"
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

// The number of columns `node` returns if it is planned, for the scans, sorts and joins that the
// malformed-join cases build: a semi, anti or null-aware anti join returns its probe input's
// columns, any other join both inputs' columns. A missing input counts as the 2-column scan that
// it stands in for.
std::size_t WidthOf(const plan::LogicalNodePtr& node) {
  if (node == nullptr) {
    return 2;
  }
  if (const auto* join = std::get_if<plan::JoinNode>(node.get())) {
    const std::size_t left = WidthOf(join->left);
    const bool probe_only = join->kind == plan::JoinKind::kSemi ||
                            join->kind == plan::JoinKind::kAnti ||
                            join->kind == plan::JoinKind::kNullAwareAnti;
    return probe_only ? left : left + WidthOf(join->right);
  }
  if (const auto* sort = std::get_if<plan::SortNode>(node.get())) {
    return WidthOf(sort->input);
  }
  if (const auto* scan = std::get_if<plan::ScanNode>(node.get())) {
    return scan->fields.size();
  }
  ADD_FAILURE() << "WidthOf: a node the malformed-join cases do not build";
  return 0;
}

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

// The rows a placement of a join returns: the join's, one, one per distinct x of the join's rows,
// or at most 3 of the join's.
enum class Expect : std::uint8_t { kJoin, kOne, kGroups, kLimit };

// A place of a join in a query: its root, the root's width, and its rows.
struct Placement {
  plan::LogicalNodePtr root;
  std::size_t width = 0;
  Expect rows = Expect::kJoin;
};

// The places of `join` (`width` columns, x its first) in a query over `scan` (x and y, one row of
// each x): at the root, below every other node (in the part pipeline it ends, or that goes on
// through it), and as an inner join's build input or probe input on x (each of the join's rows
// meets the one row of scan with its x).
std::vector<Placement> Placements(const plan::LogicalNodePtr& join, std::size_t width,
                                  const plan::LogicalNodePtr& scan) {
  const plan::BoundColumn x{.index = 0, .name = "x", .type = LogicalType::kBigInt};
  const auto node = [](plan::LogicalNode n) {
    return std::make_shared<const plan::LogicalNode>(std::move(n));
  };
  const auto inner = [&](const plan::LogicalNodePtr& left, const plan::LogicalNodePtr& right) {
    return node(plan::JoinNode{.kind = plan::JoinKind::kInner,
                               .left = left,
                               .right = right,
                               .keys = {plan::JoinKey{.left = x, .right = x}},
                               .residual = {},
                               .build = plan::BuildSide::kRight,
                               .span = {}});
  };
  const auto filter = node(plan::FilterNode{
      .input = join,
      .predicates = {plan::Predicate{.kind = plan::Predicate::Kind::kIsNotNull, .column = x}}});
  const auto sort = node(plan::SortNode{.input = join, .keys = {plan::SortKey{.column = x}}});
  const plan::AggregateCall min{
      .kind = plan::AggKind::kMin, .arg = x, .type = LogicalType::kBigInt};
  const plan::AggregateCall distinct{
      .kind = plan::AggKind::kCountDistinct, .arg = x, .type = LogicalType::kBigInt};
  const auto one = std::make_shared<const plan::Expr>(plan::Expr{
      .node = plan::ConstantExpr{.value = BigInt(1)}, .type = LogicalType::kBigInt, .name = "1"});
  return {
      {.root = join, .width = width, .rows = Expect::kJoin},
      {.root = filter, .width = width, .rows = Expect::kJoin},
      {.root = node(plan::ComputeNode{.input = join, .exprs = {one}}),
       .width = width + 1,
       .rows = Expect::kJoin},
      {.root = node(plan::ProjectNode{.input = join, .columns = {x}}),
       .width = 1,
       .rows = Expect::kJoin},
      {.root = node(plan::AggregateNode{.input = join, .aggregates = {min}}),
       .width = 1,
       .rows = Expect::kOne},
      {.root = node(plan::AggregateNode{.input = filter, .aggregates = {min}}),
       .width = 1,
       .rows = Expect::kOne},
      {.root = node(plan::AggregateNode{.input = join, .aggregates = {distinct}}),
       .width = 1,
       .rows = Expect::kOne},
      {.root = node(plan::GroupAggregateNode{.input = join, .keys = {x}, .aggregates = {min}}),
       .width = 2,
       .rows = Expect::kGroups},
      {.root = node(plan::GroupAggregateNode{.input = filter, .keys = {x}, .aggregates = {min}}),
       .width = 2,
       .rows = Expect::kGroups},
      {.root = sort, .width = width, .rows = Expect::kJoin},
      {.root = node(plan::LimitNode{.input = join, .limit = 3}),
       .width = width,
       .rows = Expect::kLimit},
      {.root = node(plan::LimitNode{.input = filter, .limit = 3}),
       .width = width,
       .rows = Expect::kLimit},
      {.root = node(plan::LimitNode{.input = sort, .limit = 3}),
       .width = width,
       .rows = Expect::kLimit},
      {.root = inner(scan, join), .width = 2 + width, .rows = Expect::kJoin},    // the build input
      {.root = inner(join, scan), .width = width + 2, .rows = Expect::kJoin},    // the probe input
      {.root = inner(filter, scan), .width = width + 2, .rows = Expect::kJoin},  // below its Filter
  };
}

// Joins of every kind run in every placement (Placements: a part pipeline goes on through their
// probes), with a profile and without, and return their rows there, none included. Over x = 0..9
// (y = 10x, NULL where x is a multiple of 3), self-joins: on x, semi keeps every row, anti and
// null-aware anti none, left matches every row; on y, anti keeps the rows of a NULL y, null-aware
// anti none (its build has a NULL key), and left pads them. A one-row join appends the row of
// MIN(x) and COUNT(*). The residual y > x (the right input's y) passes the rows of a non-NULL y
// but x = 0: semi keeps them, anti the others, and left pads the others.
TEST_F(PhysicalPlannerTest, EveryJoinKindRunsEverywhere) {
  const auto table = Table(/*split=*/true);
  const auto scan = Node(plan::ScanNode{.table = table, .table_name = "t", .fields = {0, 1}});
  const auto x = Column(0, "x", LogicalType::kBigInt);
  const auto y = Column(1, "y", LogicalType::kBigInt);
  const auto row = Node(plan::AggregateNode{
      .input = scan,
      .aggregates = {
          {.kind = plan::AggKind::kMin, .arg = x, .type = LogicalType::kBigInt},
          {.kind = plan::AggKind::kCountStar, .arg = {}, .type = LogicalType::kBigInt}}});
  // y > x over the pair: the right input's y is column 3, the left input's x column 0.
  const auto column = [](int index) {
    return std::make_shared<const plan::Expr>(
        plan::Expr{.node = plan::ColumnExpr{.index = index}, .type = LogicalType::kBigInt});
  };
  const auto greater = std::make_shared<const plan::Expr>(plan::Expr{
      .node =
          plan::PredicateExpr{
              .predicate = plan::Predicate{.kind = plan::Predicate::Kind::kCompareColumns,
                                           .column = Column(0, "y", LogicalType::kBigInt),
                                           .other = Column(1, "x", LogicalType::kBigInt),
                                           .op = plan::CompareOp::kGt},
              .operands = {column(3), column(0)}},
      .type = LogicalType::kBoolean});
  using Values = std::vector<std::optional<int64_t>>;
  const Values all = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  const std::optional<int64_t> null;
  const Values non_null_y = {null, 1, 2, null, 4, 5, null, 7, 8, null};
  struct Case {
    plan::JoinKind kind;
    std::optional<plan::BoundColumn> key;  // none: a one-row join of `row`
    bool residual = false;                 // y > x
    Values xs;                             // the x of the rows it returns
    Values right;  // a left join: the right input's x of each row (NULL: padded)
  };
  for (const Case& c :
       {Case{.kind = plan::JoinKind::kSemi, .key = x, .xs = all},
        Case{.kind = plan::JoinKind::kAnti, .key = x, .xs = {}},
        Case{.kind = plan::JoinKind::kNullAwareAnti, .key = x, .xs = {}},
        Case{.kind = plan::JoinKind::kAnti, .key = y, .xs = {0, 3, 6, 9}},
        Case{.kind = plan::JoinKind::kNullAwareAnti, .key = y, .xs = {}},
        Case{.kind = plan::JoinKind::kOneRow, .key = {}, .xs = all},
        Case{.kind = plan::JoinKind::kLeft, .key = x, .xs = all, .right = all},
        Case{.kind = plan::JoinKind::kLeft, .key = y, .xs = all, .right = non_null_y},
        Case{.kind = plan::JoinKind::kSemi, .key = x, .residual = true, .xs = {1, 2, 4, 5, 7, 8}},
        Case{.kind = plan::JoinKind::kAnti, .key = x, .residual = true, .xs = {0, 3, 6, 9}},
        Case{.kind = plan::JoinKind::kLeft,
             .key = x,
             .residual = true,
             .xs = all,
             .right = non_null_y}}) {
    SCOPED_TRACE(std::string(plan::ToString(c.kind)) +
                 (c.key.has_value() ? " on " + c.key->name : "") +
                 (c.residual ? " with a residual" : ""));
    const bool one_row = c.kind == plan::JoinKind::kOneRow;
    const bool wide = one_row || c.kind == plan::JoinKind::kLeft;
    std::vector<plan::JoinKey> keys;
    if (c.key.has_value()) {
      keys.push_back(plan::JoinKey{.left = *c.key, .right = *c.key});
    }
    const auto join = Node(plan::JoinNode{
        .kind = c.kind,
        .left = scan,
        .right = one_row ? row : scan,
        .keys = std::move(keys),
        .residual = c.residual ? std::vector<plan::ExprPtr>{greater} : std::vector<plan::ExprPtr>{},
        .build = plan::BuildSide::kRight,
        .span = {}});
    const std::size_t width = wide ? 4 : 2;
    const auto rows = Run(PlanOf(join, width));
    ASSERT_NE(rows, nullptr);
    EXPECT_EQ(Int64Column(*rows, 0), c.xs);
    if (c.kind == plan::JoinKind::kLeft) {
      EXPECT_EQ(Int64Column(*rows, 2), c.right);
    }
    if (one_row) {
      EXPECT_EQ(Int64Column(*rows, 2), Values(10, 0));
      EXPECT_EQ(Int64Column(*rows, 3), Values(10, 10));
    }
    const auto n = static_cast<int64_t>(c.xs.size());  // distinct xs: as many groups
    for (const Placement& placement : Placements(join, width, scan)) {
      int64_t expected = n;
      if (placement.rows == Expect::kOne) {
        expected = 1;
      } else if (placement.rows == Expect::kLimit) {
        expected = std::min<int64_t>(n, 3);
      }
      for (const bool profiled : {false, true}) {
        ProfileNode profile;
        auto op = BuildPhysicalPlan(PlanOf(placement.root, placement.width),
                                    profiled ? &profile : nullptr);
        ASSERT_TRUE(op.ok()) << plan::NodeName(*placement.root) << ": " << op.status().ToString();
        ExecContext ctx{.pool = arrow::default_memory_pool(), .batch_size = 3};
        const auto result = Drain(**op, ctx);
        ASSERT_TRUE(result.ok()) << plan::NodeName(*placement.root) << ": "
                                 << result.status().ToString();
        EXPECT_EQ((*result)->num_rows(), expected) << plan::NodeName(*placement.root);
      }
    }
  }
}

// What a correct plan never holds is Invalid, never unsupported: a join without its inputs or
// keys, a key of two types, a DOUBLE or BOOLEAN key, a key outside its input on either side, a
// residual that is missing, not BOOLEAN or outside the join; a join of another kind than inner
// that builds on its left input, a one-row join with keys or residuals, a semi, anti, null-aware
// anti or left join without keys, a null-aware anti join of two keys or with a residual, a semi,
// anti or left join with a residual that is missing, not BOOLEAN or outside the join, or with a
// key of two types; in a part pipeline and over a serial probe input alike, with and without a
// profile (whose lines name the join by its EXPLAIN text). Each plan has its join's own width, so
// that only the join's shape can make it Invalid.
TEST_F(PhysicalPlannerTest, MalformedJoinsAreInvalidNotUnsupported) {
  const auto table = Table(/*split=*/true);
  const auto scan = Node(plan::ScanNode{.table = table, .table_name = "t", .fields = {0, 1}});
  const auto x = Column(0, "x", LogicalType::kBigInt);
  // A sorted scan: a probe input that is no part pipeline.
  const auto sorted = Node(plan::SortNode{.input = scan, .keys = {plan::SortKey{.column = x}}});
  const auto expr = [](int index, LogicalType type) {
    return std::make_shared<const plan::Expr>(
        plan::Expr{.node = plan::ColumnExpr{.index = index}, .type = type, .name = "c"});
  };
  const auto join_of = [&](const plan::LogicalNodePtr& probe, std::vector<plan::JoinKey> keys,
                           std::vector<plan::ExprPtr> residual) {
    return Node(plan::JoinNode{.kind = plan::JoinKind::kInner,
                               .left = probe,
                               .right = scan,
                               .keys = std::move(keys),
                               .residual = std::move(residual),
                               .build = plan::BuildSide::kRight,
                               .span = {}});
  };
  const auto key = [](plan::BoundColumn left, plan::BoundColumn right) {
    return std::vector<plan::JoinKey>{
        plan::JoinKey{.left = std::move(left), .right = std::move(right)}};
  };
  // Each plan has the width its root would return, so that the root-width check never decides.
  const auto width_of = [](const plan::LogicalNodePtr& root) { return WidthOf(root); };
  const auto expect_invalid = [&](const plan::LogicalNodePtr& root, std::string_view what) {
    for (const bool profiled : {false, true}) {
      ProfileNode profile;
      const auto status =
          BuildPhysicalPlan(PlanOf(root, width_of(root)), profiled ? &profile : nullptr).status();
      EXPECT_TRUE(status.IsInvalid())
          << what << (profiled ? ", profiled" : "") << ": " << status.ToString();
      EXPECT_EQ(plan::GetSqlError(status), nullptr) << what << ": a malformed plan is a bug";
    }
  };
  expect_invalid(Node(plan::JoinNode{}), "no inputs");
  for (const plan::LogicalNodePtr& probe : {scan, sorted}) {
    expect_invalid(join_of(probe, {}, {}), "no keys");
    expect_invalid(join_of(probe, key(x, Column(0, "x", LogicalType::kInteger)), {}),
                   "a key of two types");
    expect_invalid(
        join_of(probe,
                key(Column(0, "x", LogicalType::kDouble), Column(0, "x", LogicalType::kDouble)),
                {}),
        "a DOUBLE key");
    expect_invalid(
        join_of(probe,
                key(Column(0, "x", LogicalType::kBoolean), Column(0, "x", LogicalType::kBoolean)),
                {}),
        "a BOOLEAN key");
    expect_invalid(join_of(probe, key(Column(2, "x", LogicalType::kBigInt), x), {}),
                   "a probe key outside its input");
    expect_invalid(join_of(probe, key(x, Column(-1, "x", LogicalType::kBigInt)), {}),
                   "a build key outside its input");
    expect_invalid(join_of(probe, key(x, x), {nullptr}), "a missing residual");
    expect_invalid(join_of(probe, key(x, x), {expr(1, LogicalType::kBigInt)}), "a BIGINT residual");
    expect_invalid(join_of(probe, key(x, x), {expr(4, LogicalType::kBoolean)}),
                   "a residual outside the join");
    // The other kinds: building on the left, keys or residuals where none go, a bad residual and a
    // bad key.
    const auto kind_of = [&](plan::JoinKind kind, std::vector<plan::JoinKey> keys,
                             std::vector<plan::ExprPtr> residual,
                             plan::BuildSide build = plan::BuildSide::kRight) {
      return Node(plan::JoinNode{.kind = kind,
                                 .left = probe,
                                 .right = scan,
                                 .keys = std::move(keys),
                                 .residual = std::move(residual),
                                 .build = build,
                                 .span = {}});
    };
    const auto y = Column(1, "y", LogicalType::kBigInt);
    const auto boolean = expr(0, LogicalType::kBoolean);
    for (const plan::JoinKind kind :
         {plan::JoinKind::kSemi, plan::JoinKind::kAnti, plan::JoinKind::kNullAwareAnti,
          plan::JoinKind::kOneRow, plan::JoinKind::kLeft}) {
      expect_invalid(kind_of(kind, key(x, x), {}, plan::BuildSide::kLeft),
                     "a join of another kind than inner that builds on its left input");
    }
    expect_invalid(kind_of(plan::JoinKind::kOneRow, key(x, x), {}), "a one-row join with a key");
    expect_invalid(kind_of(plan::JoinKind::kOneRow, {}, {boolean}),
                   "a one-row join with a residual");
    expect_invalid(kind_of(plan::JoinKind::kSemi, {}, {}), "a semi join without keys");
    expect_invalid(kind_of(plan::JoinKind::kAnti, {}, {}), "an anti join without keys");
    expect_invalid(kind_of(plan::JoinKind::kNullAwareAnti, {}, {}),
                   "a null-aware anti join without keys");
    expect_invalid(kind_of(plan::JoinKind::kLeft, {}, {}), "a left join without keys");
    expect_invalid(
        kind_of(plan::JoinKind::kNullAwareAnti,
                {plan::JoinKey{.left = x, .right = x}, plan::JoinKey{.left = y, .right = y}}, {}),
        "a null-aware anti join of two keys");
    expect_invalid(kind_of(plan::JoinKind::kNullAwareAnti, key(x, x), {boolean}),
                   "a null-aware anti join with a residual");
    expect_invalid(kind_of(plan::JoinKind::kSemi, key(x, x), {nullptr}),
                   "a semi join with a missing residual");
    expect_invalid(kind_of(plan::JoinKind::kSemi, key(x, x), {expr(1, LogicalType::kBigInt)}),
                   "a semi join with a BIGINT residual");
    expect_invalid(kind_of(plan::JoinKind::kAnti, key(x, x), {expr(3, LogicalType::kBigInt)}),
                   "an anti join with a BIGINT residual");
    expect_invalid(kind_of(plan::JoinKind::kLeft, key(x, x), {expr(3, LogicalType::kBigInt)}),
                   "a left join with a BIGINT residual");
    for (const plan::JoinKind kind :
         {plan::JoinKind::kSemi, plan::JoinKind::kAnti, plan::JoinKind::kLeft}) {
      expect_invalid(kind_of(kind, key(x, x), {expr(4, LogicalType::kBoolean)}),
                     "a residual outside the join of another kind");
    }
    expect_invalid(
        kind_of(plan::JoinKind::kAnti, key(x, Column(0, "x", LogicalType::kInteger)), {boolean}),
        "an anti join key of two types");
    expect_invalid(
        kind_of(plan::JoinKind::kLeft, key(x, Column(0, "x", LogicalType::kInteger)), {}),
        "a left join key of two types");
  }
  // A malformed join in a build input: in its part pipeline, and drained.
  for (const plan::LogicalNodePtr& probe : {scan, sorted}) {
    expect_invalid(Node(plan::JoinNode{.kind = plan::JoinKind::kInner,
                                       .left = scan,
                                       .right = join_of(probe, {}, {}),
                                       .keys = key(x, x),
                                       .residual = {},
                                       .build = plan::BuildSide::kRight,
                                       .span = {}}),
                   "no keys in a build input");
  }
}

// An inner join is a hash join (ADR 0022): over a probe that is a part pipeline, the pipeline's
// sink runs behind the operator that prepares its builds (BuildsFirstOperator), whatever the sink
// (the part union, an aggregate, a GROUP BY, either aggregation in two levels, a top-N); over a
// serial probe input, the probe is the HashJoinOperator itself. A plan without a join has no such
// wrapper. The rows match on equal non-NULL keys, a build on either side.
TEST_F(PhysicalPlannerTest, InnerJoinsArePlannedAsHashJoins) {
  const auto table = Table(/*split=*/true);
  const auto scan = Node(plan::ScanNode{.table = table, .table_name = "t", .fields = {0, 1}});
  const auto x = Column(0, "x", LogicalType::kBigInt);
  const auto y = Column(1, "y", LogicalType::kBigInt);
  const auto join_on = [&](const plan::BoundColumn& key, plan::BuildSide build,
                           const plan::LogicalNodePtr& left) {
    return Node(plan::JoinNode{.kind = plan::JoinKind::kInner,
                               .left = left,
                               .right = scan,
                               .keys = {plan::JoinKey{.left = key, .right = key}},
                               .residual = {},
                               .build = build,
                               .span = {}});
  };
  const auto root = [&](const plan::LogicalNodePtr& node, std::size_t width) {
    auto op = BuildPhysicalPlan(PlanOf(node, width));
    EXPECT_TRUE(op.ok()) << op.status().ToString();
    return op.ok() ? *std::move(op) : nullptr;
  };
  const auto builds_first = [](const std::unique_ptr<Operator>& op) {
    return dynamic_cast<const BuildsFirstOperator*>(op.get()) != nullptr;
  };
  const auto join = join_on(x, plan::BuildSide::kRight, scan);
  const plan::AggregateCall count{
      .kind = plan::AggKind::kCountStar, .arg = {}, .type = LogicalType::kBigInt};
  const plan::AggregateCall distinct{
      .kind = plan::AggKind::kCountDistinct, .arg = x, .type = LogicalType::kBigInt};
  EXPECT_TRUE(builds_first(root(join, 4)));
  EXPECT_TRUE(
      builds_first(root(Node(plan::AggregateNode{.input = join, .aggregates = {count}}), 1)));
  EXPECT_TRUE(builds_first(
      root(Node(plan::GroupAggregateNode{.input = join, .keys = {y}, .aggregates = {count}}), 2)));
  // COUNT(DISTINCT) next to another call: in two levels, without keys and with them.
  EXPECT_TRUE(builds_first(
      root(Node(plan::AggregateNode{.input = join, .aggregates = {distinct, count}}), 2)));
  EXPECT_TRUE(builds_first(root(
      Node(plan::GroupAggregateNode{.input = join, .keys = {y}, .aggregates = {distinct, count}}),
      3)));
  EXPECT_TRUE(builds_first(
      root(Node(plan::LimitNode{
               .input = Node(plan::SortNode{.input = join, .keys = {plan::SortKey{.column = x}}}),
               .limit = 2}),
           4)));
  EXPECT_FALSE(builds_first(root(scan, 2)));
  EXPECT_NE(dynamic_cast<const PartUnionOperator*>(root(scan, 2).get()), nullptr);
  // A probe input that is no part pipeline (a sort).
  const auto sorted = Node(plan::SortNode{.input = scan, .keys = {plan::SortKey{.column = x}}});
  EXPECT_NE(dynamic_cast<const HashJoinOperator*>(
                root(join_on(x, plan::BuildSide::kRight, sorted), 4).get()),
            nullptr);
  // x = 0..9 matches itself; y, NULL where x is a multiple of 3, matches 6 rows.
  for (const plan::BuildSide build : {plan::BuildSide::kRight, plan::BuildSide::kLeft}) {
    for (const plan::LogicalNodePtr& left : {scan, sorted}) {
      const auto by_x = Run(PlanOf(join_on(x, build, left), 4));
      ASSERT_NE(by_x, nullptr);
      EXPECT_EQ(Int64Column(*by_x, 0), Int64Column(*by_x, 2));
      EXPECT_EQ(Int64Column(*by_x, 0),
                (std::vector<std::optional<int64_t>>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}));
      const auto by_y = Run(PlanOf(join_on(y, build, left), 4));
      ASSERT_NE(by_y, nullptr);
      EXPECT_EQ(Int64Column(*by_y, 1),
                (std::vector<std::optional<int64_t>>{10, 20, 40, 50, 70, 80}));
      EXPECT_EQ(Int64Column(*by_y, 3), Int64Column(*by_y, 1));
    }
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
