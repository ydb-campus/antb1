#include <cstdint>
#include <memory>
#include <utility>

#include <arrow/api.h>
#include <gtest/gtest.h>

#include "antb1/exec/memory_budget.h"
#include "antb1/exec/operator.h"
#include "antb1/exec/profile.h"
#include "antb1/plan/logical_plan.h"

#include "../deferred_dimension_join.h"
#include "exec_test_util.h"

namespace antb1::exec {
namespace {

class DeferredDimensionGuardTest : public testing::ExecTest {};

constexpr int64_t kWorkingBytes = 4096;

// The speculative input emits a row, then fails while retaining its working buffer until
// destruction. Opening the original input checks that the retry has already reclaimed it.
class RetryInput final : public Operator {
 public:
  explicit RetryInput(bool speculative) : speculative_(speculative) {}

  const std::shared_ptr<arrow::Schema>& output_schema() const override { return schema_; }

  arrow::Status Open(ExecContext& ctx) override {
    if (speculative_) {
      ARROW_ASSIGN_OR_RAISE(working_, arrow::AllocateBuffer(kWorkingBytes, ctx.pool));
    } else {
      EXPECT_EQ(ctx.budget->bytes_allocated(), 0);
    }
    emitted_ = false;
    return arrow::Status::OK();
  }

  arrow::Result<Batch> Next() override {
    if (!emitted_) {
      emitted_ = true;
      const int64_t value = speculative_ ? 11 : 22;
      return Batch{.data = arrow::RecordBatch::Make(schema_, 1, {testing::Int64s({value})})};
    }
    if (speculative_) {
      profile()->Add("injected_failures", MetricUnit::kCount, 1);
      return arrow::Status::Invalid("error after the speculative prefix");
    }
    return Batch{};
  }

  arrow::Status Close() override { return arrow::Status::OK(); }

 private:
  const std::shared_ptr<arrow::Schema> schema_ = arrow::schema({arrow::field("n", arrow::int64())});
  std::unique_ptr<arrow::Buffer> working_;
  bool speculative_;
  bool emitted_ = false;
};

TEST_F(DeferredDimensionGuardTest, DiscardsPartialResultsAndWorkingMemoryBeforeEachRetry) {
  const auto schema = arrow::schema(
      {arrow::field("surrogate", arrow::int64()), arrow::field("code", arrow::utf8())});
  const auto batch =
      arrow::RecordBatch::Make(schema, 2, {testing::Int64s({1, 2}), testing::Strings({"A", "B"})});
  const auto dimension =
      std::make_shared<testing::MemoryTable>(schema, arrow::RecordBatchVector{batch});
  const auto plan_for = [&](const char* name) {
    auto root = std::make_shared<plan::LogicalNode>(plan::ScanNode{
        .table = dimension, .table_name = name, .fields = {0}, .ids = {plan::ColumnId{1}}});
    return plan::LogicalPlan{.root = std::move(root),
                             .output = {{.name = "n", .id = plan::ColumnId{1}}}};
  };
  const auto build = [](const plan::LogicalPlan& logical,
                        ProfileNode* profile) -> arrow::Result<std::unique_ptr<Operator>> {
    auto input =
        std::make_unique<RetryInput>(std::get<plan::ScanNode>(*logical.root).table_name == "fast");
    input->set_profile(profile);
    return input;
  };
  ProfileNode profile;
  auto op = MakeDeferredDimensionJoin(plan_for("fast"), plan_for("original"), &profile, build);
  ASSERT_TRUE(op.ok()) << op.status();
  MemoryBudget budget(16 * 1024 * 1024);
  for (int run = 0; run < 2; ++run) {
    const auto result = Drain(**op, {.pool = &budget, .budget = &budget});
    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(testing::SingleInt64(**result), 22);
  }
  // The working buffer fits exactly. Reserving even the first output batch must now retry
  // before the speculative input can reach its injected error on the next pull.
  MemoryBudget tight(kWorkingBytes);
  const auto retried = Drain(**op, {.pool = &tight, .budget = &tight});
  ASSERT_TRUE(retried.ok()) << retried.status();
  EXPECT_EQ(testing::SingleInt64(**retried), 22);
  EXPECT_EQ(tight.max_memory(), kWorkingBytes);
  EXPECT_EQ(tight.bytes_allocated(), 0);
  int64_t failures = 0;
  for (const auto* child : profile.children()) {
    for (const auto& metric : child->metrics()) {
      if (metric.name == "injected_failures") {
        failures += metric.value;
      }
    }
  }
  EXPECT_EQ(failures, 2);
  EXPECT_EQ(budget.bytes_allocated(), 0);
}

}  // namespace
}  // namespace antb1::exec
