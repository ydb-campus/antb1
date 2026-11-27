#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <arrow/api.h>
#include <gtest/gtest.h>

#include "antb1/exec/memory_budget.h"
#include "antb1/exec/operator.h"
#include "antb1/exec/physical_planner.h"
#include "antb1/exec/profile.h"
#include "antb1/exec/table_scan.h"
#include "antb1/plan/logical_plan.h"

#include "../deferred_dimension_join.h"
#include "../shared_builds.h"
#include "exec_test_util.h"

namespace antb1::exec {
namespace {

class DeferredDimensionGuardTest : public testing::ExecTest {};

constexpr int64_t kWorkingBytes = 4096;
constexpr std::size_t kKiB = 1024;

std::shared_ptr<arrow::RecordBatch> Payload(std::size_t bytes) {
  const auto schema = arrow::schema({arrow::field("payload", arrow::binary())});
  return arrow::RecordBatch::Make(schema, 1, {testing::Strings({std::string(bytes, 'x')})});
}

plan::LogicalPlan Payloads(arrow::RecordBatchVector batches) {
  const auto schema = batches.front()->schema();
  auto table = std::make_shared<testing::MemoryTable>(schema, std::move(batches));
  const plan::ColumnId id{1};
  auto scan = std::make_shared<plan::LogicalNode>(
      plan::ScanNode{.table = std::move(table), .fields = {0}, .ids = {id}});
  return {.root = std::move(scan),
          .output = {{.name = "payload", .type = plan::LogicalType::kVarchar, .id = id}}};
}

TEST_F(DeferredDimensionGuardTest, RetainsSortedOutputWithinItsExistingPoolBudget) {
  const auto batch = Payload(16 * kKiB);
  auto sorted = Payloads(arrow::RecordBatchVector(8, batch));
  const plan::BoundColumn key{.id = plan::ColumnId{1}, .type = plan::LogicalType::kVarchar};
  sorted.root = std::make_shared<plan::LogicalNode>(
      plan::SortNode{.input = sorted.root, .keys = {{.column = key}}});

  // Measure the sort's real working set, including all eight sorted buffers held by Drain.
  auto ordinary = BuildPhysicalPlan(sorted);
  ASSERT_TRUE(ordinary.ok()) << ordinary.status();
  MemoryBudget measured(std::nullopt);
  const auto expected =
      Drain(**ordinary, {.pool = &measured, .batch_size = 1, .budget = &measured});
  ASSERT_TRUE(expected.ok()) << expected.status();
  ASSERT_EQ((*expected)->num_rows(), 8);

  // Leave room for the retained batch pointers, but less room than even one data buffer.
  MemoryBudget tight(measured.max_memory() + 4096);
  auto guarded = MakeDeferredDimensionJoin(sorted, Payloads({batch}), nullptr, BuildPhysicalPlan);
  ASSERT_TRUE(guarded.ok()) << guarded.status();
  const auto result = Drain(**guarded, {.pool = &tight, .batch_size = 1, .budget = &tight});
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_TRUE((*result)->Equals(**expected));  // fallback would return one row
}

TEST_F(DeferredDimensionGuardTest, RetriesWhenTheRetainedOutputExceedsTheSizeCap) {
  // Reuse a single buffer to exercise the logical size cap without a large allocation.
  const auto batch = Payload(1024 * kKiB);
  auto guarded = MakeDeferredDimensionJoin(Payloads(arrow::RecordBatchVector(257, batch)),
                                           Payloads({batch}), nullptr, BuildPhysicalPlan);
  ASSERT_TRUE(guarded.ok()) << guarded.status();
  const auto result = Drain(**guarded, {});
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_EQ((*result)->num_rows(), 1);
}

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
  // The working buffer fits exactly. Allocating even the first batch pointer must now retry
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

class GuardScanState final : public plan::TableScanState {
 public:
  arrow::Status FinishPart(int64_t /*part*/, arrow::MemoryPool* /*pool*/) override {
    ++checks;
    return status;
  }
  arrow::Status status;
  int checks = 0;
  int reads = 0;
};

class GuardScanTable final : public plan::Table {
 public:
  const std::shared_ptr<arrow::Schema>& schema() const override { return schema_; }
  std::optional<int64_t> exact_row_count() const override { return 1; }
  std::string Describe() const override { return "guard scan"; }
  std::shared_ptr<plan::TableScanState> MakeScanState() const override { return state; }
  std::shared_ptr<GuardScanState> state = std::make_shared<GuardScanState>();

 protected:
  arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> DoScan(
      const std::vector<int>& /*fields*/, int64_t /*size*/, arrow::MemoryPool* /*pool*/,
      const std::shared_ptr<plan::ScanContext>& context) const override {
    ARROW_RETURN_NOT_OK(context->Get(*this).status());
    ++state->reads;
    auto row = arrow::RecordBatch::Make(schema_, 1, {testing::Int64s({42})});
    return std::make_unique<testing::VectorReader>(schema_, arrow::RecordBatchVector{row});
  }

 private:
  const std::shared_ptr<arrow::Schema> schema_ = arrow::schema({arrow::field("n", arrow::int64())});
};

TEST_F(DeferredDimensionGuardTest, EmptyJoinedResultDiscardsUnusedProducerScans) {
  const auto probe = std::make_shared<GuardScanTable>();
  const auto empty = std::make_shared<testing::MemoryTable>(
      probe->schema(), arrow::RecordBatchVector{}, /*split=*/true);
  const auto scan = [](std::shared_ptr<plan::Table> table, plan::ColumnId id) {
    return std::make_shared<plan::LogicalNode>(
        plan::ScanNode{.table = std::move(table), .fields = {0}, .ids = {id}});
  };
  const plan::ColumnId left{1};
  const plan::ColumnId right{2};
  const plan::JoinNode join{.kind = plan::JoinKind::kInner,
                            .left = scan(probe, left),
                            .right = scan(empty, right),
                            .keys = {{.left = {.id = left}, .right = {.id = right}}}};
  const plan::LogicalPlan joined{
      .root = std::make_shared<plan::LogicalNode>(join),
      .output = {{.name = "left", .id = left}, {.name = "right", .id = right}}};
  const auto candidate = [](const plan::LogicalPlan& plan,
                            ProfileNode* profile) -> arrow::Result<std::unique_ptr<Operator>> {
    ARROW_ASSIGN_OR_RAISE(auto inner, BuildPhysicalPlan(plan, profile));
    const auto& input = std::get<plan::JoinNode>(*plan.root).left;
    const auto table = std::get<plan::ScanNode>(*input).table;
    std::vector<SharedBuild> producers;
    producers.push_back({.table = std::make_shared<MaterializedTable>(table->schema()),
                         .make_producer = [table] -> arrow::Result<std::unique_ptr<Operator>> {
                           return std::make_unique<TableScanOperator>(table, std::vector<int>{0});
                         },
                         .combined_domains = true});
    return std::make_unique<SharedBuildsOperator>(
        std::move(inner), std::move(producers),
        [plan](ProfileNode* fallback) { return BuildPhysicalPlan(plan, fallback); });
  };
  ProfileNode profile;
  const auto op = MakeDeferredDimensionJoin(joined, joined, &profile, BuildPhysicalPlan, candidate);
  ASSERT_TRUE(op.ok()) << op.status();
  ExecContext ctx{.scans = std::make_shared<plan::ScanContext>()};
  ASSERT_TRUE((*op)->Open(ctx).ok());
  const auto end = (*op)->Next();
  ASSERT_TRUE(end.ok()) << end.status();
  EXPECT_TRUE(end->end());
  ASSERT_TRUE((*op)->Close().ok());
  EXPECT_EQ(probe->state->reads, 1);  // the eager producer only: both joins skip the probe
  probe->state->status = arrow::Status::IOError("probe file removed after the join");
  EXPECT_TRUE(ctx.scans->Finish(ctx.pool).ok());
  EXPECT_EQ(probe->state->checks, 0);
  EXPECT_TRUE(std::ranges::any_of(profile.metrics(), [](const ProfileMetric& metric) {
    return metric.name == "dimension_fallbacks" && metric.value == 1;
  }));
}

TEST_F(DeferredDimensionGuardTest, CallerFinishesOnlyTheChosenScansAfterReceivingTheirRows) {
  const auto plan_for = [](const std::shared_ptr<GuardScanTable>& table, const char* name) {
    auto root = std::make_shared<plan::LogicalNode>(plan::ScanNode{
        .table = table, .table_name = name, .fields = {0}, .ids = {plan::ColumnId{1}}});
    return plan::LogicalPlan{.root = std::move(root),
                             .output = {{.name = "n", .id = plan::ColumnId{1}}}};
  };
  const auto build = [](const plan::LogicalPlan& logical,
                        ProfileNode* /*profile*/) -> arrow::Result<std::unique_ptr<Operator>> {
    const auto& scan = std::get<plan::ScanNode>(*logical.root);
    const auto make_scan = [table = scan.table] {
      return std::make_unique<TableScanOperator>(table, std::vector<int>{0});
    };
    if (scan.table_name == "candidate") {
      return make_scan();
    }
    // A nested shared build replaces its own scan context on retry during Open.
    std::vector<SharedBuild> builds;
    builds.push_back({.table = std::make_shared<MaterializedTable>(scan.table->schema()),
                      .make_producer = [] -> arrow::Result<std::unique_ptr<Operator>> {
                        return arrow::Status::Invalid("retry shared producer");
                      }});
    return std::make_unique<SharedBuildsOperator>(
        make_scan(), std::move(builds),
        [make_scan](ProfileNode*) -> arrow::Result<std::unique_ptr<Operator>> {
          return make_scan();
        });
  };
  for (const bool retry : {false, true}) {
    SCOPED_TRACE(retry);
    const auto candidate = std::make_shared<GuardScanTable>();
    const auto original = std::make_shared<GuardScanTable>();
    if (retry) {
      candidate->state->status = arrow::Status::IOError("candidate validation failed");
    }
    const auto op = MakeDeferredDimensionJoin(plan_for(candidate, "candidate"),
                                              plan_for(original, "original"), nullptr, build);
    ASSERT_TRUE(op.ok()) << op.status();
    ExecContext ctx{.scans = std::make_shared<plan::ScanContext>()};
    GuardScanTable earlier;
    ASSERT_TRUE(ctx.scans->Get(earlier).ok());
    ASSERT_TRUE((*op)->Open(ctx).ok());
    const auto batch = (*op)->Next();
    ASSERT_TRUE(batch.ok()) << batch.status();
    EXPECT_EQ(batch->selected_rows(), 1);
    const auto end = (*op)->Next();
    ASSERT_TRUE(end.ok()) << end.status();
    EXPECT_TRUE(end->end());
    ASSERT_TRUE((*op)->Close().ok());
    ASSERT_TRUE(ctx.scans->Finish(ctx.pool).ok());
    EXPECT_EQ(earlier.state->checks, 1);
    EXPECT_EQ(original->state->checks, retry ? 1 : 0);
    EXPECT_EQ(candidate->state->checks, retry ? 1 : 2);
    const auto& winner = retry ? original : candidate;
    winner->state->status = arrow::Status::IOError("chosen input changed before completion");
    EXPECT_EQ(ctx.scans->Finish(ctx.pool), winner->state->status);
    if (retry) {
      // A direct Open without an enclosing context still finishes the fallback on exhaustion.
      ExecContext direct;
      ASSERT_TRUE((*op)->Open(direct).ok());
      const auto next = (*op)->Next();
      ASSERT_TRUE(next.ok()) << next.status();
      EXPECT_EQ(next->selected_rows(), 1);
      EXPECT_EQ((*op)->Next().status(), original->state->status);
      EXPECT_TRUE((*op)->Close().ok());
    }
  }
}

TEST_F(DeferredDimensionGuardTest, OnlyASeparateContextCanBeRetainedForFinalValidation) {
  const auto scans = std::make_shared<plan::ScanContext>();
  EXPECT_TRUE(scans->KeepForFinish(nullptr).IsInvalid());
  EXPECT_TRUE(scans->KeepForFinish(scans).IsInvalid());
  EXPECT_TRUE(scans->Finish(arrow::default_memory_pool()).ok());
}

}  // namespace
}  // namespace antb1::exec
