// Query profiles (docs/adr/0015-query-profiles.md): the profile tree of a physical plan, its exact
// counts on any number of threads, and nothing profiled without a profile root.

#include "antb1/exec/profile.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include <arrow/api.h>
#include <arrow/util/thread_pool.h>
#include <gtest/gtest.h>

#include "antb1/exec/operator.h"
#include "antb1/exec/physical_planner.h"
#include "antb1/plan/explain.h"
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
// (x < 70); the scan reads the other 70 rows and applies the filter (pushed down), 68 pass, in 5
// groups.
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
      "      Scan rows=68 runs=10 per_part\n";
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
    ASSERT_EQ(aggregate.children().size(), 1U);
    ASSERT_EQ(aggregate.children()[0]->children().size(), 1U);
    EXPECT_EQ(aggregate.children()[0]->children()[0]->detail(),
              "Scan table=t source=memory columns=[x, g], 1 pushed predicate");
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

// 2 parts of 5 rows: k = 0..9 (unique and dense: the direct layout), v = 100 + k.
std::shared_ptr<MemoryTable> Dimension() {
  const auto schema =
      arrow::schema({arrow::field("k", arrow::int64()), arrow::field("v", arrow::int64())});
  arrow::RecordBatchVector batches;
  for (int64_t part = 0; part < 2; ++part) {
    std::vector<std::optional<int64_t>> k;
    std::vector<std::optional<int64_t>> v;
    for (int64_t i = 0; i < 5; ++i) {
      k.emplace_back((part * 5) + i);
      v.emplace_back(100 + (part * 5) + i);
    }
    batches.push_back(arrow::RecordBatch::Make(schema, 5, {Int64s(k), Int64s(v)}));
  }
  return std::make_shared<MemoryTable>(schema, std::move(batches), /*split=*/true);
}

// A hash join shows a HashJoin line for its probe and a HashBuild line, with the join's EXPLAIN
// text, under the operator that prepares the build: the sink of the probe pipeline (after the
// pipeline), or the probe itself over a serial input (after its input). The build's line has the
// rows its table holds and its parts; its input is profiled below it, per part for a part
// pipeline. The same counts on 1 and 4 threads. Every row of t matches one of the dimension's (g
// = k), on the 1:1 path.
TEST_F(ProfileTest, JoinsShowBuildAndProbeLines) {
  auto pool = arrow::internal::ThreadPool::Make(4);
  ASSERT_TRUE(pool.ok());
  const auto t = Node(plan::ScanNode{.table = Table(), .table_name = "t", .fields = {0, 1}});
  const auto d = Node(plan::ScanNode{.table = Dimension(), .table_name = "d", .fields = {0, 1}});
  const auto join_of = [](plan::LogicalNodePtr probe, plan::LogicalNodePtr build) {
    return Node(
        plan::JoinNode{.kind = plan::JoinKind::kInner,
                       .left = std::move(probe),
                       .right = std::move(build),
                       .keys = {plan::JoinKey{.left = Column(1, "g", LogicalType::kBigInt),
                                              .right = Column(0, "k", LogicalType::kBigInt)}},
                       .residual = {},
                       .build = plan::BuildSide::kRight,
                       .span = {}});
  };
  const auto count_of = [](plan::LogicalNodePtr input) {
    return PlanOf(Node(plan::AggregateNode{.input = std::move(input),
                                           .aggregates = {{.kind = plan::AggKind::kCountStar,
                                                           .arg = {},
                                                           .type = LogicalType::kBigInt}}}),
                  1);
  };
  const auto join = join_of(t, d);
  const auto sorted =
      Node(plan::SortNode{.input = t, .keys = {{.column = Column(0, "x", LogicalType::kBigInt)}}});
  const auto all_of_d = Node(plan::LimitNode{.input = d, .limit = std::nullopt});
  for (arrow::internal::Executor* executor :
       {static_cast<arrow::internal::Executor*>(nullptr),
        static_cast<arrow::internal::Executor*>(pool->get())}) {
    const auto probed = Profile(count_of(join), executor);
    EXPECT_EQ(Counts(*probed),
              "PartAggregate rows=1 runs=1 parts=20 skipped=0\n"
              "  HashJoin rows=140 runs=20 per_part\n"
              "    Scan rows=140 runs=20 per_part\n"
              "  HashBuild rows=10 runs=1 parts=2 skipped=0\n"
              "    Scan rows=10 runs=2 per_part\n");
    ASSERT_EQ(probed->children().size(), 2U);
    const ProfileNode& probe = *probed->children()[0];
    const ProfileNode& build = *probed->children()[1];
    EXPECT_EQ(probe.detail(), plan::ExplainNode(*join));
    EXPECT_EQ(build.detail(), plan::ExplainNode(*join));
    for (const char* metric : {"find", "gather", "window_rows"}) {
      EXPECT_TRUE(MetricOf(probe, metric).has_value()) << metric;
    }
    EXPECT_EQ(MetricOf(probe, "window_rows"), 140);
    for (const char* metric : {"part_time", "wait", "lanes_tail", "finish"}) {
      EXPECT_TRUE(MetricOf(build, metric).has_value()) << metric;
    }
    EXPECT_EQ(MetricOf(build, "null_keys"), 0);
    EXPECT_EQ(MetricOf(build, "unique"), 1);
    EXPECT_EQ(MetricOf(build, "direct"), 1);

    // Over a serial input (a sort), the probe prepares the build: its line comes after the input.
    EXPECT_EQ(Counts(*Profile(PlanOf(join_of(sorted, d), 4), executor)),
              "HashJoin rows=140 runs=1\n"
              "  Sort rows=140 runs=1\n"
              "    PartUnion rows=140 runs=1 parts=20 skipped=0\n"
              "      Scan rows=140 runs=20 per_part\n"
              "  HashBuild rows=10 runs=1 parts=2 skipped=0\n"
              "    Scan rows=10 runs=2 per_part\n");
    // A drained build input: its batches (two per part of d, in batches of 3) are the parts.
    EXPECT_EQ(Counts(*Profile(count_of(join_of(t, all_of_d)), executor)),
              "PartAggregate rows=1 runs=1 parts=20 skipped=0\n"
              "  HashJoin rows=140 runs=20 per_part\n"
              "    Scan rows=140 runs=20 per_part\n"
              "  HashBuild rows=10 runs=1 parts=4\n"
              "    Limit rows=10 runs=1\n"
              "      PartUnion rows=10 runs=1 parts=2 skipped=0\n"
              "        Scan rows=10 runs=2 per_part\n");
  }
}

// Semi, anti, null-aware anti, left and one-row joins show the lines of an inner join, HashJoin for
// the probe and HashBuild for the build, with the same counts on 1 and 4 threads: the probe's rows
// are the rows it returns, the build's those its table holds (a one-row join's single row). A
// probe that looks keys up has find; one that keeps every row without a lookup (an anti join over
// no build row) has none. A one-row probe has gather, and so does a left probe (window_rows on its
// 1:1 path) or one with residuals (residual). Over t's 140 rows (g = x % 5) and the dimension's
// keys 0..9 (v = 100 + k): a semi join on g keeps every row, an anti or null-aware anti join on x
// keeps the 130 rows of x >= 10, and an anti join over the dimension's keys above 100 every row:
// there are none, so statistics skip both of the build's parts. A left join on x pads those 130
// rows; with the residual v > 102 on g, a left join pads the rows of g < 3 and a semi join keeps
// the 56 others.
TEST_F(ProfileTest, JoinsOfOtherKindsShowBuildAndProbeLines) {
  auto pool = arrow::internal::ThreadPool::Make(4);
  ASSERT_TRUE(pool.ok());
  const auto t = Node(plan::ScanNode{.table = Table(), .table_name = "t", .fields = {0, 1}});
  const auto d = Node(plan::ScanNode{.table = Dimension(), .table_name = "d", .fields = {0, 1}});
  const auto k = Column(0, "k", LogicalType::kBigInt);
  const auto join_of = [](plan::JoinKind kind, plan::LogicalNodePtr probe,
                          plan::LogicalNodePtr build, std::vector<plan::JoinKey> keys,
                          std::vector<plan::ExprPtr> residual = {}) {
    return Node(plan::JoinNode{.kind = kind,
                               .left = std::move(probe),
                               .right = std::move(build),
                               .keys = std::move(keys),
                               .residual = std::move(residual),
                               .build = plan::BuildSide::kRight,
                               .span = {}});
  };
  // v > 102 over a pair: t's x and g, then the dimension's k and v.
  const std::vector<plan::ExprPtr> above = {std::make_shared<const plan::Expr>(
      plan::Expr{.node =
                     plan::PredicateExpr{
                         .predicate = testing::Compare(Column(0, "v", LogicalType::kBigInt),
                                                       plan::CompareOp::kGt, testing::BigInt(102)),
                         .operands = {std::make_shared<const plan::Expr>(plan::Expr{
                             .node = plan::ColumnExpr{.index = 3}, .type = LogicalType::kBigInt})}},
                 .type = LogicalType::kBoolean})};
  const auto on = [&](const plan::BoundColumn& probe_key) {
    return std::vector<plan::JoinKey>{plan::JoinKey{.left = probe_key, .right = k}};
  };
  const auto count_of = [](plan::LogicalNodePtr input) {
    return PlanOf(Node(plan::AggregateNode{.input = std::move(input),
                                           .aggregates = {{.kind = plan::AggKind::kCountStar,
                                                           .arg = {},
                                                           .type = LogicalType::kBigInt}}}),
                  1);
  };
  const auto x = Column(0, "x", LogicalType::kBigInt);
  const auto g = Column(1, "g", LogicalType::kBigInt);
  const auto none = Node(plan::FilterNode{
      .input = d, .predicates = {testing::Compare(k, plan::CompareOp::kGt, testing::BigInt(100))}});
  const auto row = Node(plan::AggregateNode{
      .input = d,
      .aggregates = {{.kind = plan::AggKind::kMax, .arg = k, .type = LogicalType::kBigInt}}});
  const std::string scan_lines = "    Scan rows=140 runs=20 per_part\n";
  const std::string dimension =
      "  HashBuild rows=10 runs=1 parts=2 skipped=0\n"
      "    Scan rows=10 runs=2 per_part\n";
  struct Case {
    std::string name;
    plan::LogicalNodePtr join;
    std::string counts;
    bool find = true;
    bool gather = false;
    bool window_rows = false;
  };
  const std::vector<Case> cases = {
      {.name = "semi",
       .join = join_of(plan::JoinKind::kSemi, t, d, on(g)),
       .counts = "  HashJoin rows=140 runs=20 per_part\n" + scan_lines + dimension},
      {.name = "anti",
       .join = join_of(plan::JoinKind::kAnti, t, d, on(x)),
       .counts = "  HashJoin rows=130 runs=20 per_part\n" + scan_lines + dimension},
      {.name = "null-aware anti",
       .join = join_of(plan::JoinKind::kNullAwareAnti, t, d, on(x)),
       .counts = "  HashJoin rows=130 runs=20 per_part\n" + scan_lines + dimension},
      {.name = "anti over no build row",
       .join = join_of(plan::JoinKind::kAnti, t, none, on(x)),
       .counts = "  HashJoin rows=140 runs=20 per_part\n" + scan_lines +
                 "  HashBuild rows=0 runs=1 parts=0 skipped=2\n"
                 "    Filter rows=0 runs=0 per_part\n"
                 "      Scan rows=0 runs=0 per_part\n",
       .find = false},
      {.name = "one-row",
       .join = join_of(plan::JoinKind::kOneRow, t, row, {}),
       .counts = "  HashJoin rows=140 runs=20 per_part\n" + scan_lines +
                 "  HashBuild rows=1 runs=1 parts=1\n"
                 "    PartAggregate rows=1 runs=1 parts=2 skipped=0\n"
                 "      Scan rows=10 runs=2 per_part\n",
       .find = false,
       .gather = true},
      {.name = "left",
       .join = join_of(plan::JoinKind::kLeft, t, d, on(x)),
       .counts = "  HashJoin rows=140 runs=20 per_part\n" + scan_lines + dimension,
       .gather = true,
       .window_rows = true},
      {.name = "left with a residual",
       .join = join_of(plan::JoinKind::kLeft, t, d, on(g), above),
       .counts = "  HashJoin rows=140 runs=20 per_part\n" + scan_lines + dimension,
       .gather = true},
      {.name = "semi with a residual",
       .join = join_of(plan::JoinKind::kSemi, t, d, on(g), above),
       .counts = "  HashJoin rows=56 runs=20 per_part\n" + scan_lines + dimension,
       .gather = true},
  };
  for (arrow::internal::Executor* executor :
       {static_cast<arrow::internal::Executor*>(nullptr),
        static_cast<arrow::internal::Executor*>(pool->get())}) {
    for (const Case& c : cases) {
      SCOPED_TRACE(c.name);
      const auto profile = Profile(count_of(c.join), executor);
      EXPECT_EQ(Counts(*profile), "PartAggregate rows=1 runs=1 parts=20 skipped=0\n" + c.counts);
      ASSERT_EQ(profile->children().size(), 2U);
      const ProfileNode& probe = *profile->children()[0];
      const ProfileNode& build = *profile->children()[1];
      EXPECT_EQ(probe.name(), "HashJoin");
      EXPECT_EQ(build.name(), "HashBuild");
      EXPECT_EQ(probe.detail(), plan::ExplainNode(*c.join));
      EXPECT_EQ(build.detail(), plan::ExplainNode(*c.join));
      EXPECT_EQ(MetricOf(probe, "find").has_value(), c.find);
      EXPECT_EQ(MetricOf(probe, "gather").has_value(), c.gather);
      EXPECT_EQ(MetricOf(probe, "window_rows"),
                c.window_rows ? std::optional<int64_t>(140) : std::nullopt);
      EXPECT_EQ(MetricOf(probe, "residual").has_value(),
                !std::get<plan::JoinNode>(*c.join).residual.empty());
      EXPECT_TRUE(MetricOf(build, "finish").has_value());
    }
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
