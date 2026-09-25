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

#include "antb1/plan/logical_plan.h"
#include "antb1/plan/sql_status.h"

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
  // x = 0..9 in batches of 4, 4 and 2; y = 10 * x, NULL where x is a multiple of 3.
  static std::shared_ptr<MemoryTable> Table() {
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
    return std::make_shared<MemoryTable>(schema, std::move(batches));
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

TEST_F(PhysicalPlannerTest, MalformedPlansAreInvalidNotUnsupported) {
  const auto table = Table();
  EXPECT_TRUE(BuildPhysicalPlan(plan::LogicalPlan{}).status().IsInvalid());
  plan::LogicalPlan no_output{.root = Node(plan::RowCountNode{.table = table}), .output = {}};
  EXPECT_TRUE(BuildPhysicalPlan(no_output).status().IsInvalid());
  const std::vector<plan::LogicalNode> broken = {
      plan::RowCountNode{}, plan::ScanNode{},      plan::FilterNode{},
      plan::ProjectNode{},  plan::AggregateNode{}, plan::LimitNode{},
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
