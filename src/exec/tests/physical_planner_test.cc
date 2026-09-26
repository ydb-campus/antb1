#include "antb1/exec/physical_planner.h"

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
#include "antb1/plan/table.h"

namespace antb1::exec {
namespace {

class CountOnlyTable final : public plan::Table {
 public:
  explicit CountOnlyTable(std::optional<int64_t> rows)
      : schema_(arrow::schema({arrow::field("x", arrow::int64())})), rows_(rows) {}
  const std::shared_ptr<arrow::Schema>& schema() const override { return schema_; }
  std::optional<int64_t> exact_row_count() const override { return rows_; }
  arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> Scan(
      const std::vector<int>& /*fields*/, int64_t /*batch_size*/) const override {
    return arrow::Status::NotImplemented("not scannable");
  }
  std::string Describe() const override { return "count-only"; }

 private:
  std::shared_ptr<arrow::Schema> schema_;
  std::optional<int64_t> rows_;
};

plan::LogicalNodePtr Node(plan::LogicalNode node) {
  return std::make_shared<const plan::LogicalNode>(std::move(node));
}

plan::LogicalPlan PlanOf(plan::LogicalNodePtr root) {
  return plan::LogicalPlan{.root = std::move(root),
                           .output = {{.name = "c", .type = plan::LogicalType::kBigInt}}};
}

TEST(PhysicalPlannerTest, RowCountRuns) {
  const auto table = std::make_shared<CountOnlyTable>(42);
  auto op = BuildPhysicalPlan(PlanOf(Node(plan::RowCountNode{.table = table, .table_name = "t"})));
  ASSERT_TRUE(op.ok()) << op.status().ToString();
  ExecContext ctx;
  auto result = Drain(**op, ctx);
  ASSERT_TRUE(result.ok()) << result.status().ToString();
  EXPECT_EQ((*result)->schema()->field(0)->name(), "c");
  EXPECT_EQ(std::static_pointer_cast<arrow::Int64Array>((*result)->column(0)->chunk(0))->Value(0),
            42);
}

TEST(PhysicalPlannerTest, RowCountWithoutAnExactCountIsInvalid) {
  const auto table = std::make_shared<CountOnlyTable>(std::nullopt);
  auto op = BuildPhysicalPlan(PlanOf(Node(plan::RowCountNode{.table = table, .table_name = "t"})));
  EXPECT_TRUE(op.status().IsInvalid());
  EXPECT_EQ(plan::GetSqlError(op.status()), nullptr);
}

TEST(PhysicalPlannerTest, EmptyPlanIsInvalid) {
  EXPECT_TRUE(BuildPhysicalPlan(plan::LogicalPlan{}).status().IsInvalid());
  const auto table = std::make_shared<CountOnlyTable>(1);
  plan::LogicalPlan no_output{.root = Node(plan::RowCountNode{.table = table}), .output = {}};
  EXPECT_TRUE(BuildPhysicalPlan(no_output).status().IsInvalid());
}

// Every node that needs table data is Unsupported (exit code 4) for now, at the node's span.
TEST(PhysicalPlannerTest, NodesThatReadDataAreUnsupportedYet) {
  const auto table = std::make_shared<CountOnlyTable>(1);
  const auto scan = Node(plan::ScanNode{
      .table = table, .table_name = "t", .fields = {0}, .span = {.offset = 1, .length = 1}});
  const std::vector<std::pair<plan::LogicalNode, std::string>> nodes = {
      {plan::ScanNode{.table = table, .span = {.offset = 1, .length = 1}}, "a table scan"},
      {plan::FilterNode{.input = scan, .span = {.offset = 2, .length = 1}}, "WHERE"},
      {plan::ProjectNode{.input = scan, .span = {.offset = 3, .length = 1}}, "selecting columns"},
      {plan::AggregateNode{.input = scan, .span = {.offset = 4, .length = 1}},
       "an aggregate over table data"},
      {plan::LimitNode{.input = scan, .span = {.offset = 5, .length = 1}}, "LIMIT"},
  };
  for (const auto& [node, what] : nodes) {
    auto op = BuildPhysicalPlan(PlanOf(Node(node)));
    ASSERT_FALSE(op.ok()) << what;
    EXPECT_TRUE(op.status().IsNotImplemented()) << what;
    const auto detail = plan::GetSqlError(op.status());
    ASSERT_NE(detail, nullptr) << what;
    EXPECT_EQ(detail->kind(), plan::SqlErrorDetail::Kind::kUnsupported) << what;
    EXPECT_EQ(detail->span(), plan::SpanOf(node)) << what;
    EXPECT_TRUE(op.status().message().starts_with(what + " cannot be executed yet"))
        << op.status().message();
  }
}

}  // namespace
}  // namespace antb1::exec
