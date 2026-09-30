// Query profiles (docs/adr/0015-query-profiles.md): the profile tree of a physical plan, its exact
// counts on any number of threads, and nothing profiled without a profile root.

#include "antb1/exec/profile.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <arrow/api.h>
#include <arrow/util/thread_pool.h>
#include <gtest/gtest.h>

#include "antb1/exec/operator.h"
#include "antb1/exec/physical_planner.h"
#include "antb1/plan/logical_plan.h"

#include "../profiled_operator.h"
#include "exec_test_util.h"

namespace antb1::exec {
namespace {

using plan::LogicalType;

class ProfileTest : public testing::ExecTest {};
using testing::Column;
using testing::Int64s;
using testing::MemoryTable;

// 20 parts of 7 rows: x = 0..139 (exact statistics per part), g = x % 5.
std::shared_ptr<MemoryTable> Table() {
  const auto schema =
      arrow::schema({arrow::field("x", arrow::int64()), arrow::field("g", arrow::int64())});
  arrow::RecordBatchVector batches;
  for (int64_t part = 0; part < 20; ++part) {
    std::vector<std::optional<int64_t>> x;
    std::vector<std::optional<int64_t>> g;
    for (int64_t i = 0; i < 7; ++i) {
      x.emplace_back((part * 7) + i);
      g.emplace_back(((part * 7) + i) % 5);
    }
    batches.push_back(arrow::RecordBatch::Make(schema, 7, {Int64s(x), Int64s(g)}));
  }
  return std::make_shared<MemoryTable>(schema, std::move(batches), /*split=*/true);
}

plan::LogicalNodePtr Node(plan::LogicalNode node) {
  return std::make_shared<const plan::LogicalNode>(std::move(node));
}

plan::LogicalPlan PlanOf(plan::LogicalNodePtr root, std::size_t width) {
  plan::LogicalPlan plan{.root = std::move(root), .output = {}};
  for (std::size_t i = 0; i < width; ++i) {
    plan.output.push_back({.name = "c" + std::to_string(i), .type = LogicalType::kBigInt});
  }
  return plan;
}

// Runs `plan` profiled, on `executor` (nullptr: one thread).
std::unique_ptr<ProfileNode> Profile(const plan::LogicalPlan& plan,
                                     arrow::internal::Executor* executor) {
  auto root = std::make_unique<ProfileNode>();
  auto op = BuildPhysicalPlan(plan, root.get());
  EXPECT_TRUE(op.ok()) << op.status().ToString();
  ExecContext ctx{.pool = arrow::default_memory_pool(),
                  .batch_size = 3,
                  .executor = executor,
                  .threads = executor == nullptr ? 1 : 4};
  const auto table = Drain(**op, ctx);
  EXPECT_TRUE(table.ok()) << table.status().ToString();
  return root;
}

std::optional<int64_t> MetricOf(const ProfileNode& node, const std::string& name) {
  for (const ProfileMetric& metric : node.metrics()) {
    if (metric.name == name) {
      return metric.value;
    }
  }
  return std::nullopt;
}

// The counts of a node and its inputs, one line per node: they must not depend on the threads.
std::string Counts(const ProfileNode& node, int depth = 0) {
  std::string out = std::string(static_cast<std::size_t>(depth) * 2, ' ') + node.name() +
                    " rows=" + std::to_string(node.rows()) +
                    " runs=" + std::to_string(node.instances()) +
                    (node.per_part() ? " per_part" : "");
  for (const char* metric : {"parts", "skipped", "groups"}) {
    if (const auto value = MetricOf(node, metric)) {
      out += std::string(" ") + metric + "=" + std::to_string(*value);
    }
  }
  out += '\n';
  for (const ProfileNode* child : node.children()) {
    out += Counts(*child, depth + 1);
  }
  return out;
}

// ORDER BY key0 DESC LIMIT 2 over GROUP BY g over WHERE x >= 72: a top-N over the partitioned
// GROUP BY over a per-part filter and scan. The filter's statistics skip the first 10 parts
// (x < 70); the scan reads the other 70 rows, the filter keeps 68, in 5 groups.
TEST_F(ProfileTest, TheProfileMirrorsThePhysicalPlan) {
  auto pool = arrow::internal::ThreadPool::Make(4);
  ASSERT_TRUE(pool.ok());
  const auto x = Column(0, "x", LogicalType::kBigInt);
  const auto g = Column(1, "g", LogicalType::kBigInt);
  const auto scan = Node(plan::ScanNode{.table = Table(), .table_name = "t", .fields = {0, 1}});
  const auto filter = Node(plan::FilterNode{
      .input = scan,
      .predicates = {testing::Compare(x, plan::CompareOp::kGe, testing::BigInt(72))}});
  const auto grouped = Node(plan::GroupAggregateNode{
      .input = filter,
      .keys = {g},
      .aggregates = {
          {.kind = plan::AggKind::kCountStar, .arg = {}, .type = LogicalType::kBigInt}}});
  const auto sort = Node(plan::SortNode{
      .input = grouped,
      .keys = {{.column = Column(0, "g", LogicalType::kBigInt), .descending = true}}});
  const auto plan = PlanOf(Node(plan::LimitNode{.input = sort, .limit = 2, .offset = 0}), 2);
  const std::string expected =
      "TopN rows=2 runs=1\n"
      "  PartGroupAggregate rows=5 runs=1 parts=10 skipped=10 groups=5\n"
      "    Filter rows=68 runs=10 per_part\n"
      "      Scan rows=70 runs=10 per_part\n";
  for (arrow::internal::Executor* executor :
       {static_cast<arrow::internal::Executor*>(nullptr),
        static_cast<arrow::internal::Executor*>(pool->get())}) {
    const auto root = Profile(plan, executor);
    EXPECT_EQ(Counts(*root), expected);
    EXPECT_EQ(root->detail(), "Sort g DESC NULLS LAST Limit 2");
    ASSERT_EQ(root->children().size(), 1U);
    const ProfileNode& aggregate = *root->children()[0];
    EXPECT_EQ(aggregate.detail(), "GroupAggregate keys=[g] COUNT(*) top-N per partition keep=2");
    EXPECT_TRUE(MetricOf(aggregate, "wait").has_value());
    EXPECT_TRUE(MetricOf(aggregate, "part_time").has_value());
    EXPECT_GE(root->time().count(), 0);
  }
}

// A COUNT(DISTINCT) of one column alone shows its physical rewrite: a scalar COUNT over a
// partitioned GROUP BY of the column; a LIMIT over the scan, the part union below the limit.
TEST_F(ProfileTest, PhysicalChoicesShow) {
  const auto g = Column(1, "g", LogicalType::kBigInt);
  const auto scan = Node(plan::ScanNode{.table = Table(), .table_name = "t", .fields = {0, 1}});
  const auto distinct =
      PlanOf(Node(plan::AggregateNode{.input = scan,
                                      .aggregates = {{.kind = plan::AggKind::kCountDistinct,
                                                      .arg = g,
                                                      .type = LogicalType::kBigInt}}}),
             1);
  EXPECT_EQ(Counts(*Profile(distinct, nullptr)),
            "ScalarAggregate rows=1 runs=1\n"
            "  PartGroupAggregate rows=5 runs=1 parts=20 skipped=0 groups=5\n"
            "    Scan rows=140 runs=20 per_part\n");
  const auto limit = PlanOf(Node(plan::LimitNode{.input = scan, .limit = 3, .offset = 0}), 2);
  EXPECT_EQ(Counts(*Profile(limit, nullptr)),
            "Limit rows=3 runs=1\n"
            "  PartUnion rows=3 runs=1 parts=20 skipped=0\n"
            "    Scan rows=3 runs=1 per_part\n");
}

// Every kind of aggregation and sort is named, with its metrics, the same on 1 and 4 threads: a
// top-N over parts, two-level aggregations (grouped and global), and serial GROUP BY, aggregate and
// sort over the output of another aggregation.
TEST_F(ProfileTest, EveryOperatorIsNamed) {
  auto pool = arrow::internal::ThreadPool::Make(4);
  ASSERT_TRUE(pool.ok());
  const auto x = Column(0, "x", LogicalType::kBigInt);
  const auto g = Column(1, "g", LogicalType::kBigInt);
  const auto scan = Node(plan::ScanNode{.table = Table(), .table_name = "t", .fields = {0, 1}});
  const plan::AggregateCall count{
      .kind = plan::AggKind::kCountStar, .arg = {}, .type = LogicalType::kBigInt};
  const plan::AggregateCall distinct_x{
      .kind = plan::AggKind::kCountDistinct, .arg = x, .type = LogicalType::kBigInt};
  const plan::AggregateCall distinct_g{
      .kind = plan::AggKind::kCountDistinct, .arg = g, .type = LogicalType::kBigInt};
  const auto top = PlanOf(
      Node(plan::LimitNode{
          .input = Node(plan::SortNode{.input = scan, .keys = {{.column = x, .descending = true}}}),
          .limit = 2,
          .offset = 0}),
      2);
  const auto two_level = PlanOf(
      Node(plan::GroupAggregateNode{.input = scan, .keys = {g}, .aggregates = {distinct_x, count}}),
      3);
  const auto global =
      PlanOf(Node(plan::AggregateNode{.input = scan, .aggregates = {distinct_x, distinct_g}}), 2);
  const auto inner =
      Node(plan::GroupAggregateNode{.input = scan, .keys = {x, g}, .aggregates = {count}});
  const auto key1 = Column(1, "g", LogicalType::kBigInt);
  const auto regrouped =
      PlanOf(Node(plan::SortNode{.input = Node(plan::GroupAggregateNode{
                                     .input = inner, .keys = {key1}, .aggregates = {count}}),
                                 .keys = {{.column = Column(0, "g", LogicalType::kBigInt)}}}),
             2);
  const auto counted = PlanOf(Node(plan::AggregateNode{.input = inner, .aggregates = {count}}), 1);
  for (arrow::internal::Executor* executor :
       {static_cast<arrow::internal::Executor*>(nullptr),
        static_cast<arrow::internal::Executor*>(pool->get())}) {
    const auto top_profile = Profile(top, executor);
    EXPECT_EQ(Counts(*top_profile),
              "PartTopN rows=2 runs=1 parts=20 skipped=0\n"
              "  Scan rows=140 runs=20 per_part\n");
    EXPECT_EQ(top_profile->detail(), "Sort x DESC NULLS LAST Limit 2 late=1 columns");
    EXPECT_EQ(MetricOf(*top_profile, "late_columns"), 1);
    EXPECT_TRUE(MetricOf(*top_profile, "merge").has_value());

    const auto two_level_profile = Profile(two_level, executor);
    EXPECT_EQ(Counts(*two_level_profile),
              "PartTwoLevelAggregate rows=5 runs=1 parts=20 skipped=0 groups=5\n"
              "  Scan rows=140 runs=20 per_part\n");
    for (const char* metric :
         {"sample_parts", "heavy_keys", "outer", "heavy_groups", "lanes_tail"}) {
      EXPECT_TRUE(MetricOf(*two_level_profile, metric).has_value()) << metric;
    }
    EXPECT_EQ(Counts(*Profile(global, executor)),
              "PartTwoLevelAggregate rows=1 runs=1 parts=20 skipped=0 groups=1\n"
              "  Scan rows=140 runs=20 per_part\n");
    EXPECT_EQ(Counts(*Profile(regrouped, executor)),
              "Sort rows=5 runs=1\n"
              "  GroupAggregate rows=5 runs=1\n"
              "    PartGroupAggregate rows=140 runs=1 parts=20 skipped=0 groups=140\n"
              "      Scan rows=140 runs=20 per_part\n");
    EXPECT_EQ(Counts(*Profile(counted, executor)),
              "ScalarAggregate rows=1 runs=1\n"
              "  PartGroupAggregate rows=140 runs=1 parts=20 skipped=0 groups=140\n"
              "    Scan rows=140 runs=20 per_part\n");
  }
}

// Under a LIMIT on several threads, parts beyond the limit may have started: the limit's rows are
// exact, the parts' counts are not (the docs say so).
TEST_F(ProfileTest, ALimitStopsParts) {
  auto pool = arrow::internal::ThreadPool::Make(4);
  ASSERT_TRUE(pool.ok());
  const auto scan = Node(plan::ScanNode{.table = Table(), .table_name = "t", .fields = {0, 1}});
  const auto root = Profile(
      PlanOf(Node(plan::LimitNode{.input = scan, .limit = 3, .offset = 0}), 2), pool->get());
  EXPECT_EQ(root->name(), "Limit");
  EXPECT_EQ(root->rows(), 3);
  const ProfileNode& scan_node = *root->children()[0]->children()[0];
  EXPECT_EQ(scan_node.name(), "Scan");
  EXPECT_GE(scan_node.rows(), 3);
  EXPECT_GE(scan_node.instances(), 1);
  EXPECT_LE(scan_node.instances(), 20);
}

// A failing part fails the profiled query with the status of the unprofiled one; the operators
// that ran are counted.
TEST_F(ProfileTest, FailuresPassThrough) {
  const auto table = Table();
  table->FailPart(4);
  const auto scan = Node(plan::ScanNode{.table = table, .table_name = "t", .fields = {0, 1}});
  const auto plan =
      PlanOf(Node(plan::GroupAggregateNode{.input = scan,
                                           .keys = {Column(1, "g", LogicalType::kBigInt)},
                                           .aggregates = {{.kind = plan::AggKind::kCountStar,
                                                           .arg = {},
                                                           .type = LogicalType::kBigInt}}}),
             2);
  ExecContext ctx{.pool = arrow::default_memory_pool(), .batch_size = 3};
  auto plain = BuildPhysicalPlan(plan);
  ASSERT_TRUE(plain.ok());
  const auto expected = Drain(**plain, ctx);
  ASSERT_FALSE(expected.ok());
  ProfileNode root;
  auto profiled = BuildPhysicalPlan(plan, &root);
  ASSERT_TRUE(profiled.ok());
  const auto failed = Drain(**profiled, ctx);
  EXPECT_EQ(failed.status().ToString(), expected.status().ToString());
  EXPECT_EQ(root.instances(), 1);
  EXPECT_EQ(root.rows(), 0);
  EXPECT_EQ(root.children()[0]->instances(), 5) << "parts 0 .. 4 ran";
}

// Without a profile root nothing is wrapped: the plan is the one of an unprofiled query.
TEST_F(ProfileTest, NothingIsProfiledWithoutARoot) {
  const auto scan = Node(plan::ScanNode{.table = Table(), .table_name = "t", .fields = {0}});
  const auto plan = PlanOf(scan, 1);
  const auto plain = BuildPhysicalPlan(plan);
  ASSERT_TRUE(plain.ok());
  EXPECT_EQ(dynamic_cast<const ProfiledOperator*>(plain->get()), nullptr);
  ProfileNode root;
  const auto profiled = BuildPhysicalPlan(plan, &root);
  ASSERT_TRUE(profiled.ok());
  EXPECT_NE(dynamic_cast<const ProfiledOperator*>(profiled->get()), nullptr);
  EXPECT_EQ(root.name(), "PartUnion");
}

// Metrics add up (or keep their maximum) across threads, in the order of their first use;
// children are created on demand and found again; a timer without a node does nothing.
TEST_F(ProfileTest, NodesAccumulateAcrossThreads) {
  ProfileNode node;
  std::vector<std::thread> threads;
  threads.reserve(4);
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&node, t] {
      for (int i = 0; i < 1000; ++i) {
        node.AddRows(2);
        node.Add("count", MetricUnit::kCount, 1);
        node.Max("peak", MetricUnit::kBytes, (t * 1000) + i);
      }
      node.AddInstance();
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(node.rows(), 8000);
  EXPECT_EQ(node.batches(), 4000);
  EXPECT_EQ(node.instances(), 4);
  EXPECT_EQ(MetricOf(node, "count"), 4000);
  EXPECT_EQ(MetricOf(node, "peak"), 3999);
  ProfileNode* child = node.Child(1);
  EXPECT_EQ(node.children().size(), 2U);
  EXPECT_EQ(node.Child(1), child);
  {
    const ProfileTimer nothing(nullptr, "time");
  }
  {
    const ProfileTimer timed(&node, "time");
  }
  const std::vector<ProfileMetric> metrics = node.metrics();
  ASSERT_EQ(metrics.size(), 3U);
  EXPECT_EQ(metrics[0].name, "count");
  EXPECT_EQ(metrics[2].name, "time");
  EXPECT_EQ(metrics[2].unit, MetricUnit::kNanos);
  EXPECT_GE(metrics[2].value, 0);
}

}  // namespace
}  // namespace antb1::exec
