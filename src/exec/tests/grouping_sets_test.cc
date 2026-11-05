// ROLLUP, CUBE and GROUPING SETS (docs/adr/0038-grouping-sets.md): the shape of the two levels
// (PlanGroupingSets, MergesExactly), GroupTable::MergeProjected, and the serial and parallel
// operators against the GROUP BY of each set.

#include "antb1/exec/grouping_sets.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/compute/exec.h>
#include <arrow/util/thread_pool.h>
#include <gtest/gtest.h>

#include "antb1/exec/group_aggregate.h"
#include "antb1/exec/memory_budget.h"
#include "antb1/exec/operator.h"
#include "antb1/exec/physical_planner.h"
#include "antb1/exec/profile.h"
#include "antb1/exec/scalar_aggregate.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

#include "../group_table.h"
#include "../grouping_sets_shape.h"
#include "../part_operators.h"
#include "exec_test_util.h"

namespace antb1::exec {
namespace {

using plan::AggKind;
using plan::LogicalType;
using testing::Column;
using testing::MemoryTable;
using Sets = std::vector<std::vector<std::size_t>>;

constexpr int kThreads = 4;

std::shared_ptr<arrow::internal::ThreadPool> Pool() {
  auto pool = arrow::internal::ThreadPool::Make(kThreads);
  EXPECT_TRUE(pool.ok()) << pool.status().ToString();
  return *pool;
}

// ---- PlanGroupingSets and MergesExactly ----

TEST(GroupingSetsShapeTest, RootsAreTheSetsNoOtherSetContains) {
  // ROLLUP (a, b, c): the finest set is the only root; every other set is merged from it.
  const GroupingSetsShape rollup = PlanGroupingSets({{0, 1, 2}, {0, 1}, {0}, {}}, true);
  EXPECT_EQ(rollup.sets.size(), 4U);
  EXPECT_EQ(rollup.sources, (std::vector<std::size_t>{0, 0, 0, 0}));
  EXPECT_EQ(rollup.columns[1], (std::vector<std::size_t>{0, 1}));
  EXPECT_TRUE(rollup.columns[3].empty());
  // GROUPING SETS ((a, b), (b, c), (b), ()): two roots; (b) and () from the first that contains
  // them, at a position of its own in each.
  const GroupingSetsShape two = PlanGroupingSets({{0, 1}, {1, 2}, {1}, {}}, true);
  EXPECT_EQ(two.sources, (std::vector<std::size_t>{0, 1, 0, 0}));
  EXPECT_EQ(two.columns[2], (std::vector<std::size_t>{1}));
  // The smallest root that contains a set is its source.
  const GroupingSetsShape smallest = PlanGroupingSets({{0, 1, 2}, {3, 4}, {3}}, true);
  EXPECT_EQ(smallest.sources, (std::vector<std::size_t>{0, 1, 1}));
  EXPECT_EQ(smallest.columns[2], (std::vector<std::size_t>{0}));
  // Without exact merges, every distinct set is a root.
  const GroupingSetsShape fallback = PlanGroupingSets({{0, 1}, {0}, {}}, false);
  EXPECT_EQ(fallback.sources, (std::vector<std::size_t>{0, 1, 2}));
}

TEST(GroupingSetsShapeTest, DuplicateSetsAreAggregatedOnceAndEmittedPerEntry) {
  // ROLLUP (a, a) over one key: (), (a), (a).
  const GroupingSetsShape shape = PlanGroupingSets({{}, {0}, {0}}, true);
  EXPECT_EQ(shape.sets, (Sets{{}, {0}}));
  EXPECT_EQ(shape.entries, (std::vector<std::size_t>{0, 1, 1}));
  EXPECT_EQ(shape.sources, (std::vector<std::size_t>{1, 1}));
  EXPECT_EQ(shape.last_entries, (std::vector<std::size_t>{0, 2}));
}

TEST(GroupingSetsShapeTest, GroupingValuesSetABitPerMissingArgument) {
  const std::vector<plan::GroupingCall> calls = {{.args = {0, 1}}, {.args = {1}}, {.args = {1, 0}}};
  EXPECT_EQ(GroupingValues({0, 1}, calls), (std::vector<std::int64_t>{0, 0, 0}));
  EXPECT_EQ(GroupingValues({0}, calls), (std::vector<std::int64_t>{1, 1, 2}));
  EXPECT_EQ(GroupingValues({}, calls), (std::vector<std::int64_t>{3, 1, 3}));
}

TEST(GroupingSetsShapeTest, OnlyOrderIndependentCallsMergeExactly) {
  const auto call = [](AggKind kind, std::optional<LogicalType> arg, LogicalType type) {
    return plan::AggregateCall{
        .kind = kind,
        .arg = arg.has_value() ? std::optional(Column(0, "x", *arg)) : std::nullopt,
        .type = type};
  };
  const auto key = Column(1, "k", LogicalType::kBigInt);
  for (const plan::AggregateCall& exact : {
           call(AggKind::kCountStar, std::nullopt, LogicalType::kBigInt),
           call(AggKind::kCount, LogicalType::kDouble, LogicalType::kBigInt),
           call(AggKind::kSum, LogicalType::kBigInt, LogicalType::kHugeInt),
           call(AggKind::kSum, LogicalType::Decimal(18, 2), LogicalType::Decimal(38, 2)),
           call(AggKind::kAvg, LogicalType::kInteger, LogicalType::kDouble),
           call(AggKind::kAvg, LogicalType::kDate, LogicalType::kTimestamp),
           call(AggKind::kMin, LogicalType::kVarchar, LogicalType::kVarchar),
           call(AggKind::kMax, LogicalType::Decimal(38, 0), LogicalType::Decimal(38, 0)),
       }) {
    EXPECT_TRUE(MergesExactly({key}, {exact})) << plan::ToString(exact.kind);
  }
  for (const plan::AggregateCall& inexact : {
           call(AggKind::kSum, LogicalType::kDouble, LogicalType::kDouble),
           call(AggKind::kAvg, LogicalType::kDouble, LogicalType::kDouble),
           call(AggKind::kSum, LogicalType::kHugeInt, LogicalType::kHugeInt),
           call(AggKind::kSum, LogicalType::Decimal(38, 2), LogicalType::Decimal(38, 2)),
           call(AggKind::kMin, LogicalType::kDouble, LogicalType::kDouble),
           call(AggKind::kCountDistinct, LogicalType::kBigInt, LogicalType::kBigInt),
           call(AggKind::kStddevSamp, LogicalType::kDouble, LogicalType::kDouble),
       }) {
    EXPECT_FALSE(MergesExactly({key}, {inexact})) << plan::ToString(inexact.kind);
  }
  EXPECT_FALSE(MergesExactly({Column(1, "k", LogicalType::kDouble)}, {}));  // a DOUBLE key
  EXPECT_TRUE(MergesExactly({}, {}));
}

// ---- GroupTable::MergeProjected ----

// Groups 0 .. n - 1 of a table.
std::vector<std::uint32_t> All(std::size_t n) {
  std::vector<std::uint32_t> groups(n);
  std::ranges::iota(groups, 0U);
  return groups;
}

class GroupingSetsTest : public testing::ExecTest {};

// Many groups of the source become one group here; the keys of a new group are the first source
// group's; every call merges; without keys every group folds into the one group. More new groups
// than one chunk holds come out in bounded chunks.
TEST_F(GroupingSetsTest, MergeProjectedFoldsManyGroupsIntoOne) {
  const auto schema =
      arrow::schema({arrow::field("a", arrow::int64()), arrow::field("b", arrow::binary()),
                     arrow::field("v", arrow::int64())});
  const std::vector<plan::AggregateCall> calls = {
      {.kind = AggKind::kCountStar, .arg = {}, .type = LogicalType::kBigInt},
      {.kind = AggKind::kSum,
       .arg = Column(2, "v", LogicalType::kBigInt),
       .type = LogicalType::kHugeInt},
      {.kind = AggKind::kMin,
       .arg = Column(1, "b", LogicalType::kVarchar),
       .type = LogicalType::kVarchar}};
  const auto a = Column(0, "a", LogicalType::kBigInt);
  const auto b = Column(1, "b", LogicalType::kVarchar);
  auto source = GroupTable::Make({a, b}, calls, 3, arrow::default_memory_pool(), nullptr);
  ASSERT_TRUE(source.ok()) << source.status().ToString();
  const auto rows =
      arrow::RecordBatch::Make(schema, 6,
                               {testing::Int64s({1, 2, 1, std::nullopt, 2, 1}),
                                testing::Strings({"x", "y", "z", "x", "y", std::nullopt}),
                                testing::Int64s({10, 20, 30, 40, 50, 60})});
  ASSERT_TRUE((*source)->Consume(*rows).ok());
  ASSERT_EQ((*source)->num_groups(), 5U);  // (1,x) (2,y) (1,z) (NULL,x) (1,NULL)
  const auto uniques = (*source)->uniques();
  ASSERT_TRUE(uniques.ok());

  auto by_a = GroupTable::Make({a}, calls, 3, arrow::default_memory_pool(), nullptr);
  ASSERT_TRUE(by_a.ok());
  const std::vector<std::size_t> first = {0};
  ASSERT_TRUE((*by_a)->MergeProjected(**source, ProjectKeys(*uniques, first), All(5)).ok());
  EXPECT_EQ((*by_a)->num_groups(), 3U);
  const auto chunk = (*by_a)->NextChunk(SetSchema({a, b}, {0}, calls));
  ASSERT_TRUE(chunk.ok() && *chunk != nullptr);
  EXPECT_EQ((*chunk)->column(0)->ToString(), testing::Int64s({1, 2, std::nullopt})->ToString());
  EXPECT_EQ((*chunk)->column(1)->ToString(), testing::Int64s({3, 2, 1})->ToString());
  EXPECT_EQ((*chunk)->column(2)->ToString(),
            testing::Decimals(LogicalType::kHugeInt, {"100", "70", "40"})->ToString());
  EXPECT_EQ((*chunk)->column(3)->ToString(), testing::Strings({"x", "y", "x"})->ToString());

  auto by_b = GroupTable::Make({b}, calls, 3, arrow::default_memory_pool(), nullptr);
  ASSERT_TRUE(by_b.ok());
  const std::vector<std::size_t> second = {1};
  ASSERT_TRUE((*by_b)->MergeProjected(**source, ProjectKeys(*uniques, second), All(5)).ok());
  EXPECT_EQ((*by_b)->num_groups(), 4U);  // x, y, z, NULL

  // A subset of the source's groups, as the parallel operator merges each partition of a set: the
  // groups split by the hash of their projected keys into two tables, which together hold every
  // group once, with every row counted once.
  const auto split = SplitByKeys(ProjectKeys(*uniques, first), 2, arrow::default_memory_pool());
  ASSERT_TRUE(split.ok()) << split.status().ToString();
  int64_t groups = 0;
  int64_t counted = 0;
  for (std::size_t q = 0; q < 2; ++q) {
    auto part = GroupTable::Make({a}, calls, 3, arrow::default_memory_pool(), nullptr);
    ASSERT_TRUE(part.ok());
    ASSERT_TRUE((*part)->MergeProjected(**source, split->keys[q], split->rows[q]).ok());
    groups += (*part)->num_groups();
    const auto rows_of = (*part)->NextChunk(SetSchema({a, b}, {0}, calls));
    ASSERT_TRUE(rows_of.ok());
    if (*rows_of != nullptr) {
      for (const auto& count :
           testing::Int64Column(*arrow::Table::FromRecordBatches({*rows_of}).ValueOrDie(), 1)) {
        counted += count.value_or(0);
      }
    }
  }
  EXPECT_EQ(groups, 3);
  EXPECT_EQ(counted, 6);

  auto none = GroupTable::Make({}, calls, 3, arrow::default_memory_pool(), nullptr);
  ASSERT_TRUE(none.ok());
  ASSERT_TRUE((*none)->MergeProjected(**source, arrow::compute::ExecBatch({}, 5), All(5)).ok());
  EXPECT_EQ((*none)->num_groups(), 1U);
  const auto all = (*none)->NextChunk(SetSchema({a, b}, {}, calls));
  ASSERT_TRUE(all.ok() && *all != nullptr);
  EXPECT_EQ((*all)->column(0)->ToString(), testing::Int64s({6})->ToString());
  EXPECT_EQ((*all)->column(2)->ToString(), testing::Strings({"x"})->ToString());
}

TEST_F(GroupingSetsTest, MergeProjectedAddsManyGroupsInBoundedChunks) {
  const auto schema =
      arrow::schema({arrow::field("a", arrow::int64()), arrow::field("b", arrow::int64())});
  const auto groups = static_cast<int64_t>(GroupTable::kMaxMergeChunk) + 1000;
  std::vector<std::optional<int64_t>> a;
  std::vector<std::optional<int64_t>> b;
  for (int64_t i = 0; i < groups; ++i) {
    a.emplace_back(i);
    b.emplace_back(i % 2);
  }
  const std::vector<plan::AggregateCall> calls = {
      {.kind = AggKind::kCountStar, .arg = {}, .type = LogicalType::kBigInt}};
  const auto ka = Column(0, "a", LogicalType::kBigInt);
  const auto kb = Column(1, "b", LogicalType::kBigInt);
  auto source = GroupTable::Make({ka, kb}, calls, 2, arrow::default_memory_pool(), nullptr);
  ASSERT_TRUE(source.ok());
  ASSERT_TRUE((*source)
                  ->Consume(*arrow::RecordBatch::Make(schema, groups,
                                                      {testing::Int64s(a), testing::Int64s(b)}))
                  .ok());
  const auto uniques = (*source)->uniques();
  ASSERT_TRUE(uniques.ok());
  auto by_a = GroupTable::Make({ka}, calls, 2, arrow::default_memory_pool(), nullptr);
  ASSERT_TRUE(by_a.ok());
  const std::vector<std::size_t> first = {0};
  ASSERT_TRUE((*by_a)
                  ->MergeProjected(**source, ProjectKeys(*uniques, first),
                                   All(static_cast<std::size_t>(groups)))
                  .ok());
  int64_t rows = 0;
  int64_t chunks = 0;
  while (true) {
    const auto chunk = (*by_a)->NextChunk(SetSchema({ka, kb}, {0}, calls));
    ASSERT_TRUE(chunk.ok());
    if (*chunk == nullptr) {
      break;
    }
    EXPECT_LE((*chunk)->num_rows(), static_cast<int64_t>(GroupTable::kMaxMergeChunk));
    rows += (*chunk)->num_rows();
    ++chunks;
  }
  EXPECT_EQ(rows, groups);
  EXPECT_EQ(chunks, 2);
}

// ---- the operators ----

// A table of `parts` parts of 50 rows (every fourth part empty): a BIGINT (row % 7, NULL every 11th
// row), s VARCHAR ('a' + (row % 5), NULL every 13th), d DATE (row % 3), m DECIMAL(10,2) (row
// * 1.25), x DOUBLE (row * 0.1, rounding by order when summed), v BIGINT (row).
std::shared_ptr<MemoryTable> Table(bool split, int64_t parts = 12) {
  const auto schema =
      arrow::schema({arrow::field("a", arrow::int64()), arrow::field("s", arrow::binary()),
                     arrow::field("d", arrow::date32()),
                     arrow::field("m", plan::ToArrow(LogicalType::Decimal(10, 2))),
                     arrow::field("x", arrow::float64()), arrow::field("v", arrow::int64())});
  arrow::RecordBatchVector batches;
  for (int64_t part = 0; part < parts; ++part) {
    const int64_t rows = part % 4 == 3 ? 0 : 50;
    std::vector<std::optional<int64_t>> a;
    std::vector<std::optional<std::string>> s;
    std::vector<std::optional<int32_t>> d;
    std::vector<std::optional<std::string>> m;
    arrow::DoubleBuilder x;
    std::vector<std::optional<int64_t>> v;
    for (int64_t i = 0; i < rows; ++i) {
      const int64_t row = (part * 50) + i;
      a.push_back(row % 11 == 0 ? std::nullopt : std::optional<int64_t>(row % 7));
      s.push_back(row % 13 == 0
                      ? std::nullopt
                      : std::optional(std::string(1, static_cast<char>('a' + (row % 5)))));
      d.emplace_back(static_cast<int32_t>(row % 3));
      m.emplace_back(std::to_string(row * 125));
      EXPECT_TRUE(x.Append(static_cast<double>(row) * 0.1).ok());
      v.emplace_back(row);
    }
    batches.push_back(
        arrow::RecordBatch::Make(schema, rows,
                                 {testing::Int64s(a), testing::Strings(s), testing::Dates(d),
                                  testing::Decimals(LogicalType::Decimal(10, 2), m),
                                  x.Finish().ValueOrDie(), testing::Int64s(v)}));
  }
  if (!split) {
    auto combined = arrow::Table::FromRecordBatches(schema, batches).ValueOrDie();
    return std::make_shared<MemoryTable>(
        schema, arrow::RecordBatchVector{combined->CombineChunksToBatch().ValueOrDie()}, false);
  }
  return std::make_shared<MemoryTable>(schema, std::move(batches), true);
}

plan::BoundColumn ColA() { return Column(0, "a", LogicalType::kBigInt); }
plan::BoundColumn ColS() { return Column(1, "s", LogicalType::kVarchar); }
plan::BoundColumn ColD() { return Column(2, "d", LogicalType::kDate); }
plan::BoundColumn ColM() { return Column(3, "m", LogicalType::Decimal(10, 2)); }
plan::BoundColumn ColX() { return Column(4, "x", LogicalType::kDouble); }
plan::BoundColumn ColV() { return Column(5, "v", LogicalType::kBigInt); }

// Calls whose partial states merge exactly.
std::vector<plan::AggregateCall> ExactCalls() {
  return {{.kind = AggKind::kCountStar, .arg = {}, .type = LogicalType::kBigInt},
          {.kind = AggKind::kCount, .arg = ColA(), .type = LogicalType::kBigInt},
          {.kind = AggKind::kSum, .arg = ColV(), .type = LogicalType::kHugeInt},
          {.kind = AggKind::kSum, .arg = ColM(), .type = LogicalType::Decimal(38, 2)},
          {.kind = AggKind::kAvg, .arg = ColM(), .type = LogicalType::kDouble},
          {.kind = AggKind::kAvg, .arg = ColD(), .type = LogicalType::kTimestamp},
          {.kind = AggKind::kMin, .arg = ColS(), .type = LogicalType::kVarchar},
          {.kind = AggKind::kMax, .arg = ColM(), .type = LogicalType::Decimal(10, 2)}};
}

// Calls that do not: each set is then aggregated from the input.
std::vector<plan::AggregateCall> InexactCalls() {
  return {{.kind = AggKind::kSum, .arg = ColX(), .type = LogicalType::kDouble},
          {.kind = AggKind::kCountDistinct, .arg = ColV(), .type = LogicalType::kBigInt},
          {.kind = AggKind::kStddevSamp, .arg = ColX(), .type = LogicalType::kDouble},
          {.kind = AggKind::kCountStar, .arg = {}, .type = LogicalType::kBigInt}};
}

plan::LogicalNodePtr Node(plan::LogicalNode node) {
  return std::make_shared<const plan::LogicalNode>(std::move(node));
}

plan::LogicalNodePtr Scan(const std::shared_ptr<MemoryTable>& table) {
  return Node(plan::ScanNode{.table = table, .table_name = "t", .fields = {0, 1, 2, 3, 4, 5}});
}

// The columns of the aggregations these tests run.
std::size_t OutputWidth(const plan::LogicalNode& node) {
  if (const auto* sets = std::get_if<plan::GroupingSetsNode>(&node)) {
    return sets->keys.size() + sets->aggregates.size() + sets->grouping.size();
  }
  if (const auto* group = std::get_if<plan::GroupAggregateNode>(&node)) {
    return group->keys.size() + group->aggregates.size();
  }
  return std::get<plan::AggregateNode>(node).aggregates.size();
}

arrow::Result<std::shared_ptr<arrow::Table>> RunPlan(const plan::LogicalNodePtr& root,
                                                     arrow::internal::Executor* executor,
                                                     int64_t batch_size = 16) {
  plan::LogicalPlan plan{.root = root, .output = {}};
  plan.output.assign(OutputWidth(*root),
                     plan::OutputColumn{.name = "c", .type = LogicalType::kBigInt});
  ARROW_ASSIGN_OR_RAISE(auto op, BuildPhysicalPlan(plan));
  ExecContext ctx{.pool = arrow::default_memory_pool(),
                  .batch_size = batch_size,
                  .executor = executor,
                  .threads = executor == nullptr ? 1 : kThreads};
  return Drain(*op, ctx);
}

// Every row as its cells' text, sorted: results whose row order is open compare as multisets.
std::vector<std::string> RowsOf(const arrow::Table& table) {
  std::vector<std::string> rows;
  for (int64_t r = 0; r < table.num_rows(); ++r) {
    std::string row;
    for (int c = 0; c < table.num_columns(); ++c) {
      row += table.column(c)->GetScalar(r).ValueOrDie()->ToString();
      row += '|';
    }
    rows.push_back(std::move(row));
  }
  std::ranges::sort(rows);
  return rows;
}

// What the node should give: each set's own GROUP BY (or, for the empty set, the global
// aggregation), its rows padded with NULL for the keys the set lacks and followed by the GROUPING
// values, as `aggregate` answers them over the same input.
using Aggregate = std::function<arrow::Result<std::shared_ptr<arrow::Table>>(
    const std::vector<plan::BoundColumn>& keys, const std::vector<plan::AggregateCall>& calls)>;

std::vector<std::string> Expected(const std::vector<plan::BoundColumn>& keys, const Sets& sets,
                                  const std::vector<plan::AggregateCall>& calls,
                                  const std::vector<plan::GroupingCall>& grouping,
                                  const Aggregate& aggregate) {
  std::vector<std::string> rows;
  for (const std::vector<std::size_t>& set : sets) {
    const auto table = aggregate(SetKeys(keys, set), calls);
    EXPECT_TRUE(table.ok()) << table.status().ToString();
    if (!table.ok()) {
      return {};
    }
    const std::vector<std::int64_t> values = GroupingValues(set, grouping);
    for (int64_t r = 0; r < (*table)->num_rows(); ++r) {
      std::string row;
      std::size_t next = 0;
      for (std::size_t k = 0; k < keys.size(); ++k) {
        if (next < set.size() && set[next] == k) {
          row += (*table)->column(static_cast<int>(next++))->GetScalar(r).ValueOrDie()->ToString();
        } else {
          row += "null";
        }
        row += '|';
      }
      for (std::size_t c = 0; c < calls.size(); ++c) {
        row += (*table)
                   ->column(static_cast<int>(set.size() + c))
                   ->GetScalar(r)
                   .ValueOrDie()
                   ->ToString();
        row += '|';
      }
      for (const std::int64_t value : values) {
        row += std::to_string(value) + '|';
      }
      rows.push_back(std::move(row));
    }
  }
  std::ranges::sort(rows);
  return rows;
}

// The plan's per-set reference over a table: GroupAggregate, or Aggregate for no keys.
Aggregate PlannedAggregate(const std::shared_ptr<MemoryTable>& table,
                           arrow::internal::Executor* executor) {
  return [table, executor](const std::vector<plan::BoundColumn>& keys,
                           const std::vector<plan::AggregateCall>& calls) {
    if (keys.empty()) {
      return RunPlan(Node(plan::AggregateNode{.input = Scan(table), .aggregates = calls}),
                     executor);
    }
    return RunPlan(
        Node(plan::GroupAggregateNode{.input = Scan(table), .keys = keys, .aggregates = calls}),
        executor);
  };
}

// The rows of a table as a source operator hands them on, in batches of 16 rows.
std::vector<Batch> BatchesOf(const MemoryTable& table) {
  std::vector<Batch> batches;
  auto read = table.Scan({0, 1, 2, 3, 4, 5}, 16);
  EXPECT_TRUE(read.ok()) << read.status().ToString();
  while (read.ok()) {
    std::shared_ptr<arrow::RecordBatch> batch;
    EXPECT_TRUE((*read)->ReadNext(&batch).ok());
    if (batch == nullptr) {
      break;
    }
    batches.push_back(Batch{.data = std::move(batch), .selection = {}});
  }
  return batches;
}

// The serial operators' per-set reference over a table's batches.
Aggregate SerialAggregate(const std::shared_ptr<MemoryTable>& table) {
  return [table](const std::vector<plan::BoundColumn>& keys,
                 const std::vector<plan::AggregateCall>& calls)
             -> arrow::Result<std::shared_ptr<arrow::Table>> {
    auto source = std::make_unique<testing::ScriptedSource>(table->schema(), BatchesOf(*table));
    ExecContext ctx;
    if (keys.empty()) {
      ScalarAggregateOperator op(std::move(source), calls);
      return Drain(op, ctx);
    }
    GroupAggregateOperator op(std::move(source), keys, calls);
    return Drain(op, ctx);
  };
}

arrow::Result<std::shared_ptr<arrow::Table>> RunSerial(
    const std::shared_ptr<MemoryTable>& table, const std::vector<plan::BoundColumn>& keys,
    const Sets& sets, const std::vector<plan::AggregateCall>& calls,
    const std::vector<plan::GroupingCall>& grouping, bool per_set = false) {
  GroupingSetsOperator op(
      std::make_unique<testing::ScriptedSource>(table->schema(), BatchesOf(*table)), keys, sets,
      calls, grouping, per_set);
  ExecContext ctx;
  return Drain(op, ctx);
}

plan::LogicalNodePtr GroupingSets(const plan::LogicalNodePtr& input,
                                  std::vector<plan::BoundColumn> keys, Sets sets,
                                  std::vector<plan::AggregateCall> calls,
                                  std::vector<plan::GroupingCall> grouping = {}) {
  return Node(plan::GroupingSetsNode{.input = input,
                                     .keys = std::move(keys),
                                     .key_ids = {},
                                     .sets = std::move(sets),
                                     .aggregates = std::move(calls),
                                     .grouping = std::move(grouping)});
}

struct Case {
  std::string name;
  std::vector<plan::BoundColumn> keys;
  Sets sets;
  std::vector<plan::AggregateCall> calls;
  std::vector<plan::GroupingCall> grouping;
};

std::vector<Case> Cases() {
  const std::vector<plan::GroupingCall> grouping = {{.args = {0, 1, 2}}, {.args = {1}}};
  return {
      {.name = "rollup",
       .keys = {ColA(), ColS(), ColD()},
       .sets = {{0, 1, 2}, {0, 1}, {0}, {}},
       .calls = ExactCalls(),
       .grouping = grouping},
      {.name = "cube",
       .keys = {ColA(), ColS(), ColD()},
       .sets = {{0, 1, 2}, {0, 1}, {0, 2}, {0}, {1, 2}, {1}, {2}, {}},
       .calls = ExactCalls(),
       .grouping = grouping},
      {.name = "duplicates", .keys = {ColA()}, .sets = {{}, {0}, {0}}, .calls = ExactCalls()},
      {.name = "two roots",
       .keys = {ColA(), ColS(), ColD()},
       .sets = {{0, 1}, {1, 2}, {1}, {}},
       .calls = ExactCalls(),
       .grouping = grouping},
      {.name = "many groups, a later key alone",
       .keys = {ColV(), ColA()},
       .sets = {{0, 1}, {1}, {0}},
       .calls = ExactCalls()},
      {.name = "inexact calls",
       .keys = {ColA(), ColS()},
       .sets = {{0, 1}, {0}, {}},
       .calls = InexactCalls()},
      {.name = "DOUBLE key",
       .keys = {ColX(), ColA()},
       .sets = {{0, 1}, {1}, {}},
       .calls = {{.kind = AggKind::kCountStar, .arg = {}, .type = LogicalType::kBigInt}}},
  };
}

// The serial operator gives every set's own GROUP BY, padded, in two levels or per set alike.
TEST_F(GroupingSetsTest, SerialRowsAreEachSetsGroupBy) {
  const auto table = Table(/*split=*/true);
  for (const Case& c : Cases()) {
    const auto expected = Expected(c.keys, c.sets, c.calls, c.grouping, SerialAggregate(table));
    for (const bool per_set : {false, true}) {
      const auto result = RunSerial(table, c.keys, c.sets, c.calls, c.grouping, per_set);
      ASSERT_TRUE(result.ok()) << c.name << ": " << result.status().ToString();
      EXPECT_EQ(RowsOf(**result), expected) << c.name << (per_set ? " per set" : "");
      EXPECT_EQ((*result)->num_columns(),
                static_cast<int>(c.keys.size() + c.calls.size() + c.grouping.size()));
    }
  }
}

// The parallel operator gives every set's own partitioned GROUP BY, padded; the same rows in the
// same order on one thread and on four; and, where the calls merge exactly, the serial rows.
TEST_F(GroupingSetsTest, ParallelRowsAreEachSetsGroupByOnAnyThreads) {
  const auto pool = Pool();
  for (const Case& c : Cases()) {
    const auto table = Table(/*split=*/true);
    const auto expected =
        Expected(c.keys, c.sets, c.calls, c.grouping, PlannedAggregate(table, nullptr));
    const auto node = GroupingSets(Scan(table), c.keys, c.sets, c.calls, c.grouping);
    const auto one = RunPlan(node, nullptr);
    ASSERT_TRUE(one.ok()) << c.name << ": " << one.status().ToString();
    EXPECT_EQ(RowsOf(**one), expected) << c.name;
    const auto four = RunPlan(node, pool.get());
    ASSERT_TRUE(four.ok()) << c.name << ": " << four.status().ToString();
    EXPECT_TRUE((*four)->Equals(**one)) << c.name;
    if (MergesExactly(c.keys, c.calls)) {
      const auto serial = RunSerial(table, c.keys, c.sets, c.calls, c.grouping);
      ASSERT_TRUE(serial.ok());
      EXPECT_EQ(RowsOf(**serial), expected) << c.name;
    }
  }
}

// The rows of each set come together, in the order of the list; a duplicate set's again.
TEST_F(GroupingSetsTest, SetsComeInListOrder) {
  const auto pool = Pool();
  const auto table = Table(/*split=*/true);
  const std::vector<plan::GroupingCall> grouping = {{.args = {0, 1}}};
  const auto node =
      GroupingSets(Scan(table), {ColA(), ColS()}, {{0}, {}, {0, 1}, {0}}, ExactCalls(), grouping);
  for (arrow::internal::Executor* executor :
       {static_cast<arrow::internal::Executor*>(nullptr),
        static_cast<arrow::internal::Executor*>(pool.get())}) {
    const auto result = RunPlan(node, executor);
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    const auto values = testing::Int64Column(**result, (*result)->num_columns() - 1);
    std::vector<std::optional<int64_t>> runs;  // the GROUPING value of each run of rows
    for (const auto& value : values) {
      if (runs.empty() || runs.back() != value) {
        runs.push_back(value);
      }
    }
    EXPECT_EQ(runs, (std::vector<std::optional<int64_t>>{1, 3, 0, 1}));
    // 8 values of a (NULL included) in each (a) set, one () row.
    EXPECT_EQ(std::ranges::count(values, std::optional<int64_t>(1)), 16);
    EXPECT_EQ(std::ranges::count(values, std::optional<int64_t>(3)), 1);
  }
}

// Over no input rows the empty set still has its row (COUNT 0, SUM NULL), and a set of keys none.
TEST_F(GroupingSetsTest, TheEmptySetHasARowOverNoInput) {
  const auto pool = Pool();
  const auto schema = Table(false, 0)->schema();
  for (const bool split : {false, true}) {
    const auto empty = std::make_shared<MemoryTable>(schema, arrow::RecordBatchVector{}, split);
    const std::vector<plan::AggregateCall> calls = {
        {.kind = AggKind::kCountStar, .arg = {}, .type = LogicalType::kBigInt},
        {.kind = AggKind::kSum, .arg = ColV(), .type = LogicalType::kHugeInt}};
    const std::vector<plan::GroupingCall> grouping = {{.args = {0}}};
    for (const bool per_set : {false, true}) {
      const auto serial = RunSerial(empty, {ColA()}, {{0}, {}, {}}, calls, grouping, per_set);
      ASSERT_TRUE(serial.ok()) << serial.status().ToString();
      EXPECT_EQ(RowsOf(**serial), (std::vector<std::string>{"null|0|null|1|", "null|0|null|1|"}));
    }
    for (arrow::internal::Executor* executor :
         {static_cast<arrow::internal::Executor*>(nullptr),
          static_cast<arrow::internal::Executor*>(pool.get())}) {
      const auto result =
          RunPlan(GroupingSets(Scan(empty), {ColA()}, {{0}, {}, {}}, calls, grouping), executor);
      ASSERT_TRUE(result.ok()) << result.status().ToString();
      EXPECT_EQ(RowsOf(**result), (std::vector<std::string>{"null|0|null|1|", "null|0|null|1|"}));
    }
  }
}

// A finalize error of a later set (the variance of the whole input overflows; one value per
// group has none) fails the first Next(), before any row of an earlier set.
TEST_F(GroupingSetsTest, EverySetsFinalizeErrorsComeFirst) {
  const auto pool = Pool();
  const auto schema = Table(false, 0)->schema();
  arrow::DoubleBuilder x;
  ASSERT_TRUE(x.AppendValues({1e308, -1e308}).ok());
  const auto batch = arrow::RecordBatch::Make(
      schema, 2,
      {testing::Int64s({1, 2}), testing::Strings({"a", "b"}), testing::Dates({0, 0}),
       testing::Decimals(LogicalType::Decimal(10, 2), {"1", "2"}), x.Finish().ValueOrDie(),
       testing::Int64s({1, 2})});
  const std::vector<plan::AggregateCall> calls = {
      {.kind = AggKind::kStddevSamp, .arg = ColX(), .type = LogicalType::kDouble}};
  for (const bool split : {false, true}) {
    const auto table =
        std::make_shared<MemoryTable>(schema, arrow::RecordBatchVector{batch}, split);
    const auto serial = RunSerial(table, {ColA()}, {{0}, {}}, calls, {});
    EXPECT_FALSE(serial.ok());
    EXPECT_TRUE(serial.status().message().contains("out of range")) << serial.status().ToString();
    for (arrow::internal::Executor* executor :
         {static_cast<arrow::internal::Executor*>(nullptr),
          static_cast<arrow::internal::Executor*>(pool.get())}) {
      plan::LogicalPlan plan{.root = GroupingSets(Scan(table), {ColA()}, {{0}, {}}, calls),
                             .output = {}};
      plan.output.assign(2, plan::OutputColumn{.name = "c", .type = LogicalType::kBigInt});
      auto op = BuildPhysicalPlan(plan);
      ASSERT_TRUE(op.ok()) << op.status().ToString();
      ExecContext ctx{.executor = executor, .threads = executor == nullptr ? 1 : kThreads};
      ASSERT_TRUE((*op)->Open(ctx).ok());
      const auto first = (*op)->Next();
      EXPECT_FALSE(first.ok());
      EXPECT_TRUE(first.status().message().contains("out of range")) << first.status().ToString();
      ASSERT_TRUE((*op)->Close().ok());
    }
  }
}

// Over a part pipeline the planner groups per part (PartGroupingSetsOperator); over any other input
// (here a union) serially (GroupingSetsOperator). Both give each set's GROUP BY.
TEST_F(GroupingSetsTest, ThePlannerPicksThePartOperatorOverAPipeline) {
  const auto table = Table(/*split=*/true);
  const Case rollup = Cases().front();
  const auto build = [&](const plan::LogicalNodePtr& input) {
    plan::LogicalPlan plan{
        .root = GroupingSets(input, rollup.keys, rollup.sets, rollup.calls, rollup.grouping),
        .output = {}};
    plan.output.assign(OutputWidth(*plan.root),
                       plan::OutputColumn{.name = "c", .type = LogicalType::kBigInt});
    return BuildPhysicalPlan(plan);
  };
  const auto parallel = build(Scan(table));
  ASSERT_TRUE(parallel.ok()) << parallel.status().ToString();
  EXPECT_NE(dynamic_cast<const PartGroupingSetsOperator*>(parallel->get()), nullptr);
  std::vector<plan::BoundColumn> columns = {ColA(), ColS(), ColD(), ColM(), ColX(), ColV()};
  const auto unioned = Node(plan::UnionAllNode{.inputs = {Scan(table)}, .columns = {columns}});
  const auto serial = build(unioned);
  ASSERT_TRUE(serial.ok()) << serial.status().ToString();
  ASSERT_NE(dynamic_cast<const GroupingSetsOperator*>(serial->get()), nullptr);
  ExecContext ctx;
  const auto rows = Drain(**serial, ctx);
  ASSERT_TRUE(rows.ok()) << rows.status().ToString();
  EXPECT_EQ(RowsOf(**rows), Expected(rollup.keys, rollup.sets, rollup.calls, rollup.grouping,
                                     SerialAggregate(table)));
}

// Each other set takes one of the second level's two ways, from the data alone: (v) is about as
// fine as its root (v, a), so it is merged once, straight from the root's groups; (a) has a few
// values, so each partition reduces its groups first; () always does. The rows are each set's
// GROUP BY either way, the same on any number of threads.
TEST_F(GroupingSetsTest, EachSetIsMergedOnceOrReducedFirst) {
  const auto pool = Pool();
  const auto table = Table(/*split=*/true, 40);
  const std::vector<plan::BoundColumn> keys = {ColV(), ColA()};
  const Sets sets = {{0, 1}, {0}, {1}, {}};
  const auto expected = Expected(keys, sets, ExactCalls(), {}, PlannedAggregate(table, nullptr));
  for (arrow::internal::Executor* executor :
       {static_cast<arrow::internal::Executor*>(nullptr),
        static_cast<arrow::internal::Executor*>(pool.get())}) {
    plan::LogicalPlan plan{.root = GroupingSets(Scan(table), keys, sets, ExactCalls()),
                           .output = {}};
    plan.output.assign(OutputWidth(*plan.root),
                       plan::OutputColumn{.name = "c", .type = LogicalType::kBigInt});
    ProfileNode root;
    auto op = BuildPhysicalPlan(plan, &root);
    ASSERT_TRUE(op.ok()) << op.status().ToString();
    ExecContext ctx{.executor = executor, .threads = executor == nullptr ? 1 : kThreads};
    const auto result = Drain(**op, ctx);
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    EXPECT_EQ(RowsOf(**result), expected);
    EXPECT_EQ(root.name(), "PartGroupingSets");
    std::optional<int64_t> direct;
    for (const ProfileMetric& metric : root.metrics()) {
      if (metric.name == "direct_sets") {
        direct = metric.value;
      }
    }
    EXPECT_EQ(direct, std::optional<int64_t>(1));
  }
}

// A part that fails fails the query with its own error, the first in part order, on any number of
// threads; a Next() after it fails too, rather than read tables the failure left unmade.
TEST_F(GroupingSetsTest, APartsFailureFailsTheQuery) {
  const auto pool = Pool();
  const Case rollup = Cases().front();
  for (arrow::internal::Executor* executor :
       {static_cast<arrow::internal::Executor*>(nullptr),
        static_cast<arrow::internal::Executor*>(pool.get())}) {
    const auto table = Table(/*split=*/true, 40);
    table->FailPart(5);
    table->FailPart(30);
    plan::LogicalPlan plan{
        .root = GroupingSets(Scan(table), rollup.keys, rollup.sets, rollup.calls, rollup.grouping),
        .output = {}};
    plan.output.assign(OutputWidth(*plan.root),
                       plan::OutputColumn{.name = "c", .type = LogicalType::kBigInt});
    auto op = BuildPhysicalPlan(plan);
    ASSERT_TRUE(op.ok()) << op.status().ToString();
    ExecContext ctx{.executor = executor, .threads = executor == nullptr ? 1 : kThreads};
    ASSERT_TRUE((*op)->Open(ctx).ok());
    const auto first = (*op)->Next();
    EXPECT_TRUE(first.status().IsIOError()) << first.status().ToString();
    EXPECT_EQ(first.status().message(), "part 5 is broken");
    EXPECT_FALSE((*op)->Next().ok());
    EXPECT_TRUE((*op)->Close().ok());
  }
}

// The tables are charged to the query's budget: a limit far below them fails, and cleanly.
TEST_F(GroupingSetsTest, TablesAreChargedToTheBudget) {
  const auto pool = Pool();
  const auto table = Table(/*split=*/true, 40);
  for (arrow::internal::Executor* executor :
       {static_cast<arrow::internal::Executor*>(nullptr),
        static_cast<arrow::internal::Executor*>(pool.get())}) {
    MemoryBudget budget(16 * 1024, arrow::default_memory_pool());
    plan::LogicalPlan plan{
        .root = GroupingSets(Scan(table), {ColV(), ColA()}, {{0, 1}, {1}, {0}}, ExactCalls()),
        .output = {}};
    plan.output.assign(2 + ExactCalls().size(),
                       plan::OutputColumn{.name = "c", .type = LogicalType::kBigInt});
    auto op = BuildPhysicalPlan(plan);
    ASSERT_TRUE(op.ok()) << op.status().ToString();
    ExecContext ctx{.pool = &budget,
                    .executor = executor,
                    .threads = executor == nullptr ? 1 : kThreads,
                    .budget = &budget};
    const auto result = Drain(**op, ctx);
    EXPECT_TRUE(result.status().IsOutOfMemory()) << result.status().ToString();
  }
}

}  // namespace
}  // namespace antb1::exec
