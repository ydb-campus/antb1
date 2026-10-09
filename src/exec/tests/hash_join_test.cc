// The hash joins (docs/adr/0022-joins-and-query-blocks.md, "Execution"): the build (JoinBuild)
// from a part pipeline or a drained input, PrepareBuilds, the operator that prepares a probe
// pipeline's builds (BuildsFirstOperator) over every part sink, and the probe (HashJoinOperator):
// an inner join on its 1:1 and 1:N paths, with residuals, NULL and multi-column keys, empty
// builds, nested builds and chains, errors in the serial order, memory and profiles; semi, anti,
// null-aware anti and one-row joins, their NULL keys, empty builds, one-row windows and values,
// chains and profiles; on one thread and on a 4-thread pool (HashJoinTest). Then joins planned by
// the physical planner against a nested-loop reference, through every sink and the hidden physical
// rules (HashJoinPlanTest).

#include "../hash_join.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/util/thread_pool.h>
#include <gtest/gtest.h>

#include "antb1/exec/filter.h"
#include "antb1/exec/group_aggregate.h"
#include "antb1/exec/join_table.h"
#include "antb1/exec/memory_budget.h"
#include "antb1/exec/operator.h"
#include "antb1/exec/physical_planner.h"
#include "antb1/exec/profile.h"
#include "antb1/exec/scalar_aggregate.h"
#include "antb1/exec/table_scan.h"
#include "antb1/plan/explain.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/table.h"

#include "../part_operators.h"
#include "exec_test_util.h"

namespace antb1::exec {
namespace {

using plan::BuildSide;
using plan::LogicalType;
using testing::Bools;
using testing::Column;
using testing::Dates;
using testing::Int32s;
using testing::Int64Column;
using testing::Int64s;
using testing::MemoryTable;
using testing::ScriptedSource;
using testing::Strings;
using testing::WorkerFailingPool;

constexpr int kThreads = 4;

class HashJoinTest : public testing::ExecTest {};

std::shared_ptr<arrow::internal::ThreadPool> MakeThreadPool() {
  auto pool = arrow::internal::ThreadPool::Make(kThreads);
  EXPECT_TRUE(pool.ok()) << pool.status().ToString();
  return *pool;
}

// No executor (the calling thread), then the pool.
std::vector<arrow::internal::Executor*> Executors(arrow::internal::ThreadPool* pool) {
  return {nullptr, pool};
}

ExecContext ContextOf(arrow::internal::Executor* executor, int64_t batch_size = 3,
                      MemoryBudget* budget = nullptr) {
  return ExecContext{.pool = budget != nullptr ? static_cast<arrow::MemoryPool*>(budget)
                                               : arrow::default_memory_pool(),
                     .batch_size = batch_size,
                     .executor = executor,
                     .threads = executor == nullptr ? 1 : kThreads,
                     .budget = budget};
}

std::shared_ptr<arrow::RecordBatch> BatchOf(const std::shared_ptr<arrow::Schema>& schema,
                                            arrow::ArrayVector columns) {
  const int64_t rows = columns.front()->length();
  return arrow::RecordBatch::Make(schema, rows, std::move(columns));
}

std::shared_ptr<arrow::Schema> Int64Schema(const std::vector<std::string>& names) {
  arrow::FieldVector fields;
  fields.reserve(names.size());
  for (const std::string& name : names) {
    fields.push_back(arrow::field(name, arrow::int64()));
  }
  return arrow::schema(fields);
}

// ---- rows as text, and the nested-loop reference ----

// A table's rows, each cell as text: "null" for NULL, a VARCHAR in quotes, anything else as Arrow
// prints its scalar.
using Rows = std::vector<std::vector<std::string>>;

std::string CellOf(const arrow::ChunkedArray& column, int64_t row) {
  auto scalar = column.GetScalar(row);
  EXPECT_TRUE(scalar.ok()) << scalar.status().ToString();
  if (!(*scalar)->is_valid) {
    return "null";
  }
  if ((*scalar)->type->id() == arrow::Type::BINARY) {
    return "'" + static_cast<const arrow::BinaryScalar&>(**scalar).value->ToString() + "'";
  }
  return (*scalar)->ToString();
}

Rows RowsOf(const arrow::Table& table) {
  Rows rows(static_cast<std::size_t>(table.num_rows()));
  for (int c = 0; c < table.num_columns(); ++c) {
    for (int64_t r = 0; r < table.num_rows(); ++r) {
      rows[static_cast<std::size_t>(r)].push_back(CellOf(*table.column(c), r));
    }
  }
  return rows;
}

Rows RowsOf(const std::shared_ptr<arrow::RecordBatch>& batch) {
  auto table = arrow::Table::FromRecordBatches({batch});
  EXPECT_TRUE(table.ok()) << table.status().ToString();
  return RowsOf(**table);
}

// The selected rows of `batches`.
Rows RowsOf(const std::shared_ptr<arrow::Schema>& schema, const std::vector<Batch>& batches) {
  arrow::RecordBatchVector selected;
  for (const Batch& batch : batches) {
    auto rows = Materialize(batch, arrow::default_memory_pool());
    EXPECT_TRUE(rows.ok()) << rows.status().ToString();
    selected.push_back(*rows);
  }
  auto table = arrow::Table::FromRecordBatches(schema, selected);
  EXPECT_TRUE(table.ok()) << table.status().ToString();
  return RowsOf(**table);
}

// The rows of every part of `table`, in part order.
Rows RowsOf(const plan::Table& table) {
  std::vector<int> fields(static_cast<std::size_t>(table.schema()->num_fields()));
  std::ranges::iota(fields, 0);
  auto reader = table.Scan(fields, 1024, arrow::default_memory_pool());
  EXPECT_TRUE(reader.ok()) << reader.status().ToString();
  auto read = (*reader)->ToTable();
  EXPECT_TRUE(read.ok()) << read.status().ToString();
  return RowsOf(**read);
}

// Whether the key cells of probe row `p` equal those of build row `b` (NULL never matching).
bool KeysMatch(const std::vector<std::string>& p, const std::vector<std::string>& b,
               const std::vector<int>& probe_keys, const std::vector<int>& build_keys) {
  for (std::size_t k = 0; k < probe_keys.size(); ++k) {
    const std::string& x = p[static_cast<std::size_t>(probe_keys[k])];
    const std::string& y = b[static_cast<std::size_t>(build_keys[k])];
    if (x == "null" || y == "null" || x != y) {
      return false;
    }
  }
  return true;
}

// Whether some key cell of `row` is NULL.
bool NullKey(const std::vector<std::string>& row, const std::vector<int>& keys) {
  return std::ranges::any_of(
      keys, [&row](int key) { return row[static_cast<std::size_t>(key)] == "null"; });
}

// The inner join of `probe` and `build` on these key columns (equal cells, NULL never matching):
// every probe row in order, each with its matches in build order; a row is the left input's cells,
// then the right input's.
Rows ReferenceJoin(const Rows& probe, const Rows& build, const std::vector<int>& probe_keys,
                   const std::vector<int>& build_keys, BuildSide build_side = BuildSide::kRight) {
  Rows out;
  for (const std::vector<std::string>& p : probe) {
    for (const std::vector<std::string>& b : build) {
      if (!KeysMatch(p, b, probe_keys, build_keys)) {
        continue;
      }
      std::vector<std::string> row = build_side == BuildSide::kLeft ? b : p;
      const std::vector<std::string>& right = build_side == BuildSide::kLeft ? p : b;
      row.insert(row.end(), right.begin(), right.end());
      out.push_back(std::move(row));
    }
  }
  return out;
}

// The rows of `probe` that a semi, anti or null-aware anti join with `build` on these key columns
// keeps, in order: semi those with a match (once), anti those without one; null-aware anti every
// row when `build` is empty, none when a build row has a NULL key, else those without a match and
// without a NULL key.
Rows ReferenceFilter(plan::JoinKind kind, const Rows& probe, const Rows& build,
                     const std::vector<int>& probe_keys, const std::vector<int>& build_keys) {
  const bool null_in_build = std::ranges::any_of(
      build, [&](const std::vector<std::string>& b) { return NullKey(b, build_keys); });
  Rows out;
  for (const std::vector<std::string>& p : probe) {
    const bool matched = std::ranges::any_of(build, [&](const std::vector<std::string>& b) {
      return KeysMatch(p, b, probe_keys, build_keys);
    });
    bool keep = false;
    if (kind == plan::JoinKind::kSemi) {
      keep = matched;
    } else if (kind == plan::JoinKind::kAnti) {
      keep = !matched;
    } else {
      keep = build.empty() || (!null_in_build && !matched && !NullKey(p, probe_keys));
    }
    if (keep) {
      out.push_back(p);
    }
  }
  return out;
}

// Every row of `probe` followed by the cells of `row` (the build row of a one-row join).
Rows ReferenceOneRow(const Rows& probe, const std::vector<std::string>& row) {
  Rows out = probe;
  for (std::vector<std::string>& p : out) {
    p.insert(p.end(), row.begin(), row.end());
  }
  return out;
}

// The rows of `rows` whose cell `column` is "true".
Rows WhereTrue(const Rows& rows, std::size_t column) {
  Rows out;
  for (const std::vector<std::string>& row : rows) {
    if (row[column] == "true") {
      out.push_back(row);
    }
  }
  return out;
}

// ---- tables, builds and probes ----

// A table of `parts` parts (a batch each, of `rows` rows) with the BIGINT columns <prefix>k
// (key(i)) and <prefix>id (i), where i numbers the rows in part order.
std::shared_ptr<MemoryTable> KeyTable(const std::string& prefix, int64_t parts, int64_t rows,
                                      const std::function<std::optional<int64_t>(int64_t)>& key) {
  const auto schema = Int64Schema({prefix + "k", prefix + "id"});
  arrow::RecordBatchVector batches;
  for (int64_t part = 0; part < parts; ++part) {
    std::vector<std::optional<int64_t>> keys;
    std::vector<std::optional<int64_t>> ids;
    for (int64_t r = 0; r < rows; ++r) {
      const int64_t i = (part * rows) + r;
      keys.push_back(key(i));
      ids.emplace_back(i);
    }
    batches.push_back(BatchOf(schema, {Int64s(keys), Int64s(ids)}));
  }
  return std::make_shared<MemoryTable>(schema, std::move(batches), /*split=*/true);
}

std::shared_ptr<const JoinBuildSpec> SpecOf(std::shared_ptr<arrow::Schema> schema,
                                            std::vector<plan::BoundColumn> keys) {
  auto spec = JoinBuildSpec::Make(std::move(schema), std::move(keys));
  EXPECT_TRUE(spec.ok()) << spec.status().ToString();
  return *spec;
}

std::vector<int> AllFields(const plan::Table& table) {
  std::vector<int> fields(static_cast<std::size_t>(table.schema()->num_fields()));
  std::ranges::iota(fields, 0);
  return fields;
}

// The parts of `table`, scanned.
PartPipeline ScanPipeline(const std::shared_ptr<MemoryTable>& table) {
  return [table,
          fields = AllFields(*table)](int64_t part) -> arrow::Result<std::unique_ptr<Operator>> {
    return std::make_unique<TableScanOperator>(table, fields, part);
  };
}

// The spec of a build of `schema` on these BIGINT keys; keyless for a one-row join.
std::shared_ptr<const JoinBuildSpec> SpecOf(plan::JoinKind kind,
                                            const std::shared_ptr<arrow::Schema>& schema,
                                            const std::vector<int>& keys) {
  if (kind == plan::JoinKind::kOneRow) {
    auto spec = JoinBuildSpec::Keyless(schema);
    EXPECT_TRUE(spec.ok()) << spec.status().ToString();
    return *spec;
  }
  std::vector<plan::BoundColumn> columns;
  columns.reserve(keys.size());
  for (const int key : keys) {
    columns.push_back(Column(key, schema->field(key)->name(), LogicalType::kBigInt));
  }
  return SpecOf(schema, std::move(columns));
}

// The build of a join of `kind` of `table`'s parts on these keys (BIGINT columns of the table).
std::shared_ptr<JoinBuild> ScanBuild(plan::JoinKind kind, const std::shared_ptr<MemoryTable>& table,
                                     const std::vector<int>& keys, ProfileNode* profile = nullptr) {
  return std::make_shared<JoinBuild>(SpecOf(kind, table->schema(), keys), ScanPipeline(table),
                                     table->num_parts(), std::vector<std::shared_ptr<JoinBuild>>{},
                                     profile, kind);
}

// An inner join's build of `table`'s parts on these keys.
std::shared_ptr<JoinBuild> ScanBuild(const std::shared_ptr<MemoryTable>& table,
                                     const std::vector<int>& keys, ProfileNode* profile = nullptr) {
  return ScanBuild(plan::JoinKind::kInner, table, keys, profile);
}

// The build of a join of `kind` of what `source` returns, on these BIGINT keys.
std::shared_ptr<JoinBuild> DrainedBuild(plan::JoinKind kind, std::unique_ptr<Operator> source,
                                        const std::vector<int>& keys,
                                        ProfileNode* profile = nullptr) {
  auto spec = SpecOf(kind, source->output_schema(), keys);
  return std::make_shared<JoinBuild>(std::move(spec), std::move(source), profile, kind);
}

// An inner join's build of what `source` returns, on these BIGINT keys.
std::shared_ptr<JoinBuild> DrainedBuild(std::unique_ptr<Operator> source,
                                        const std::vector<int>& keys,
                                        ProfileNode* profile = nullptr) {
  return DrainedBuild(plan::JoinKind::kInner, std::move(source), keys, profile);
}

std::unique_ptr<HashJoinOperator> MakeJoin(std::unique_ptr<Operator> probe,
                                           std::shared_ptr<JoinBuild> build, std::vector<int> keys,
                                           bool prepares, BuildSide build_side = BuildSide::kRight,
                                           std::vector<plan::ExprPtr> residual = {}) {
  auto join = HashJoinOperator::Make(std::move(probe), std::move(build), std::move(keys),
                                     build_side, std::move(residual), prepares);
  EXPECT_TRUE(join.ok()) << join.status().ToString();
  return join.ok() ? *std::move(join) : nullptr;
}

// The parts of `table`, each scanned and probing `build` on `keys` (columns of the table).
PartPipeline ProbePipeline(const std::shared_ptr<MemoryTable>& table,
                           const std::shared_ptr<JoinBuild>& build, const std::vector<int>& keys,
                           BuildSide build_side = BuildSide::kRight,
                           const std::vector<plan::ExprPtr>& residual = {}) {
  return [table, fields = AllFields(*table), build, keys, build_side,
          residual](int64_t part) -> arrow::Result<std::unique_ptr<Operator>> {
    ARROW_ASSIGN_OR_RAISE(
        std::unique_ptr<HashJoinOperator> join,
        HashJoinOperator::Make(std::make_unique<TableScanOperator>(table, fields, part), build,
                               keys, build_side, residual, /*prepares=*/false));
    return join;
  };
}

// The output schema of `pipeline`'s parts (from a pipeline built, never opened).
std::shared_ptr<arrow::Schema> SchemaOf(const PartPipeline& pipeline) {
  auto sample = pipeline(0);
  EXPECT_TRUE(sample.ok()) << sample.status().ToString();
  return (*sample)->output_schema();
}

// The rows the probe pipeline gives in part order (a part union), with `builds` prepared first.
arrow::Result<std::shared_ptr<arrow::Table>> RunUnion(
    const PartPipeline& pipeline, int64_t parts, std::vector<std::shared_ptr<JoinBuild>> builds,
    ExecContext ctx) {
  BuildsFirstOperator op(
      std::make_unique<PartUnionOperator>(pipeline, parts, SchemaOf(pipeline), std::nullopt),
      std::move(builds));
  return Drain(op, ctx);
}

// Every batch of `op` until the end, opened and closed with `ctx`.
arrow::Result<std::vector<Batch>> Batches(Operator& op, ExecContext ctx) {
  ARROW_RETURN_NOT_OK(op.Open(ctx));
  std::vector<Batch> batches;
  while (true) {
    ARROW_ASSIGN_OR_RAISE(Batch batch, op.Next());
    if (batch.end()) {
      break;
    }
    batches.push_back(std::move(batch));
  }
  ARROW_RETURN_NOT_OK(op.Close());
  return batches;
}

// A source over one batch of these columns.
std::unique_ptr<ScriptedSource> SourceOf(const std::shared_ptr<arrow::Schema>& schema,
                                         arrow::ArrayVector columns,
                                         std::shared_ptr<arrow::BooleanArray> selection = nullptr) {
  return std::make_unique<ScriptedSource>(
      schema, std::vector<Batch>{Batch{.data = BatchOf(schema, std::move(columns)),
                                       .selection = std::move(selection)}});
}

std::optional<int64_t> MetricOf(const ProfileNode& node, const std::string& name) {
  for (const ProfileMetric& metric : node.metrics()) {
    if (metric.name == name) {
      return metric.value;
    }
  }
  return std::nullopt;
}

// ---- expressions ----

plan::ExprPtr ColumnAt(int index, LogicalType type) {
  return std::make_shared<const plan::Expr>(
      plan::Expr{.node = plan::ColumnExpr{.index = index}, .type = type, .name = "c"});
}

plan::ExprPtr ConstantOf(int64_t value) {
  return std::make_shared<const plan::Expr>(plan::Expr{
      .node = plan::ConstantExpr{.value = testing::BigInt(value)}, .type = LogicalType::kBigInt});
}

plan::ExprPtr Times(plan::ExprPtr left, plan::ExprPtr right) {
  return std::make_shared<const plan::Expr>(plan::Expr{
      .node =
          plan::ArithExpr{
              .op = plan::ArithOp::kMultiply, .left = std::move(left), .right = std::move(right)},
      .type = LogicalType::kBigInt});
}

// `operand <op> value` (BIGINT), as a WHERE condition inside an expression.
plan::ExprPtr Condition(plan::CompareOp op, int64_t value, plan::ExprPtr operand) {
  plan::Predicate predicate =
      testing::Compare(Column(0, "o", LogicalType::kBigInt), op, testing::BigInt(value));
  return std::make_shared<const plan::Expr>(plan::Expr{
      .node =
          plan::PredicateExpr{.predicate = std::move(predicate), .operands = {std::move(operand)}},
      .type = LogicalType::kBoolean});
}

// ---- the 1:1 and 1:N paths ----

// A build of unique keys: each window of at most batch_size rows of a probe batch is a slice of it
// (its buffers, at an offset), selected where it matched (a row the probe batch does not select
// matches nothing), with the build's columns where it matched and NULL elsewhere; a window without
// a match is skipped.
TEST_F(HashJoinTest, OneToOnePathKeepsTheProbeColumns) {
  // The build's and the probe's second columns are not nullable: the join's build column is.
  const auto build_schema = arrow::schema(
      {arrow::field("k", arrow::int64()), arrow::field("v", arrow::int64(), /*nullable=*/false)});
  const std::vector<Batch> build_batches = {
      Batch{.data =
                BatchOf(build_schema, {Int64s({0, 1, 2, 3, 4}), Int64s({100, 101, 102, 103, 104})}),
            .selection = nullptr},
      Batch{.data =
                BatchOf(build_schema, {Int64s({5, 6, 7, 8, 9}), Int64s({105, 106, 107, 108, 109})}),
            .selection = nullptr}};
  const auto probe_schema = arrow::schema(
      {arrow::field("a", arrow::int64()), arrow::field("b", arrow::int64(), /*nullable=*/false)});
  const std::vector<std::optional<int64_t>> probe_keys = {1, 20, 3, std::nullopt, 5, 6, 30, 7};
  const auto probe = BatchOf(probe_schema, {Int64s(probe_keys), Int64s({0, 1, 2, 3, 4, 5, 6, 7})});
  const auto selection = Bools({true, true, false, true, true, true, true, true});
  const std::vector<bool> matched = {true, false, false, false, true, true, false, true};
  for (const int64_t batch_size : {1, 3, 64}) {
    SCOPED_TRACE(batch_size);
    auto join = MakeJoin(
        std::make_unique<ScriptedSource>(
            probe_schema, std::vector<Batch>{Batch{.data = probe, .selection = selection}}),
        DrainedBuild(std::make_unique<ScriptedSource>(build_schema, build_batches), {0}), {0},
        /*prepares=*/true);
    ASSERT_NE(join, nullptr);
    EXPECT_FALSE(join->output_schema()->field(1)->nullable());  // the probe's, as it is
    EXPECT_TRUE(join->output_schema()->field(3)->nullable());   // the build's
    auto batches = Batches(*join, ContextOf(nullptr, batch_size));
    ASSERT_TRUE(batches.ok()) << batches.status().ToString();
    std::size_t next = 0;
    for (int64_t begin = 0; begin < probe->num_rows(); begin += batch_size) {
      const int64_t rows = std::min(batch_size, probe->num_rows() - begin);
      int64_t hits = 0;
      for (int64_t i = begin; i < begin + rows; ++i) {
        hits += matched[static_cast<std::size_t>(i)] ? 1 : 0;
      }
      if (hits == 0) {
        continue;  // skipped
      }
      ASSERT_LT(next, batches->size());
      const Batch& out = (*batches)[next++];
      ASSERT_EQ(out.data->num_rows(), rows);
      for (int c = 0; c < 2; ++c) {  // the probe's columns: its buffers, sliced
        EXPECT_EQ(out.data->column(c)->data()->buffers[1]->data(),
                  probe->column(c)->data()->buffers[1]->data());
        EXPECT_EQ(out.data->column(c)->offset(), begin);
      }
      if (hits == rows) {
        EXPECT_EQ(out.selection, nullptr);
      } else {
        ASSERT_NE(out.selection, nullptr);
      }
      const auto& values = static_cast<const arrow::Int64Array&>(*out.data->column(3));
      for (int64_t i = 0; i < rows; ++i) {
        const auto row = static_cast<std::size_t>(begin + i);
        if (out.selection != nullptr) {
          EXPECT_EQ(out.selection->Value(i), matched[row]) << row;
        }
        if (matched[row]) {
          EXPECT_EQ(values.Value(i), 100 + probe_keys[row].value_or(-1000)) << row;
        } else {
          EXPECT_TRUE(values.IsNull(i)) << row;
        }
      }
    }
    EXPECT_EQ(next, batches->size());
  }
}

// A key repeated 10 times across two build parts: a probe row's matches fan out across output
// batches of at most batch_size rows, in probe order, each row's matches in the build's (part,
// row) order; without a selection. The same on one thread and on the pool.
TEST_F(HashJoinTest, FanOutContinuesAcrossBatches) {
  const auto pool = MakeThreadPool();
  // Key 7 for rows 0-6 and 8-10 (6 in part 0, 4 in part 1), 8 for row 7, 9 for row 11.
  const auto build_table = KeyTable("b", 2, 6, [](int64_t i) -> std::optional<int64_t> {
    if (i == 7) {
      return 8;
    }
    return i == 11 ? 9 : 7;
  });
  const auto probe_schema = Int64Schema({"pk", "pid"});
  const arrow::ArrayVector probe_columns = {Int64s({7, std::nullopt, 8, 7}), Int64s({0, 1, 2, 3})};
  const Rows expected =
      ReferenceJoin(RowsOf(BatchOf(probe_schema, probe_columns)), RowsOf(*build_table), {0}, {0});
  ASSERT_EQ(expected.size(), 21U);
  for (arrow::internal::Executor* executor : Executors(pool.get())) {
    auto join = MakeJoin(SourceOf(probe_schema, probe_columns), ScanBuild(build_table, {0}), {0},
                         /*prepares=*/true);
    ASSERT_NE(join, nullptr);
    auto batches = Batches(*join, ContextOf(executor, 3));
    ASSERT_TRUE(batches.ok()) << batches.status().ToString();
    for (const Batch& batch : *batches) {
      EXPECT_LE(batch.data->num_rows(), 3);
      EXPECT_EQ(batch.selection, nullptr);
    }
    EXPECT_EQ(batches->size(), 7U);
    EXPECT_EQ(RowsOf(join->output_schema(), *batches), expected);
  }
}

// A NULL in either key column, on either side, never matches, on both paths. A build whose rows all
// have a NULL key holds no row (counted in null_keys): its probe input is never opened.
TEST_F(HashJoinTest, NullKeysNeverMatch) {
  const auto build_schema = Int64Schema({"k1", "k2", "id"});
  const auto probe_schema = Int64Schema({"a1", "a2", "pid"});
  const std::optional<int64_t> null;
  const arrow::ArrayVector probe = {Int64s({1, 1, null, null, 2}), Int64s({1, null, 1, null, 2}),
                                    Int64s({0, 1, 2, 3, 4})};
  for (const bool repeated : {false, true}) {
    SCOPED_TRACE(repeated ? "1:N" : "1:1");
    arrow::ArrayVector build = {Int64s({1, 1, null, null, 2}), Int64s({1, null, 1, null, 2}),
                                Int64s({10, 11, 12, 13, 14})};
    if (repeated) {
      build = {Int64s({1, 1, null, null, 2, 2}), Int64s({1, null, 1, null, 2, 2}),
               Int64s({10, 11, 12, 13, 14, 15})};
    }
    const Rows expected = ReferenceJoin(RowsOf(BatchOf(probe_schema, probe)),
                                        RowsOf(BatchOf(build_schema, build)), {0, 1}, {0, 1});
    ASSERT_EQ(expected.size(), repeated ? 3U : 2U);
    auto join = MakeJoin(SourceOf(probe_schema, probe),
                         DrainedBuild(SourceOf(build_schema, build), {0, 1}), {0, 1},
                         /*prepares=*/true);
    ASSERT_NE(join, nullptr);
    ExecContext ctx = ContextOf(nullptr);
    auto result = Drain(*join, ctx);
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    EXPECT_EQ(RowsOf(**result), expected);
  }
  // Only NULL keys.
  ProfileNode profile;
  auto build = DrainedBuild(SourceOf(build_schema, {Int64s({1, null, null}),
                                                    Int64s({null, 2, null}), Int64s({10, 11, 12})}),
                            {0, 1}, &profile);
  auto source = SourceOf(probe_schema, probe);
  const ScriptedSource& probed = *source;
  auto join = MakeJoin(std::move(source), build, {0, 1}, /*prepares=*/true);
  ASSERT_NE(join, nullptr);
  ExecContext ctx = ContextOf(nullptr);
  auto result = Drain(*join, ctx);
  ASSERT_TRUE(result.ok()) << result.status().ToString();
  EXPECT_EQ((*result)->num_rows(), 0);
  EXPECT_EQ(probed.opens(), 0);
  EXPECT_EQ(MetricOf(profile, "null_keys"), 3);
  EXPECT_EQ(profile.rows(), 0);
}

// Keys of two columns: (BIGINT, VARCHAR), where '' is not NULL and 'XL ' is not 'XL', and
// (INTEGER, DATE), with unique and repeated keys.
TEST_F(HashJoinTest, MultiColumnKeys) {
  const std::optional<std::string> no_text;
  {
    const auto build_schema =
        arrow::schema({arrow::field("k", arrow::int64()), arrow::field("s", arrow::binary()),
                       arrow::field("id", arrow::int64())});
    const auto probe_schema =
        arrow::schema({arrow::field("a", arrow::int64()), arrow::field("t", arrow::binary()),
                       arrow::field("pid", arrow::int64())});
    const arrow::ArrayVector build = {Int64s({1, 1, 1, 2, 2}),
                                      Strings({"XL", "", no_text, "XL ", "XL"}),
                                      Int64s({10, 11, 12, 13, 14})};
    const arrow::ArrayVector probe = {Int64s({1, 1, 1, 2, 2, 3}),
                                      Strings({"XL", "", no_text, "XL", "XL ", "XL"}),
                                      Int64s({0, 1, 2, 3, 4, 5})};
    const Rows expected = ReferenceJoin(RowsOf(BatchOf(probe_schema, probe)),
                                        RowsOf(BatchOf(build_schema, build)), {0, 1}, {0, 1});
    ASSERT_EQ(expected.size(), 4U);
    auto spec = SpecOf(build_schema, {Column(0, "k", LogicalType::kBigInt),
                                      Column(1, "s", LogicalType::kVarchar)});
    auto join = MakeJoin(SourceOf(probe_schema, probe),
                         std::make_shared<JoinBuild>(spec, SourceOf(build_schema, build), nullptr,
                                                     plan::JoinKind::kInner),
                         {0, 1}, /*prepares=*/true);
    ASSERT_NE(join, nullptr);
    ExecContext ctx = ContextOf(nullptr);
    auto result = Drain(*join, ctx);
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    EXPECT_EQ(RowsOf(**result), expected);
  }
  {
    const auto build_schema =
        arrow::schema({arrow::field("k", arrow::int32()), arrow::field("d", arrow::date32()),
                       arrow::field("id", arrow::int64())});
    const auto probe_schema =
        arrow::schema({arrow::field("a", arrow::int32()), arrow::field("e", arrow::date32()),
                       arrow::field("pid", arrow::int64())});
    const arrow::ArrayVector build = {Int32s({1, 1, 2, 2}), Dates({19000, 19001, 19000, 19000}),
                                      Int64s({10, 11, 12, 13})};
    const arrow::ArrayVector probe = {Int32s({1, 2, 2, std::nullopt}),
                                      Dates({19001, 19000, 19001, 19000}), Int64s({0, 1, 2, 3})};
    const Rows expected = ReferenceJoin(RowsOf(BatchOf(probe_schema, probe)),
                                        RowsOf(BatchOf(build_schema, build)), {0, 1}, {0, 1});
    ASSERT_EQ(expected.size(), 3U);
    auto spec = SpecOf(build_schema,
                       {Column(0, "k", LogicalType::kInteger), Column(1, "d", LogicalType::kDate)});
    auto join = MakeJoin(SourceOf(probe_schema, probe),
                         std::make_shared<JoinBuild>(spec, SourceOf(build_schema, build), nullptr,
                                                     plan::JoinKind::kInner),
                         {0, 1}, /*prepares=*/true);
    ASSERT_NE(join, nullptr);
    ExecContext ctx = ContextOf(nullptr);
    auto result = Drain(*join, ctx);
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    EXPECT_EQ(RowsOf(**result), expected);
  }
}

// A BOOLEAN build column (gathered by slices, with NULLs) on the 1:1 path, where a row without a
// match gets NULL, and on the 1:N path.
TEST_F(HashJoinTest, BooleanPayloadOnBothPaths) {
  const auto build_schema =
      arrow::schema({arrow::field("k", arrow::int64()), arrow::field("flag", arrow::boolean())});
  const auto probe_schema = Int64Schema({"a"});
  const std::optional<bool> no_flag;
  {
    auto join = MakeJoin(SourceOf(probe_schema, {Int64s({1, 9, 2, 3, 9, 9, 4})}),
                         DrainedBuild(SourceOf(build_schema, {Int64s({1, 2, 3, 4}),
                                                              Bools({true, no_flag, false, true})}),
                                      {0}),
                         {0}, /*prepares=*/true);
    ASSERT_NE(join, nullptr);
    auto batches = Batches(*join, ContextOf(nullptr, 64));
    ASSERT_TRUE(batches.ok()) << batches.status().ToString();
    ASSERT_EQ(batches->size(), 1U);
    const auto& flags = static_cast<const arrow::BooleanArray&>(*(*batches)[0].data->column(2));
    const std::vector<std::optional<bool>> expected = {true,    no_flag, no_flag, false,
                                                       no_flag, no_flag, true};
    ASSERT_EQ(flags.length(), 7);
    for (int64_t i = 0; i < flags.length(); ++i) {
      const auto& want = expected[static_cast<std::size_t>(i)];
      EXPECT_EQ(flags.IsValid(i), want.has_value()) << i;
      if (want.has_value() && flags.IsValid(i)) {
        EXPECT_EQ(flags.Value(i), *want) << i;
      }
    }
  }
  {
    const arrow::ArrayVector build = {Int64s({1, 1, 2, 1, 2}),
                                      Bools({true, no_flag, false, false, true})};
    const arrow::ArrayVector probe = {Int64s({2, 1, 3})};
    const Rows expected = ReferenceJoin(RowsOf(BatchOf(probe_schema, probe)),
                                        RowsOf(BatchOf(build_schema, build)), {0}, {0});
    auto join = MakeJoin(SourceOf(probe_schema, probe),
                         DrainedBuild(SourceOf(build_schema, build), {0}), {0}, /*prepares=*/true);
    ASSERT_NE(join, nullptr);
    ExecContext ctx = ContextOf(nullptr, 2);
    auto result = Drain(*join, ctx);
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    EXPECT_EQ(RowsOf(**result), expected);
  }
}

// A build on the left: its columns come first, and residuals read the join's output in that
// order.
TEST_F(HashJoinTest, BuildOnTheLeftPutsItsColumnsFirst) {
  const auto probe_table = KeyTable("p", 4, 3, [](int64_t i) { return i % 7; });
  // Repeated build keys (the 1:N path), then unique ones (the 1:1 path).
  for (const bool unique : {false, true}) {
    SCOPED_TRACE(unique ? "1:1" : "1:N");
    const auto build_table =
        KeyTable("b", 3, 4, [unique](int64_t i) { return unique ? i : i % 5; });
    const Rows all =
        ReferenceJoin(RowsOf(*probe_table), RowsOf(*build_table), {0}, {0}, BuildSide::kLeft);
    // The build's id (column 1) below 6.
    Rows expected;
    for (const std::vector<std::string>& row : all) {
      if (std::stoll(row[1]) < 6) {
        expected.push_back(row);
      }
    }
    ASSERT_FALSE(expected.empty());
    ASSERT_LT(expected.size(), all.size());
    const auto build = ScanBuild(build_table, {0});
    for (const bool residual : {false, true}) {
      const PartPipeline pipeline =
          ProbePipeline(probe_table, build, {0}, BuildSide::kLeft,
                        residual ? std::vector<plan::ExprPtr>{Condition(
                                       plan::CompareOp::kLt, 6, ColumnAt(1, LogicalType::kBigInt))}
                                 : std::vector<plan::ExprPtr>{});
      const auto schema = SchemaOf(pipeline);
      EXPECT_EQ(schema->field(0)->name(), "bk");
      EXPECT_EQ(schema->field(2)->name(), "pk");
      EXPECT_TRUE(schema->field(0)->nullable());
      auto result = RunUnion(pipeline, probe_table->num_parts(), {build}, ContextOf(nullptr));
      ASSERT_TRUE(result.ok()) << result.status().ToString();
      EXPECT_EQ(RowsOf(**result), residual ? expected : all);
    }
  }
}

// ---- residuals ----

// Residuals keep the rows where every one is true (FALSE and NULL drop a row), on both paths. Each
// is evaluated only on the rows the ones before it kept: an overflow after a narrowing condition
// raises nothing, as in DuckDB; it is an error when no condition comes first. A probe row without a
// match never meets a residual.
TEST_F(HashJoinTest, ResidualsKeepRowsWhereEveryConditionIsTrue) {
  const auto build_schema = Int64Schema({"k", "v"});
  const auto probe_schema =
      arrow::schema({arrow::field("a", arrow::int64()), arrow::field("b", arrow::int64()),
                     arrow::field("p", arrow::boolean())});
  const std::optional<bool> unknown;
  // Rows 6 and 7 match nothing; their b overflows b * 2^62.
  const arrow::ArrayVector probe = {Int64s({1, 2, 3, 4, 5, 6, 99, 98}),
                                    Int64s({0, 1, 5, 1, 0, 9, 7, 8}),
                                    Bools({true, false, unknown, true, true, unknown, true, true})};
  const Rows probe_rows = RowsOf(BatchOf(probe_schema, probe));
  constexpr int64_t kTwoTo62 = 4611686018427387904;
  for (const bool repeated : {false, true}) {
    SCOPED_TRACE(repeated ? "1:N" : "1:1");
    const arrow::ArrayVector build =
        repeated ? arrow::ArrayVector{Int64s({1, 1, 2, 3, 4, 4, 5, 6}),
                                      Int64s({10, 11, 20, 30, 40, 41, 50, 60})}
                 : arrow::ArrayVector{Int64s({1, 2, 3, 4, 5, 6}), Int64s({10, 20, 30, 40, 50, 60})};
    const Rows joined = ReferenceJoin(probe_rows, RowsOf(BatchOf(build_schema, build)), {0}, {0});
    const auto run = [&](std::vector<plan::ExprPtr> residual) {
      auto join = MakeJoin(SourceOf(probe_schema, probe),
                           DrainedBuild(SourceOf(build_schema, build), {0}), {0},
                           /*prepares=*/true, BuildSide::kRight, std::move(residual));
      EXPECT_NE(join, nullptr);
      ExecContext ctx = ContextOf(nullptr, 3);
      return Drain(*join, ctx);
    };
    // p: TRUE keeps, FALSE and NULL drop.
    auto flagged = run({ColumnAt(2, LogicalType::kBoolean)});
    ASSERT_TRUE(flagged.ok()) << flagged.status().ToString();
    EXPECT_EQ(RowsOf(**flagged), WhereTrue(joined, 2));
    // b < 2, then b * 2^62 > 0: only b = 1 passes, and b = 5 or 9 never reaches the product.
    const plan::ExprPtr narrow =
        Condition(plan::CompareOp::kLt, 2, ColumnAt(1, LogicalType::kBigInt));
    const plan::ExprPtr product = Condition(
        plan::CompareOp::kGt, 0, Times(ColumnAt(1, LogicalType::kBigInt), ConstantOf(kTwoTo62)));
    auto ordered = run({narrow, product});
    ASSERT_TRUE(ordered.ok()) << ordered.status().ToString();
    Rows expected;
    for (const std::vector<std::string>& row : joined) {
      if (row[1] == "1") {
        expected.push_back(row);
      }
    }
    ASSERT_FALSE(expected.empty());
    EXPECT_EQ(RowsOf(**ordered), expected);
    auto reversed = run({product, narrow});
    EXPECT_TRUE(reversed.status().IsExecutionError()) << reversed.status().ToString();
    // Without the overflowing matched rows (b = 5 and 9), the product alone raises nothing: the
    // probe rows without a match (b = 7 and 8) never meet it.
    const arrow::ArrayVector small_build =
        repeated ? arrow::ArrayVector{Int64s({1, 1, 2, 4, 4, 5}), Int64s({10, 11, 20, 40, 41, 50})}
                 : arrow::ArrayVector{Int64s({1, 2, 4, 5}), Int64s({10, 20, 40, 50})};
    auto join = MakeJoin(SourceOf(probe_schema, probe),
                         DrainedBuild(SourceOf(build_schema, small_build), {0}), {0},
                         /*prepares=*/true, BuildSide::kRight, {product});
    ASSERT_NE(join, nullptr);
    ExecContext ctx = ContextOf(nullptr, 3);
    auto alone = Drain(*join, ctx);
    ASSERT_TRUE(alone.ok()) << alone.status().ToString();
    EXPECT_EQ((*alone)->num_rows(), repeated ? 3 : 2);  // the matched rows with b = 1
  }
}

// ---- empty builds ----

// A build that holds no row (no part, every row filtered out, only NULL keys): the probe never
// opens its input, over a serial input or in a part pipeline, where the sink still answers (a
// COUNT(*) of 0); in a chain, the builds below it are not prepared.
TEST_F(HashJoinTest, EmptyBuildOpensNoProbeInput) {
  const auto pool = MakeThreadPool();
  const auto key = [](int64_t i) -> std::optional<int64_t> { return i % 4; };
  const std::vector<plan::AggregateCall> count_star = {
      {.kind = plan::AggKind::kCountStar, .arg = {}, .type = LogicalType::kBigInt}};
  // The empty builds.
  const auto no_part = [&] { return ScanBuild(KeyTable("b", 0, 3, key), {0}); };
  const auto filtered = [&] {
    const auto table = KeyTable("b", 3, 4, key);
    PartPipeline pipeline = [table](int64_t part) -> arrow::Result<std::unique_ptr<Operator>> {
      return std::make_unique<FilterOperator>(
          std::make_unique<TableScanOperator>(table, std::vector<int>{0, 1}, part),
          std::vector<plan::Predicate>{testing::Compare(
              Column(0, "bk", LogicalType::kBigInt), plan::CompareOp::kGt, testing::BigInt(1000))});
    };
    return std::make_shared<JoinBuild>(
        SpecOf(table->schema(), {Column(0, "bk", LogicalType::kBigInt)}), std::move(pipeline),
        table->num_parts(), std::vector<std::shared_ptr<JoinBuild>>{}, nullptr,
        plan::JoinKind::kInner);
  };
  const auto null_keys = [&] {
    return ScanBuild(
        KeyTable("b", 2, 3, [](int64_t) -> std::optional<int64_t> { return std::nullopt; }), {0});
  };
  for (arrow::internal::Executor* executor : Executors(pool.get())) {
    SCOPED_TRACE(executor == nullptr ? "one thread" : "pool");
    const ExecContext ctx = ContextOf(executor);
    for (const auto& empty : {std::function<std::shared_ptr<JoinBuild>()>(no_part),
                              std::function<std::shared_ptr<JoinBuild>()>(filtered),
                              std::function<std::shared_ptr<JoinBuild>()>(null_keys)}) {
      {  // over a serial input
        auto source = SourceOf(Int64Schema({"pk", "pid"}), {Int64s({0, 1, 2}), Int64s({0, 1, 2})});
        const ScriptedSource& probed = *source;
        auto join = MakeJoin(std::move(source), empty(), {0}, /*prepares=*/true);
        ASSERT_NE(join, nullptr);
        ExecContext run = ctx;
        auto result = Drain(*join, run);
        ASSERT_TRUE(result.ok()) << result.status().ToString();
        EXPECT_EQ((*result)->num_rows(), 0);
        EXPECT_EQ(probed.opens(), 0);
        EXPECT_EQ(probed.closes(), 0);
      }
      {  // in a part pipeline, under an aggregate
        const auto probe_table = KeyTable("p", 4, 5, key);
        const auto build = empty();
        BuildsFirstOperator op(
            std::make_unique<PartAggregateOperator>(ProbePipeline(probe_table, build, {0}),
                                                    probe_table->num_parts(), 4, count_star),
            {build});
        ExecContext run = ctx;
        auto result = Drain(op, run);
        ASSERT_TRUE(result.ok()) << result.status().ToString();
        EXPECT_EQ(testing::SingleInt64(**result), 0);
        EXPECT_TRUE(probe_table->scanned_parts().empty());
      }
    }
    // A chain: the probe joins `inner`, then the empty `outer`; `inner` is never prepared.
    const auto probe_table = KeyTable("p", 4, 5, key);
    const auto inner_table = KeyTable("c", 2, 3, key);
    const auto inner = ScanBuild(inner_table, {0});
    const auto outer = null_keys();
    PartPipeline chain = [probe_table, inner,
                          outer](int64_t part) -> arrow::Result<std::unique_ptr<Operator>> {
      ARROW_ASSIGN_OR_RAISE(std::unique_ptr<HashJoinOperator> first,
                            HashJoinOperator::Make(std::make_unique<TableScanOperator>(
                                                       probe_table, std::vector<int>{0, 1}, part),
                                                   inner, {0}, BuildSide::kRight, {}, false));
      ARROW_ASSIGN_OR_RAISE(
          std::unique_ptr<HashJoinOperator> second,
          HashJoinOperator::Make(std::move(first), outer, {2}, BuildSide::kRight, {}, false));
      return second;
    };
    ExecContext run = ctx;
    auto result = RunUnion(chain, probe_table->num_parts(), {outer, inner}, run);
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    EXPECT_EQ((*result)->num_rows(), 0);
    EXPECT_TRUE(inner_table->scanned_parts().empty());
    EXPECT_TRUE(probe_table->scanned_parts().empty());
    EXPECT_EQ(inner->table(), nullptr);
  }
}

// ---- drained builds ----

// A build of any operator's batches, drained on the calling thread (the selected rows only, a
// batch without one skipped), its batches then the parts: the same rows on one thread and on the
// pool. A failing input's error comes out, the input is closed, and the build holds nothing.
TEST_F(HashJoinTest, DrainedBuildInput) {
  const auto pool = MakeThreadPool();
  const auto build_schema = Int64Schema({"k", "id"});
  const std::optional<int64_t> null;
  const std::vector<Batch> build_batches = {
      Batch{.data = BatchOf(build_schema, {Int64s({1, 2, 3, 4}), Int64s({0, 1, 2, 3})}),
            .selection = Bools({true, false, true, true})},
      Batch{.data = BatchOf(build_schema, {Int64s({2, 5, null, 6}), Int64s({4, 5, 6, 7})}),
            .selection = nullptr},
      Batch{.data = BatchOf(build_schema, {Int64s({7, 8}), Int64s({8, 9})}),
            .selection = Bools({false, false})},
      Batch{.data = BatchOf(build_schema, {Int64s({1, 3}), Int64s({10, 11})}),
            .selection = nullptr}};
  const Rows build_rows = RowsOf(build_schema, build_batches);
  const auto probe_schema = Int64Schema({"a", "pid"});
  const arrow::ArrayVector probe = {Int64s({1, 2, 3, 4, 5, 6, 7, 8, null}),
                                    Int64s({0, 1, 2, 3, 4, 5, 6, 7, 8})};
  const Rows expected = ReferenceJoin(RowsOf(BatchOf(probe_schema, probe)), build_rows, {0}, {0});
  for (arrow::internal::Executor* executor : Executors(pool.get())) {
    SCOPED_TRACE(executor == nullptr ? "one thread" : "pool");
    ProfileNode profile;
    auto source = std::make_unique<ScriptedSource>(build_schema, build_batches);
    const ScriptedSource& drained = *source;
    auto join = MakeJoin(SourceOf(probe_schema, probe),
                         DrainedBuild(std::move(source), {0}, &profile), {0}, /*prepares=*/true);
    ASSERT_NE(join, nullptr);
    ExecContext ctx = ContextOf(executor, 2);
    auto result = Drain(*join, ctx);
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    EXPECT_EQ(RowsOf(**result), expected);
    EXPECT_EQ(drained.opens(), 1);
    EXPECT_EQ(drained.closes(), 1);
    EXPECT_EQ(MetricOf(profile, "parts"), 3);  // the batch without a selected row is no part
    EXPECT_EQ(MetricOf(profile, "null_keys"), 1);
    EXPECT_EQ(profile.rows(), 8);
  }
  // A failing input: closed, its error out, nothing held.
  auto failing = std::make_unique<ScriptedSource>(build_schema, build_batches);
  failing->FailAt(1);
  const ScriptedSource& broken = *failing;
  const auto build = DrainedBuild(std::move(failing), {0});
  ExecContext ctx = ContextOf(nullptr);
  const arrow::Status prepared = build->Prepare(ctx);
  EXPECT_TRUE(prepared.IsIOError()) << prepared.ToString();
  EXPECT_EQ(broken.closes(), 1);
  EXPECT_EQ(build->table(), nullptr);
  auto join = MakeJoin(SourceOf(probe_schema, probe), build, {0}, /*prepares=*/true);
  ASSERT_NE(join, nullptr);
  ASSERT_TRUE(join->Open(ctx).ok());
  EXPECT_TRUE(join->Next().status().IsIOError());
  EXPECT_TRUE(join->Close().ok());
  EXPECT_EQ(broken.closes(), 2);
}

// ---- builds and sinks ----

// A probe pipeline under every part sink, its build from another table's parts: one thread and the
// pool give the same rows, and a part union gives the nested-loop reference, in its order.
TEST_F(HashJoinTest, EverySinkGivesTheSameRowsOnAnyNumberOfThreads) {
  const auto pool = MakeThreadPool();
  // Build keys 0..9 then 0..4 again (repeats); probe keys i % 13, NULL every 11th row.
  const auto build_table = KeyTable("b", 3, 5, [](int64_t i) { return i % 10; });
  const auto probe_table = KeyTable("p", 6, 7, [](int64_t i) -> std::optional<int64_t> {
    if (i % 11 == 5) {
      return std::nullopt;
    }
    return i % 13;
  });
  const Rows expected = ReferenceJoin(RowsOf(*probe_table), RowsOf(*build_table), {0}, {0});
  const auto bk = Column(2, "bk", LogicalType::kBigInt);
  const auto pid = Column(1, "pid", LogicalType::kBigInt);
  const auto bid = Column(3, "bid", LogicalType::kBigInt);
  const std::vector<plan::AggregateCall> calls = {
      {.kind = plan::AggKind::kCountStar, .arg = {}, .type = LogicalType::kBigInt},
      {.kind = plan::AggKind::kMin, .arg = bid, .type = LogicalType::kBigInt},
      {.kind = plan::AggKind::kMax, .arg = pid, .type = LogicalType::kBigInt}};
  const std::vector<plan::AggregateCall> two_level = {
      {.kind = plan::AggKind::kCountDistinct, .arg = pid, .type = LogicalType::kBigInt},
      {.kind = plan::AggKind::kCountStar, .arg = {}, .type = LogicalType::kBigInt}};
  const int64_t parts = probe_table->num_parts();
  // The sinks over the probe pipeline of `build`.
  const auto sinks = [&](const std::shared_ptr<JoinBuild>& build) {
    const PartPipeline pipeline = ProbePipeline(probe_table, build, {0});
    const auto schema = SchemaOf(pipeline);
    std::vector<std::unique_ptr<PartSink>> made;
    made.push_back(std::make_unique<PartUnionOperator>(pipeline, parts, schema, std::nullopt));
    made.push_back(std::make_unique<PartAggregateOperator>(pipeline, parts, 4, calls));
    const GroupAggregateOperator grouped(pipeline(0).ValueOrDie(), {bk}, calls);
    made.push_back(std::make_unique<PartGroupAggregateOperator>(
        pipeline, parts, 4, std::vector<plan::BoundColumn>{bk}, calls, grouped.output_schema()));
    const ScalarAggregateOperator global(pipeline(0).ValueOrDie(), two_level);
    made.push_back(std::make_unique<PartTwoLevelAggregateOperator>(
        pipeline, parts, 2, 4, std::vector<plan::BoundColumn>{}, two_level, global.output_schema(),
        /*global=*/true));
    made.push_back(std::make_unique<PartTopNOperator>(
        pipeline, parts, schema,
        std::vector<plan::SortKey>{{.column = pid, .descending = true},
                                   {.column = bid, .descending = true}},
        7, 0));
    return made;
  };
  std::vector<std::shared_ptr<arrow::Table>> serial;
  for (arrow::internal::Executor* executor : Executors(pool.get())) {
    SCOPED_TRACE(executor == nullptr ? "one thread" : "pool");
    const auto build = ScanBuild(build_table, {0});
    auto made = sinks(build);
    for (std::size_t i = 0; i < made.size(); ++i) {
      BuildsFirstOperator op(std::move(made[i]), {build});
      ExecContext ctx = ContextOf(executor, 4);
      auto result = Drain(op, ctx);
      ASSERT_TRUE(result.ok()) << i << ": " << result.status().ToString();
      if (executor == nullptr) {
        serial.push_back(*result);
      } else {
        EXPECT_TRUE((*result)->Equals(*serial[i])) << i;
      }
      if (i == 0) {
        EXPECT_EQ(RowsOf(**result), expected);
      }
      if (i == 1) {
        EXPECT_EQ(Int64Column(**result, 0),
                  (std::vector<std::optional<int64_t>>{static_cast<int64_t>(expected.size())}));
      }
      EXPECT_EQ(build->table(), nullptr) << i << ": released";
    }
  }
}

// A build whose input probes a build of its own prepares that one first (post-order) and releases
// it once its own table is built; joins chain in one probe pipeline, outermost build first. Both
// give the nested-loop reference.
TEST_F(HashJoinTest, NestedBuildsAndChains) {
  const auto pool = MakeThreadPool();
  const auto t1 = KeyTable("a", 4, 5, [](int64_t i) { return i % 6; });
  const auto t2 = KeyTable("b", 3, 4, [](int64_t i) { return i % 7; });
  const auto t3 = KeyTable("c", 2, 3, [](int64_t i) { return i % 5; });
  for (arrow::internal::Executor* executor : Executors(pool.get())) {
    SCOPED_TRACE(executor == nullptr ? "one thread" : "pool");
    {
      // t1 joins (t2 joins t3 on bid = ck) on ak = bk.
      std::mutex mu;
      std::vector<std::string> log;  // the tables whose parts were built, in order
      const auto logged = [&](const std::string& name, PartPipeline parts) -> PartPipeline {
        return [&mu, &log, name, built = std::move(parts)](int64_t part) {
          {
            const std::scoped_lock lock(mu);
            log.push_back(name);
          }
          return built(part);
        };
      };
      const auto inner = std::make_shared<JoinBuild>(
          SpecOf(t3->schema(), {Column(0, "ck", LogicalType::kBigInt)}),
          logged("c", ScanPipeline(t3)), t3->num_parts(), std::vector<std::shared_ptr<JoinBuild>>{},
          nullptr, plan::JoinKind::kInner);
      const PartPipeline middle = ProbePipeline(t2, inner, {1});
      const auto outer = std::make_shared<JoinBuild>(
          SpecOf(SchemaOf(middle), {Column(0, "bk", LogicalType::kBigInt)}), logged("b", middle),
          t2->num_parts(), std::vector<std::shared_ptr<JoinBuild>>{inner}, nullptr,
          plan::JoinKind::kInner);
      const Rows build_rows = ReferenceJoin(RowsOf(*t2), RowsOf(*t3), {1}, {0});
      const Rows expected = ReferenceJoin(RowsOf(*t1), build_rows, {0}, {0});
      ASSERT_FALSE(expected.empty());
      ExecContext ctx = ContextOf(executor);
      ASSERT_TRUE(outer->Prepare(ctx).ok());
      EXPECT_EQ(inner->table(), nullptr) << "released once the outer table is built";
      ASSERT_NE(outer->table(), nullptr);
      ASSERT_EQ(log.size(), 5U);
      EXPECT_EQ(log, (std::vector<std::string>{"c", "c", "b", "b", "b"}));
      outer->Release();
      auto result = RunUnion(ProbePipeline(t1, outer, {0}), t1->num_parts(), {outer}, ctx);
      ASSERT_TRUE(result.ok()) << result.status().ToString();
      EXPECT_EQ(RowsOf(**result), expected);
      EXPECT_EQ(outer->table(), nullptr);
    }
    {
      // t1 joins t2 on ak = bk, then t3 on aid = cid.
      const auto first = ScanBuild(t2, {0});
      const auto second = ScanBuild(t3, {1});
      PartPipeline chain = [t1, first,
                            second](int64_t part) -> arrow::Result<std::unique_ptr<Operator>> {
        ARROW_ASSIGN_OR_RAISE(std::unique_ptr<HashJoinOperator> join,
                              HashJoinOperator::Make(std::make_unique<TableScanOperator>(
                                                         t1, std::vector<int>{0, 1}, part),
                                                     first, {0}, BuildSide::kRight, {}, false));
        ARROW_ASSIGN_OR_RAISE(
            std::unique_ptr<HashJoinOperator> next,
            HashJoinOperator::Make(std::move(join), second, {1}, BuildSide::kRight, {}, false));
        return next;
      };
      const Rows expected =
          ReferenceJoin(ReferenceJoin(RowsOf(*t1), RowsOf(*t2), {0}, {0}), RowsOf(*t3), {1}, {1});
      ASSERT_FALSE(expected.empty());
      ExecContext ctx = ContextOf(executor);
      auto result = RunUnion(chain, t1->num_parts(), {second, first}, ctx);
      ASSERT_TRUE(result.ok()) << result.status().ToString();
      EXPECT_EQ(RowsOf(**result), expected);
    }
  }
}

// ---- errors ----

// A build's first failing part in part order decides its error, on one thread and on the pool; a
// build's error comes before any probe part runs; in a chain the outer build, prepared first,
// decides, and a failed PrepareBuilds holds no build.
TEST_F(HashJoinTest, FirstBuildErrorInPartOrderWins) {
  const auto pool = MakeThreadPool();
  const auto key = [](int64_t i) -> std::optional<int64_t> { return i % 3; };
  for (arrow::internal::Executor* executor : Executors(pool.get())) {
    SCOPED_TRACE(executor == nullptr ? "one thread" : "pool");
    ExecContext ctx = ContextOf(executor);
    const auto build_table = KeyTable("b", 6, 2, key);
    build_table->FailPart(2);
    build_table->FailPart(1);
    const auto build = ScanBuild(build_table, {0});
    const arrow::Status prepared = build->Prepare(ctx);
    EXPECT_TRUE(prepared.IsIOError()) << prepared.ToString();
    EXPECT_EQ(prepared.message(), "part 1 is broken");
    EXPECT_EQ(build->table(), nullptr);
    // The build fails before the probe's broken part is read.
    const auto probe_table = KeyTable("p", 3, 2, key);
    probe_table->FailPart(0);
    auto result = RunUnion(ProbePipeline(probe_table, build, {0}), 3, {build}, ctx);
    EXPECT_EQ(result.status().message(), "part 1 is broken");
    EXPECT_TRUE(probe_table->scanned_parts().empty());
    // A chain of two failing builds: the outer one's error.
    const auto outer_table = KeyTable("o", 4, 2, key);
    outer_table->FailPart(3);
    const auto outer = ScanBuild(outer_table, {0});
    const auto inner_table = KeyTable("i", 2, 2, key);
    inner_table->FailPart(0);
    const auto inner = ScanBuild(inner_table, {0});
    const std::vector<std::shared_ptr<JoinBuild>> chain = {outer, inner};
    EXPECT_EQ(PrepareBuilds(chain, ctx).message(), "part 3 is broken");
    EXPECT_TRUE(inner_table->scanned_parts().empty());
    // The outer one built, the inner one failing: neither is held.
    const auto good = ScanBuild(KeyTable("g", 2, 2, key), {0});
    const std::vector<std::shared_ptr<JoinBuild>> half = {good, inner};
    EXPECT_EQ(PrepareBuilds(half, ctx).message(), "part 0 is broken");
    EXPECT_EQ(good->table(), nullptr);
  }
}

// Every build part runs out of memory on a worker (as many parts at once might): each runs again
// alone on the calling thread, and the join's rows are those of a run without failures; a drained
// build's parts too.
TEST_F(HashJoinTest, ABuildPartRunsAgainAloneAfterOutOfMemory) {
  const auto pool = MakeThreadPool();
  const auto build_table = KeyTable("b", 6, 20, [](int64_t i) { return i % 37; });
  const auto probe_schema = Int64Schema({"pk", "pid"});
  std::vector<std::optional<int64_t>> keys;
  std::vector<std::optional<int64_t>> ids;
  for (int64_t i = 0; i < 50; ++i) {
    keys.emplace_back(i);
    ids.emplace_back(i);
  }
  const arrow::ArrayVector probe = {Int64s(keys), Int64s(ids)};
  const Rows expected =
      ReferenceJoin(RowsOf(BatchOf(probe_schema, probe)), RowsOf(*build_table), {0}, {0});
  // The build's batches, for a drained build.
  std::vector<Batch> batches;
  {
    auto reader = build_table->Scan({0, 1}, 20, arrow::default_memory_pool());
    ASSERT_TRUE(reader.ok());
    while (true) {
      std::shared_ptr<arrow::RecordBatch> batch;
      ASSERT_TRUE((*reader)->ReadNext(&batch).ok());
      if (batch == nullptr) {
        break;
      }
      batches.push_back(Batch{.data = batch, .selection = nullptr});
    }
  }
  for (const bool drained : {false, true}) {
    SCOPED_TRACE(drained ? "drained" : "pipeline");
    MemoryBudget budget(std::nullopt);
    WorkerFailingPool failing(&budget);
    {
      auto build =
          drained
              ? DrainedBuild(std::make_unique<ScriptedSource>(build_table->schema(), batches), {0})
              : ScanBuild(build_table, {0});
      auto join = MakeJoin(SourceOf(probe_schema, probe), build, {0}, /*prepares=*/true);
      ASSERT_NE(join, nullptr);
      ExecContext ctx{.pool = &failing,
                      .batch_size = 16,
                      .executor = pool.get(),
                      .threads = kThreads,
                      .budget = &budget};
      auto result = Drain(*join, ctx);
      ASSERT_TRUE(result.ok()) << result.status().ToString();
      EXPECT_EQ(RowsOf(**result), expected);
    }
    EXPECT_GE(failing.failures(), 6);  // every part failed on a worker first
    EXPECT_EQ(budget.bytes_allocated(), 0);
  }
}

// What the planner must not send is Invalid: probe keys of another count, outside the probe or of
// another type than their build key, residuals that are not BOOLEAN or read outside the join, a
// build whose kind does not fit (LEFT, building on the left, keys or none, residuals), a probe
// without its build prepared, and Next before Open or after Close.
TEST_F(HashJoinTest, MisuseIsInvalid) {
  const auto build_schema = Int64Schema({"k", "v"});
  const auto build = [&] {
    return DrainedBuild(SourceOf(build_schema, {Int64s({1}), Int64s({2})}), {0});
  };
  const auto probe_schema =
      arrow::schema({arrow::field("a", arrow::int64()), arrow::field("s", arrow::binary())});
  const auto probe = [&] { return SourceOf(probe_schema, {Int64s({1}), Strings({"x"})}); };
  const auto make = [&](std::unique_ptr<Operator> input, std::shared_ptr<JoinBuild> side,
                        std::vector<int> keys, std::vector<plan::ExprPtr> residual) {
    return HashJoinOperator::Make(std::move(input), std::move(side), std::move(keys),
                                  BuildSide::kRight, std::move(residual), true)
        .status();
  };
  EXPECT_TRUE(make(nullptr, build(), {0}, {}).IsInvalid());
  EXPECT_TRUE(make(probe(), nullptr, {0}, {}).IsInvalid());
  EXPECT_TRUE(make(probe(), build(), {0, 0}, {}).IsInvalid());
  EXPECT_TRUE(make(probe(), build(), {2}, {}).IsInvalid());
  EXPECT_TRUE(make(probe(), build(), {-1}, {}).IsInvalid());
  EXPECT_TRUE(make(probe(), build(), {1}, {}).IsInvalid());  // VARCHAR for BIGINT
  EXPECT_TRUE(make(probe(), build(), {0}, {nullptr}).IsInvalid());
  EXPECT_TRUE(make(probe(), build(), {0}, {ColumnAt(0, LogicalType::kBigInt)}).IsInvalid());
  EXPECT_TRUE(make(probe(), build(), {0}, {ColumnAt(4, LogicalType::kBoolean)}).IsInvalid());
  EXPECT_TRUE(make(probe(), build(), {0}, {ColumnAt(-1, LogicalType::kBoolean)}).IsInvalid());
  EXPECT_TRUE(make(probe(), build(), {0},
                   {Condition(plan::CompareOp::kLt, 1, ColumnAt(3, LogicalType::kBigInt))})
                  .ok());
  // The other kinds (the build's): a LEFT join (not run yet); a kind but inner building on the
  // left; a one-row join over a build with keys, and a semi join over one without; a null-aware
  // anti join of two keys; residuals on any kind but inner.
  const auto build_of = [&](plan::JoinKind kind, const std::shared_ptr<const JoinBuildSpec>& spec) {
    return std::make_shared<JoinBuild>(spec, SourceOf(build_schema, {Int64s({1}), Int64s({2})}),
                                       nullptr, kind);
  };
  const auto keyed = SpecOf(build_schema, {Column(0, "k", LogicalType::kBigInt)});
  const auto keyless = SpecOf(plan::JoinKind::kOneRow, build_schema, {});
  const auto two_keys = SpecOf(
      build_schema, {Column(0, "k", LogicalType::kBigInt), Column(1, "v", LogicalType::kBigInt)});
  const plan::ExprPtr residual =
      Condition(plan::CompareOp::kLt, 1, ColumnAt(3, LogicalType::kBigInt));
  EXPECT_TRUE(make(probe(), build_of(plan::JoinKind::kLeft, keyed), {0}, {}).IsInvalid());
  EXPECT_TRUE(HashJoinOperator::Make(probe(), build_of(plan::JoinKind::kSemi, keyed), {0},
                                     BuildSide::kLeft, {}, true)
                  .status()
                  .IsInvalid());
  EXPECT_TRUE(make(probe(), build_of(plan::JoinKind::kOneRow, keyed), {0}, {}).IsInvalid());
  EXPECT_TRUE(make(probe(), build_of(plan::JoinKind::kSemi, keyless), {}, {}).IsInvalid());
  EXPECT_TRUE(
      make(probe(), build_of(plan::JoinKind::kNullAwareAnti, two_keys), {0, 0}, {}).IsInvalid());
  for (const plan::JoinKind kind :
       {plan::JoinKind::kSemi, plan::JoinKind::kAnti, plan::JoinKind::kNullAwareAnti}) {
    EXPECT_TRUE(make(probe(), build_of(kind, keyed), {0}, {residual}).IsInvalid());
    EXPECT_TRUE(make(probe(), build_of(kind, keyed), {0}, {}).ok());
  }
  EXPECT_TRUE(
      make(probe(), build_of(plan::JoinKind::kOneRow, keyless), {}, {residual}).IsInvalid());
  EXPECT_TRUE(make(probe(), build_of(plan::JoinKind::kOneRow, keyless), {}, {}).ok());
  EXPECT_TRUE(make(probe(), build_of(plan::JoinKind::kSemi, two_keys), {0, 0}, {}).ok());

  ExecContext ctx = ContextOf(nullptr);
  // A probe in a pipeline whose build is not prepared.
  auto unprepared = HashJoinOperator::Make(probe(), build(), {0}, BuildSide::kRight, {}, false);
  ASSERT_TRUE(unprepared.ok()) << unprepared.status().ToString();
  EXPECT_TRUE((*unprepared)->Open(ctx).IsInvalid());
  EXPECT_TRUE((*unprepared)->Close().ok());
  // Next before Open and after Close.
  auto join = HashJoinOperator::Make(probe(), build(), {0}, BuildSide::kRight, {}, true);
  ASSERT_TRUE(join.ok()) << join.status().ToString();
  EXPECT_TRUE((*join)->Next().status().IsInvalid());
  ASSERT_TRUE((*join)->Open(ctx).ok());
  auto first = (*join)->Next();
  ASSERT_TRUE(first.ok()) << first.status().ToString();
  EXPECT_EQ(first->selected_rows(), 1);
  ASSERT_TRUE((*join)->Close().ok());
  EXPECT_TRUE((*join)->Next().status().IsInvalid());
  const auto table = KeyTable("p", 2, 2, [](int64_t i) { return i; });
  const auto scanned = ScanBuild(table, {0});
  BuildsFirstOperator op(
      std::make_unique<PartUnionOperator>(ScanPipeline(table), 2, table->schema(), std::nullopt),
      {scanned});
  EXPECT_TRUE(op.Next().status().IsInvalid());
  ASSERT_TRUE(op.Open(ctx).ok());
  ASSERT_TRUE(op.Next().ok());
  ASSERT_TRUE(op.Close().ok());
  EXPECT_TRUE(op.Next().status().IsInvalid());
}

// ---- lifecycle ----

// Builds are prepared when the operator that runs their probe pipeline is first pulled, not in its
// Open: an unpulled run (LIMIT 0) prepares nothing. A probe over a serial input prepares its build
// at its first Next and only then opens its input. A rerun builds again.
TEST_F(HashJoinTest, BuildsArePreparedAtTheFirstNext) {
  const auto key = [](int64_t i) -> std::optional<int64_t> { return i % 4; };
  ExecContext ctx = ContextOf(nullptr);
  {
    const auto build_table = KeyTable("b", 3, 2, key);
    const auto probe_table = KeyTable("p", 2, 3, key);
    const auto build = ScanBuild(build_table, {0});
    const PartPipeline pipeline = ProbePipeline(probe_table, build, {0});
    BuildsFirstOperator op(
        std::make_unique<PartUnionOperator>(pipeline, 2, SchemaOf(pipeline), std::nullopt),
        {build});
    ASSERT_TRUE(op.Open(ctx).ok());
    ASSERT_TRUE(op.Close().ok());
    EXPECT_TRUE(build_table->scanned_parts().empty());
    EXPECT_TRUE(probe_table->scanned_parts().empty());
    // Two runs: each builds.
    std::optional<Rows> first;
    for (int run = 1; run <= 2; ++run) {
      auto result = Drain(op, ctx);
      ASSERT_TRUE(result.ok()) << result.status().ToString();
      if (first.has_value()) {
        EXPECT_EQ(RowsOf(**result), *first);
      } else {
        first = RowsOf(**result);
      }
      EXPECT_EQ(build_table->scanned_parts().size(), static_cast<std::size_t>(3 * run));
    }
  }
  {
    // The probe's input opens after the build is prepared.
    const auto build_schema = Int64Schema({"k", "v"});
    auto build_source = SourceOf(build_schema, {Int64s({1, 2}), Int64s({10, 20})});
    const ScriptedSource& built = *build_source;
    const auto build = DrainedBuild(std::move(build_source), {0});
    std::vector<bool> prepared_at_open;
    class OnOpen final : public Operator {
     public:
      OnOpen(std::unique_ptr<Operator> input, std::function<void()> on_open)
          : input_(std::move(input)), on_open_(std::move(on_open)) {}
      [[nodiscard]] const std::shared_ptr<arrow::Schema>& output_schema() const override {
        return input_->output_schema();
      }
      arrow::Status Open(ExecContext& context) override {
        on_open_();
        return input_->Open(context);
      }
      arrow::Result<Batch> Next() override { return input_->Next(); }
      arrow::Status Close() override { return input_->Close(); }

     private:
      std::unique_ptr<Operator> input_;
      std::function<void()> on_open_;
    };
    auto probe = std::make_unique<OnOpen>(SourceOf(Int64Schema({"a"}), {Int64s({2, 3})}), [&] {
      prepared_at_open.push_back(build->table() != nullptr);
    });
    auto join = MakeJoin(std::move(probe), build, {0}, /*prepares=*/true);
    ASSERT_NE(join, nullptr);
    ASSERT_TRUE(join->Open(ctx).ok());
    EXPECT_EQ(built.opens(), 0);
    EXPECT_TRUE(prepared_at_open.empty());
    auto batch = join->Next();
    ASSERT_TRUE(batch.ok()) << batch.status().ToString();
    EXPECT_EQ(batch->selected_rows(), 1);
    EXPECT_EQ(built.opens(), 1);
    EXPECT_EQ(prepared_at_open, std::vector<bool>{true});
    auto end = join->Next();
    ASSERT_TRUE(end.ok() && end->end());
    EXPECT_EQ(build->table(), nullptr) << "released at the input's end";
    ASSERT_TRUE(join->Close().ok());
    // A rerun builds again.
    auto rerun = Drain(*join, ctx);
    ASSERT_TRUE(rerun.ok()) << rerun.status().ToString();
    EXPECT_EQ((*rerun)->num_rows(), 1);
    EXPECT_EQ(built.opens(), 2);
  }
}

// The builds are held while the probe pipeline's parts may run, released once the sink's parts
// are done (while the operator still exists, before its Close), and released at Close after a run
// that fails or stops early. A failed Prepare holds nothing while its build exists, not even the
// batches of a drained input.
TEST_F(HashJoinTest, BuildsHoldNothingOnceTheirPartsAreDone) {
  const auto pool = MakeThreadPool();
  const auto key = [](int64_t i) -> std::optional<int64_t> { return i % 5; };
  const std::vector<plan::AggregateCall> count_star = {
      {.kind = plan::AggKind::kCountStar, .arg = {}, .type = LogicalType::kBigInt}};
  for (arrow::internal::Executor* executor : Executors(pool.get())) {
    SCOPED_TRACE(executor == nullptr ? "one thread" : "pool");
    MemoryBudget budget(std::nullopt);
    ExecContext ctx = ContextOf(executor, 3, &budget);
    const auto build_table = KeyTable("b", 3, 4, key);
    const auto probe_table = KeyTable("p", 5, 4, key);
    {
      // An aggregate: its parts are done before it returns its row.
      const auto build = ScanBuild(build_table, {0});
      BuildsFirstOperator op(std::make_unique<PartAggregateOperator>(
                                 ProbePipeline(probe_table, build, {0}), 5, 4, count_star),
                             {build});
      ASSERT_TRUE(op.Open(ctx).ok());
      auto row = op.Next();
      ASSERT_TRUE(row.ok()) << row.status().ToString();
      EXPECT_EQ(build->table(), nullptr);
      EXPECT_TRUE(op.Close().ok());
    }
    {
      // A union: held while it hands on parts' batches, released at its end, before Close.
      const auto build = ScanBuild(build_table, {0});
      const PartPipeline pipeline = ProbePipeline(probe_table, build, {0});
      BuildsFirstOperator op(
          std::make_unique<PartUnionOperator>(pipeline, 5, SchemaOf(pipeline), std::nullopt),
          {build});
      ASSERT_TRUE(op.Open(ctx).ok());
      auto first = op.Next();
      ASSERT_TRUE(first.ok() && !first->end());
      EXPECT_NE(build->table(), nullptr);
      while (true) {
        auto batch = op.Next();
        ASSERT_TRUE(batch.ok()) << batch.status().ToString();
        if (batch->end()) {
          break;
        }
      }
      EXPECT_EQ(build->table(), nullptr);
      EXPECT_TRUE(op.Close().ok());
      // Stopped early: held until Close.
      ASSERT_TRUE(op.Open(ctx).ok());
      ASSERT_TRUE(op.Next().ok());
      EXPECT_NE(build->table(), nullptr);
      EXPECT_TRUE(op.Close().ok());
      EXPECT_EQ(build->table(), nullptr);
    }
    {
      // A failing probe part: held until Close.
      const auto broken = KeyTable("p", 5, 4, key);
      broken->FailPart(3);
      const auto build = ScanBuild(build_table, {0});
      const PartPipeline pipeline = ProbePipeline(broken, build, {0});
      BuildsFirstOperator op(
          std::make_unique<PartUnionOperator>(pipeline, 5, SchemaOf(pipeline), std::nullopt),
          {build});
      ASSERT_TRUE(op.Open(ctx).ok());
      arrow::Status failure;
      while (failure.ok()) {
        auto batch = op.Next();
        ASSERT_TRUE(!batch.ok() || !batch->end());
        failure = batch.status();
      }
      EXPECT_TRUE(failure.IsIOError()) << failure.ToString();
      EXPECT_NE(build->table(), nullptr);
      EXPECT_TRUE(op.Close().ok());
      EXPECT_EQ(build->table(), nullptr);
    }
    EXPECT_EQ(budget.bytes_allocated(), 0);
    // A failed Prepare: nothing held while the build exists, its nested build's table neither.
    MemoryBudget tiny(2048);
    ExecContext small = ContextOf(executor, 16, &tiny);
    const auto big_table = KeyTable("b", 4, 50, key);
    for (const bool drained : {false, true}) {
      std::shared_ptr<JoinBuild> build;
      if (drained) {
        std::vector<Batch> batches;
        for (int64_t part = 0; part < 4; ++part) {
          auto reader = big_table->ScanPart(part, {0, 1}, 50, arrow::default_memory_pool());
          ASSERT_TRUE(reader.ok());
          std::shared_ptr<arrow::RecordBatch> batch;
          ASSERT_TRUE((*reader)->ReadNext(&batch).ok());
          batches.push_back(Batch{.data = batch, .selection = nullptr});
        }
        build = DrainedBuild(std::make_unique<ScriptedSource>(big_table->schema(), batches), {0});
      } else {
        build = ScanBuild(big_table, {0});
      }
      const arrow::Status status = build->Prepare(small);
      EXPECT_TRUE(status.IsOutOfMemory()) << status.ToString();
      EXPECT_EQ(build->table(), nullptr);
      EXPECT_EQ(tiny.bytes_allocated(), 0) << (drained ? "drained" : "pipeline");
    }
    // A build whose input fails once its nested build is prepared: neither holds anything.
    const auto broken_input = KeyTable("b", 4, 5, key);
    broken_input->FailPart(1);
    const auto inner = ScanBuild(KeyTable("c", 1, 2, key), {0});
    const PartPipeline middle = ProbePipeline(broken_input, inner, {0});
    const auto outer = std::make_shared<JoinBuild>(
        SpecOf(SchemaOf(middle), {Column(1, "bid", LogicalType::kBigInt)}), middle,
        broken_input->num_parts(), std::vector<std::shared_ptr<JoinBuild>>{inner}, nullptr,
        plan::JoinKind::kInner);
    MemoryBudget counted(std::nullopt);
    ExecContext counting = ContextOf(executor, 3, &counted);
    const arrow::Status status = outer->Prepare(counting);
    EXPECT_TRUE(status.IsIOError()) << status.ToString();
    EXPECT_EQ(inner->table(), nullptr);
    EXPECT_EQ(counted.bytes_allocated(), 0);
    // A drained input whose batches come from the budget's pool (as a GROUP BY's would), its part
    // 3 failing: once Prepare returns, the budget holds none of its batches, also where the parts
    // ran on workers (no task owns a batch: a worker may destroy a task after Prepare returned).
    MemoryBudget pooled(std::nullopt);
    const auto drained_schema = Int64Schema({"k", "id"});
    std::vector<Batch> pooled_batches;
    for (int64_t b = 0; b < 6; ++b) {
      arrow::Int64Builder keys(&pooled);
      arrow::Int64Builder ids(&pooled);
      for (int64_t r = 0; r < 10; ++r) {
        ASSERT_TRUE(keys.Append(((b * 10) + r) % 7).ok());
        ASSERT_TRUE(ids.Append((b * 10) + r).ok());
      }
      auto key_column = keys.Finish();
      auto id_column = ids.Finish();
      ASSERT_TRUE(key_column.ok() && id_column.ok());
      // Part 3's batch has a schema the build does not take: its part fails (Invalid).
      pooled_batches.push_back(
          Batch{.data = BatchOf(b == 3 ? Int64Schema({"k", "other"}) : drained_schema,
                                {*key_column, *id_column}),
                .selection = nullptr});
    }
    {
      auto source = std::make_unique<ScriptedSource>(drained_schema, std::move(pooled_batches));
      source->HandOver();
      const auto pooled_build = DrainedBuild(std::move(source), {0});
      ExecContext pooling = ContextOf(executor, 3, &pooled);
      const arrow::Status failed = pooled_build->Prepare(pooling);
      EXPECT_TRUE(failed.IsInvalid()) << failed.ToString();
      EXPECT_EQ(pooled_build->table(), nullptr);
      EXPECT_EQ(pooled.bytes_allocated(), 0);
    }
  }
}

// ---- profiles ----

// A build's profile node gets its time, the rows it holds (a batch per chunk) and its metrics; a
// probe's gets find, gather, residual and, on the 1:1 path, the rows of its windows.
TEST_F(HashJoinTest, ProfilesCountBuildsAndProbes) {
  const auto pool = MakeThreadPool();
  for (arrow::internal::Executor* executor : Executors(pool.get())) {
    SCOPED_TRACE(executor == nullptr ? "one thread" : "pool");
    // Unique keys 0..11, 113 and 114 (too sparse for the direct layout); a NULL key in row 12.
    const auto build_table = KeyTable("b", 3, 5, [](int64_t i) -> std::optional<int64_t> {
      if (i == 12) {
        return std::nullopt;
      }
      return i < 12 ? i : i + 100;
    });
    ProfileNode build_node;
    const auto build = ScanBuild(build_table, {0}, &build_node);
    ExecContext ctx = ContextOf(executor);
    ASSERT_TRUE(build->Prepare(ctx).ok());
    EXPECT_EQ(build_node.instances(), 1);
    EXPECT_EQ(build_node.rows(), 14);
    EXPECT_EQ(build_node.batches(), static_cast<int64_t>(build->table()->chunks().size()));
    EXPECT_EQ(MetricOf(build_node, "parts"), 3);
    EXPECT_EQ(MetricOf(build_node, "null_keys"), 1);
    EXPECT_EQ(MetricOf(build_node, "unique"), 1);
    EXPECT_EQ(MetricOf(build_node, "direct"), 0);  // 113 and 114 make the range sparse
    for (const char* name : {"part_time", "wait", "lanes_tail", "finish"}) {
      EXPECT_TRUE(MetricOf(build_node, name).has_value()) << name;
    }
    // A probe of 8 rows in windows of 3, 3 of them matching (keys 0, 1 and 2), one residual.
    ProfileNode probe_node;
    const auto probe_schema = Int64Schema({"a"});
    auto join =
        MakeJoin(SourceOf(probe_schema, {Int64s({0, 50, 51, 1, 52, 53, 54, 2})}), build, {0},
                 /*prepares=*/false, BuildSide::kRight,
                 {Condition(plan::CompareOp::kGe, 0, ColumnAt(1, LogicalType::kBigInt))});
    ASSERT_NE(join, nullptr);
    join->set_profile(&probe_node);
    auto result = Drain(*join, ctx);
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    EXPECT_EQ((*result)->num_rows(), 3);
    EXPECT_EQ(MetricOf(probe_node, "window_rows"), 8);  // windows [0, 3), [3, 6) and [6, 8)
    for (const char* name : {"find", "gather", "residual"}) {
      EXPECT_TRUE(MetricOf(probe_node, name).has_value()) << name;
    }
  }
}

// ---- semi, anti, null-aware anti and one-row joins ----

// Semi, anti and null-aware anti joins pass the probe's batches on: each window of at most
// batch_size rows is a slice of the probe batch (its buffers, at an offset), selected where the
// join keeps a row, without a selection where it keeps every row, and skipped where it keeps none;
// a row the probe batch does not select is never kept. Their output schema is the probe's own
// schema object. The same over a build of unique keys and one of repeated keys, for any batch size.
TEST_F(HashJoinTest, SelectionKindsPassProbeBatchesThrough) {
  const auto build_schema = Int64Schema({"k", "v"});
  const auto probe_schema = arrow::schema(
      {arrow::field("a", arrow::int64()), arrow::field("b", arrow::int64(), /*nullable=*/false)});
  const std::optional<int64_t> null;
  // In windows of 3 rows: keys that match, keys that do not (a NULL among them), a match and a
  // miss beside a row that is not selected, and so on.
  const auto probe =
      BatchOf(probe_schema, {Int64s({1, 2, 3, 7, 8, null, 1, 9, 2, null, 4, 5, 10, 11, 12}),
                             Int64s({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14})});
  const auto selection = Bools(
      {true, true, true, true, true, true, false, true, true, false, true, true, true, true, true});
  // The rows each kind keeps, by build keys 1, 2 and 3.
  const std::vector<std::pair<plan::JoinKind, std::set<int64_t>>> kept = {
      {plan::JoinKind::kSemi, {0, 1, 2, 8}},
      {plan::JoinKind::kAnti, {3, 4, 5, 7, 10, 11, 12, 13, 14}},
      {plan::JoinKind::kNullAwareAnti, {3, 4, 7, 10, 11, 12, 13, 14}}};
  for (const bool repeated : {false, true}) {
    const arrow::ArrayVector build =
        repeated ? arrow::ArrayVector{Int64s({1, 1, 2, 3, 3}), Int64s({10, 11, 20, 30, 31})}
                 : arrow::ArrayVector{Int64s({1, 2, 3}), Int64s({10, 20, 30})};
    for (const auto& [kind, rows_kept] : kept) {
      for (const int64_t batch_size : {1, 3, 64}) {
        SCOPED_TRACE(std::string(plan::ToString(kind)) + (repeated ? ", repeated" : ", unique") +
                     ", batch size " + std::to_string(batch_size));
        auto join = MakeJoin(
            std::make_unique<ScriptedSource>(
                probe_schema, std::vector<Batch>{Batch{.data = probe, .selection = selection}}),
            DrainedBuild(kind, SourceOf(build_schema, build), {0}), {0}, /*prepares=*/true);
        ASSERT_NE(join, nullptr);
        EXPECT_EQ(join->output_schema().get(), probe_schema.get());
        auto batches = Batches(*join, ContextOf(nullptr, batch_size));
        ASSERT_TRUE(batches.ok()) << batches.status().ToString();
        std::size_t next = 0;
        for (int64_t begin = 0; begin < probe->num_rows(); begin += batch_size) {
          const int64_t rows = std::min(batch_size, probe->num_rows() - begin);
          int64_t hits = 0;
          for (int64_t i = begin; i < begin + rows; ++i) {
            hits += rows_kept.contains(i) ? 1 : 0;
          }
          if (hits == 0) {
            continue;  // skipped
          }
          ASSERT_LT(next, batches->size());
          const Batch& out = (*batches)[next++];
          ASSERT_EQ(out.data->num_rows(), rows);
          for (int c = 0; c < 2; ++c) {  // the probe's buffers, sliced
            EXPECT_EQ(out.data->column(c)->data()->buffers[1]->data(),
                      probe->column(c)->data()->buffers[1]->data());
            EXPECT_EQ(out.data->column(c)->offset(), begin);
          }
          if (hits == rows) {
            EXPECT_EQ(out.selection, nullptr);
            continue;
          }
          ASSERT_NE(out.selection, nullptr);
          for (int64_t i = 0; i < rows; ++i) {
            EXPECT_EQ(out.selection->Value(i), rows_kept.contains(begin + i)) << begin + i;
          }
        }
        EXPECT_EQ(next, batches->size());
      }
    }
  }
}

// The NULL keys of every kind. A probe row with a NULL key matches nothing: inner and semi joins
// drop it, an anti join keeps it. A null-aware anti join keeps no row when its build input has a
// NULL key (a row the build input does not select is none of its rows), drops the NULL-key row
// over a build with rows, and keeps every row over an empty build input. A row the probe does not
// select is never kept. Inner and semi joins over a build without rows never open their probe
// input; every other join reads it to its end, also one that keeps no row: an error of its last
// batch is the join's.
TEST_F(HashJoinTest, NullKeyMatrixOfEveryKind) {
  const auto schema = Int64Schema({"k", "id"});
  const std::optional<int64_t> null;
  // Probe ids 0 to 4 in two batches, keys 1, NULL and 2, then 3 and 2, the last one not selected.
  const std::vector<Batch> probe = {
      Batch{.data = BatchOf(schema, {Int64s({1, null, 2}), Int64s({0, 1, 2})}),
            .selection = nullptr},
      Batch{.data = BatchOf(schema, {Int64s({3, 2}), Int64s({3, 4})}),
            .selection = Bools({true, false})}};
  const auto batch = [&](const std::vector<std::optional<int64_t>>& keys,
                         std::shared_ptr<arrow::BooleanArray> selection = nullptr) {
    const std::vector<std::optional<int64_t>> ids(keys.size(), 100);
    return Batch{.data = BatchOf(schema, {Int64s(keys), Int64s(ids)}),
                 .selection = std::move(selection)};
  };
  const std::vector<std::pair<std::string, std::vector<Batch>>> builds = {
      {"2, 2, 3", {batch({2, 2, 3})}},
      {"2, NULL", {batch({2, null})}},
      {"NULL, NULL", {batch({null, null})}},
      {"empty", {}},
      {"2, 3, an unselected NULL", {batch({2, 3, null}, Bools({true, true, false}))}}};
  struct Expected {
    std::vector<std::optional<int64_t>> ids;  // the probe ids of the output rows
    int opens = 1;                            // of the probe input
  };
  const std::array<plan::JoinKind, 4> kinds = {plan::JoinKind::kInner, plan::JoinKind::kSemi,
                                               plan::JoinKind::kAnti,
                                               plan::JoinKind::kNullAwareAnti};
  // Per build, per kind in the order of `kinds`.
  const std::vector<std::array<Expected, 4>> expected = {
      {{{.ids = {2, 2, 3}}, {.ids = {2, 3}}, {.ids = {0, 1}}, {.ids = {0}}}},
      {{{.ids = {2}}, {.ids = {2}}, {.ids = {0, 1, 3}}, {.ids = {}}}},
      {{{.ids = {}, .opens = 0}, {.ids = {}, .opens = 0}, {.ids = {0, 1, 2, 3}}, {.ids = {}}}},
      {{{.ids = {}, .opens = 0},
        {.ids = {}, .opens = 0},
        {.ids = {0, 1, 2, 3}},
        {.ids = {0, 1, 2, 3}}}},
      {{{.ids = {2, 3}}, {.ids = {2, 3}}, {.ids = {0, 1}}, {.ids = {0}}}}};
  for (std::size_t b = 0; b < builds.size(); ++b) {
    for (std::size_t k = 0; k < kinds.size(); ++k) {
      // Then with the probe's second batch failing.
      for (const bool failing : {false, true}) {
        SCOPED_TRACE(std::string(plan::ToString(kinds[k])) + " over " + builds[b].first +
                     (failing ? ", the probe failing" : ""));
        auto source = std::make_unique<ScriptedSource>(schema, probe);
        if (failing) {
          source->FailAt(1);
        }
        const ScriptedSource& probed = *source;
        auto join = MakeJoin(
            std::move(source),
            DrainedBuild(kinds[k], std::make_unique<ScriptedSource>(schema, builds[b].second), {0}),
            {0}, /*prepares=*/true);
        ASSERT_NE(join, nullptr);
        ExecContext ctx = ContextOf(nullptr, 2);
        auto result = Drain(*join, ctx);
        const int opens = expected[b][k].opens;
        EXPECT_EQ(probed.opens(), opens);
        if (failing && opens == 1) {
          EXPECT_TRUE(result.status().IsIOError()) << result.status().ToString();
          EXPECT_EQ(probed.pulls(), 2);
          continue;
        }
        ASSERT_TRUE(result.ok()) << result.status().ToString();
        EXPECT_EQ(Int64Column(**result, 1), expected[b][k].ids);
        EXPECT_EQ(probed.pulls(), opens * 3);  // both batches and the end, or nothing
      }
    }
  }
  // The unselected NULL key is no NULL of the build input.
  const auto build = DrainedBuild(plan::JoinKind::kNullAwareAnti,
                                  std::make_unique<ScriptedSource>(schema, builds[4].second), {0});
  ExecContext ctx = ContextOf(nullptr);
  ASSERT_TRUE(build->Prepare(ctx).ok());
  EXPECT_FALSE(build->table()->has_null());
  EXPECT_EQ(build->table()->input_rows(), 2);
  build->Release();
}

// A join whose build empties it, an inner or a semi join over a build without rows, ends the chain
// below it: the inner join's build below it is never prepared, and the probe table is never
// scanned. Any other kind reads on, its build prepared first: an anti join over no build row and a
// null-aware anti join over an empty build input keep every row of the join below, and a null-aware
// anti join over a build input with a NULL key keeps none, but reads them all.
TEST_F(HashJoinTest, EmptyBuildsDecidePerKind) {
  const auto pool = MakeThreadPool();
  const auto key = [](int64_t i) -> std::optional<int64_t> { return i % 4; };
  const auto schema = Int64Schema({"k", "id"});
  const std::optional<int64_t> null;
  struct Case {
    plan::JoinKind kind;
    std::vector<std::optional<int64_t>> keys;  // of the build input; none: no batch
    bool empties = false;                      // the join's build empties it
    bool keeps_all = false;                    // else: every row of the join below, or none
  };
  const std::vector<Case> cases = {
      {.kind = plan::JoinKind::kInner, .keys = {}, .empties = true},
      {.kind = plan::JoinKind::kInner, .keys = {null}, .empties = true},
      {.kind = plan::JoinKind::kSemi, .keys = {}, .empties = true},
      {.kind = plan::JoinKind::kSemi, .keys = {null}, .empties = true},
      {.kind = plan::JoinKind::kAnti, .keys = {}, .keeps_all = true},
      {.kind = plan::JoinKind::kAnti, .keys = {null}, .keeps_all = true},
      {.kind = plan::JoinKind::kNullAwareAnti, .keys = {}, .keeps_all = true},
      {.kind = plan::JoinKind::kNullAwareAnti, .keys = {null}},
  };
  for (arrow::internal::Executor* executor : Executors(pool.get())) {
    for (const Case& c : cases) {
      SCOPED_TRACE(std::string(plan::ToString(c.kind)) +
                   (c.keys.empty() ? " over no input" : " over NULL") +
                   (executor == nullptr ? ", one thread" : ", pool"));
      const auto probe_table = KeyTable("p", 4, 5, key);
      const auto inner_table = KeyTable("c", 2, 3, key);
      const auto inner = ScanBuild(inner_table, {0});
      std::vector<Batch> batches;
      if (!c.keys.empty()) {
        batches.push_back(
            Batch{.data = BatchOf(schema, {Int64s(c.keys), Int64s({0})}), .selection = nullptr});
      }
      const auto outer =
          DrainedBuild(c.kind, std::make_unique<ScriptedSource>(schema, std::move(batches)), {0});
      // The probe joins `inner` on pk = ck, then `outer` on ck.
      const PartPipeline chain = [probe_table, inner,
                                  outer](int64_t part) -> arrow::Result<std::unique_ptr<Operator>> {
        ARROW_ASSIGN_OR_RAISE(std::unique_ptr<HashJoinOperator> first,
                              HashJoinOperator::Make(std::make_unique<TableScanOperator>(
                                                         probe_table, std::vector<int>{0, 1}, part),
                                                     inner, {0}, BuildSide::kRight, {}, false));
        ARROW_ASSIGN_OR_RAISE(
            std::unique_ptr<HashJoinOperator> second,
            HashJoinOperator::Make(std::move(first), outer, {2}, BuildSide::kRight, {}, false));
        return second;
      };
      auto result = RunUnion(chain, probe_table->num_parts(), {outer, inner}, ContextOf(executor));
      ASSERT_TRUE(result.ok()) << result.status().ToString();
      EXPECT_EQ(inner->table(), nullptr);  // released
      if (c.empties) {
        EXPECT_EQ((*result)->num_rows(), 0);
        EXPECT_TRUE(inner_table->scanned_parts().empty());
        EXPECT_TRUE(probe_table->scanned_parts().empty());
        continue;
      }
      EXPECT_EQ(inner_table->scanned_parts(), (std::vector<int64_t>{0, 1}));
      EXPECT_EQ(probe_table->scanned_parts(), (std::vector<int64_t>{0, 1, 2, 3}));
      const Rows below = ReferenceJoin(RowsOf(*probe_table), RowsOf(*inner_table), {0}, {0});
      ASSERT_FALSE(below.empty());
      EXPECT_EQ(RowsOf(**result), c.keeps_all ? below : Rows{});
    }
  }
}

// A one-row join appends its build's single row to every probe row: here an aggregate over an
// empty input, whose SUM of an INTEGER is a NULL HUGEINT, SUM of a DECIMAL(5,2) a NULL
// DECIMAL(38,2), AVG a NULL DOUBLE, MIN of a VARCHAR and MAX of a DATE NULL of their types, and
// COUNT(*) 0, in nullable fields. The probe's columns are slices of its batches, and its selection
// is kept. Over a serial probe input and in a part pipeline, on one thread and on the pool, the
// build's input is drained once.
TEST_F(HashJoinTest, OneRowJoinAppendsAnAggregateOverEmptyInput) {
  const auto pool = MakeThreadPool();
  const auto input_schema =
      arrow::schema({arrow::field("i", arrow::int32()), arrow::field("d", arrow::decimal128(5, 2)),
                     arrow::field("s", arrow::binary()), arrow::field("t", arrow::date32())});
  const auto i = Column(0, "i", LogicalType::kInteger);
  const std::vector<plan::AggregateCall> calls = {
      {.kind = plan::AggKind::kSum, .arg = i, .type = LogicalType::kHugeInt},
      {.kind = plan::AggKind::kSum,
       .arg = Column(1, "d", LogicalType::Decimal(5, 2)),
       .type = LogicalType::Decimal(38, 2)},
      {.kind = plan::AggKind::kAvg, .arg = i, .type = LogicalType::kDouble},
      {.kind = plan::AggKind::kMin,
       .arg = Column(2, "s", LogicalType::kVarchar),
       .type = LogicalType::kVarchar},
      {.kind = plan::AggKind::kMax,
       .arg = Column(3, "t", LogicalType::kDate),
       .type = LogicalType::kDate},
      {.kind = plan::AggKind::kCountStar, .arg = {}, .type = LogicalType::kBigInt}};
  const arrow::DataTypeVector types = {arrow::decimal128(38, 0), arrow::decimal128(38, 2),
                                       arrow::float64(),         arrow::binary(),
                                       arrow::date32(),          arrow::int64()};
  const std::vector<std::string> values = {"null", "null", "null", "null", "null", "0"};
  // The build of the aggregate of an empty input; `drained` is the input.
  const auto build_of = [&](const ScriptedSource** drained) {
    auto empty = std::make_unique<ScriptedSource>(input_schema, std::vector<Batch>{});
    *drained = empty.get();
    return DrainedBuild(plan::JoinKind::kOneRow,
                        std::make_unique<ScalarAggregateOperator>(std::move(empty), calls), {});
  };
  const auto expect_types = [&](const arrow::Schema& schema) {
    ASSERT_EQ(schema.num_fields(), 8);
    for (std::size_t c = 0; c < types.size(); ++c) {
      const auto& field = schema.field(2 + static_cast<int>(c));
      EXPECT_TRUE(field->type()->Equals(*types[c])) << field->type()->ToString();
      EXPECT_TRUE(field->nullable());
    }
  };
  {
    // Over a serial input, in windows of 2 rows: rows 0 and 1, 2 and 3, then 4, which is skipped.
    const auto probe_schema = Int64Schema({"a", "b"});
    const auto probe =
        BatchOf(probe_schema, {Int64s({1, 2, 3, 4, 5}), Int64s({10, 20, 30, 40, 50})});
    const ScriptedSource* drained = nullptr;
    auto join =
        MakeJoin(std::make_unique<ScriptedSource>(
                     probe_schema,
                     std::vector<Batch>{Batch{
                         .data = probe, .selection = Bools({true, false, true, true, false})}}),
                 build_of(&drained), {}, /*prepares=*/true);
    ASSERT_NE(join, nullptr);
    expect_types(*join->output_schema());
    auto batches = Batches(*join, ContextOf(nullptr, 2));
    ASSERT_TRUE(batches.ok()) << batches.status().ToString();
    ASSERT_EQ(batches->size(), 2U);
    for (std::size_t w = 0; w < 2; ++w) {
      const Batch& out = (*batches)[w];
      EXPECT_EQ(out.data->column(0)->data()->buffers[1]->data(),
                probe->column(0)->data()->buffers[1]->data());
      EXPECT_EQ(out.data->column(0)->offset(), static_cast<int64_t>(2 * w));
    }
    ASSERT_NE((*batches)[0].selection, nullptr);
    EXPECT_TRUE((*batches)[0].selection->Value(0));
    EXPECT_FALSE((*batches)[0].selection->Value(1));
    EXPECT_EQ((*batches)[1].selection, nullptr);
    EXPECT_EQ(RowsOf(join->output_schema(), *batches),
              ReferenceOneRow(Rows{{"1", "10"}, {"3", "30"}, {"4", "40"}}, values));
    EXPECT_EQ(drained->opens(), 1);
  }
  for (arrow::internal::Executor* executor : Executors(pool.get())) {
    SCOPED_TRACE(executor == nullptr ? "one thread" : "pool");
    // In a part pipeline over a Filter (a selection) of 3 parts: pk > 2.
    const auto table =
        KeyTable("p", 3, 4, [](int64_t row) -> std::optional<int64_t> { return row; });
    const ScriptedSource* drained = nullptr;
    const auto build = build_of(&drained);
    const PartPipeline pipeline =
        [table, build](int64_t part) -> arrow::Result<std::unique_ptr<Operator>> {
      ARROW_ASSIGN_OR_RAISE(
          std::unique_ptr<HashJoinOperator> join,
          HashJoinOperator::Make(
              std::make_unique<FilterOperator>(
                  std::make_unique<TableScanOperator>(table, std::vector<int>{0, 1}, part),
                  std::vector<plan::Predicate>{
                      testing::Compare(Column(0, "pk", LogicalType::kBigInt), plan::CompareOp::kGt,
                                       testing::BigInt(2))}),
              build, {}, BuildSide::kRight, {}, false));
      return join;
    };
    expect_types(*SchemaOf(pipeline));
    auto result = RunUnion(pipeline, table->num_parts(), {build}, ContextOf(executor, 3));
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    EXPECT_EQ(build->table(), nullptr);  // released, its values with it
    EXPECT_EQ(build->values(), nullptr);
    Rows probe_rows;
    for (const std::vector<std::string>& row : RowsOf(*table)) {
      if (std::stoll(row[0]) > 2) {
        probe_rows.push_back(row);
      }
    }
    EXPECT_EQ(RowsOf(**result), ReferenceOneRow(probe_rows, values));
    EXPECT_EQ(drained->opens(), 1);
  }
}

// A one-row join's build of no row or of two rows is Invalid, a planner bug (SQL plans only an
// aggregate without groups there), from Prepare: before any probe part runs or any probe input
// opens, for a drained build input and one of parts. The build then holds nothing.
TEST_F(HashJoinTest, OneRowBuildOfAnotherCountIsInvalid) {
  const auto schema = Int64Schema({"x", "y"});
  const auto row = [](int64_t i) -> std::optional<int64_t> { return i; };
  for (const int64_t rows : {0, 2}) {
    SCOPED_TRACE(rows);
    // Drained, under the probes of a part pipeline.
    std::vector<Batch> batches;
    if (rows > 0) {
      batches.push_back(
          Batch{.data = BatchOf(schema, {Int64s({7, 8}), Int64s({70, 80})}), .selection = nullptr});
    }
    const auto probe_table = KeyTable("p", 3, 2, row);
    const auto drained = DrainedBuild(plan::JoinKind::kOneRow,
                                      std::make_unique<ScriptedSource>(schema, batches), {});
    auto result = RunUnion(ProbePipeline(probe_table, drained, {}), probe_table->num_parts(),
                           {drained}, ContextOf(nullptr));
    EXPECT_TRUE(result.status().IsInvalid()) << result.status().ToString();
    EXPECT_TRUE(probe_table->scanned_parts().empty());
    EXPECT_EQ(drained->table(), nullptr);
    EXPECT_EQ(drained->values(), nullptr);
    // From the parts of a table (rows parts of one row), under a probe over a serial input.
    auto source = SourceOf(schema, {Int64s({1}), Int64s({2})});
    const ScriptedSource& probed = *source;
    const auto scanned = ScanBuild(plan::JoinKind::kOneRow, KeyTable("b", rows, 1, row), {});
    auto join = MakeJoin(std::move(source), scanned, {}, /*prepares=*/true);
    ASSERT_NE(join, nullptr);
    ExecContext ctx = ContextOf(nullptr);
    auto serial = Drain(*join, ctx);
    EXPECT_TRUE(serial.status().IsInvalid()) << serial.status().ToString();
    EXPECT_EQ(probed.opens(), 0);
    EXPECT_EQ(scanned->table(), nullptr);
  }
}

// A one-row join whose VARCHAR values are long appends them to windows of fewer rows than
// batch_size (OneRowValues::rows: at most 1 MiB of values per window, the bytes of every VARCHAR
// column counted, one row at least). Every batch out is valid: its value columns are as long as
// its probe columns, slices of the one buffer the build made. With a probe selection and without,
// every selected probe row comes out once, in order, with the values.
TEST_F(HashJoinTest, OneRowWindowsOfLongValuesHoldFewerRows) {
  const auto probe_schema = Int64Schema({"a", "b"});
  const std::optional<int64_t> null;
  const auto probe = BatchOf(probe_schema, {Int64s({0, 1, 2, 3, 4, 5, 6, 7, 8, 9}),
                                            Int64s({10, 11, 12, 13, 14, 15, 16, 17, 18, 19})});
  // In windows of 3 rows: rows 0 and 2, none, all three, none.
  const auto selection = Bools({true, false, true, false, false, false, true, true, true, false});
  struct Case {
    std::size_t bytes;             // of each VARCHAR value
    int values;                    // the VARCHAR columns, before a NULL BIGINT one
    int64_t rows;                  // of a window at batch_size 8
    std::size_t windows;           // the batches out without a selection
    std::size_t selected_windows;  // and with it
  };
  // 1 MiB holds one value of 300 KiB 3 times, two of them once, and one of 1.5 MiB not once (a
  // window still has a row).
  for (const Case& c : {Case{.bytes = std::size_t{300} * 1024,
                             .values = 1,
                             .rows = 3,
                             .windows = 4,
                             .selected_windows = 2},
                        Case{.bytes = std::size_t{300} * 1024,
                             .values = 2,
                             .rows = 1,
                             .windows = 10,
                             .selected_windows = 5},
                        Case{.bytes = std::size_t{1536} * 1024,
                             .values = 1,
                             .rows = 1,
                             .windows = 10,
                             .selected_windows = 5}}) {
    const std::string value(c.bytes, 'v');
    arrow::FieldVector build_fields;
    arrow::ArrayVector build_columns;
    for (int v = 0; v < c.values; ++v) {
      build_fields.push_back(arrow::field("l" + std::to_string(v), arrow::binary()));
      build_columns.push_back(Strings({value}));
    }
    build_fields.push_back(arrow::field("n", arrow::int64()));
    build_columns.push_back(Int64s({null}));
    const auto build_schema = arrow::schema(build_fields);
    for (const bool selected : {false, true}) {
      SCOPED_TRACE(std::to_string(c.values) + " values of " + std::to_string(c.bytes) + " bytes" +
                   (selected ? ", selected" : ""));
      auto join = MakeJoin(
          std::make_unique<ScriptedSource>(
              probe_schema, std::vector<Batch>{Batch{.data = probe,
                                                     .selection = selected ? selection : nullptr}}),
          DrainedBuild(plan::JoinKind::kOneRow, SourceOf(build_schema, build_columns), {}), {},
          /*prepares=*/true);
      ASSERT_NE(join, nullptr);
      auto batches = Batches(*join, ContextOf(nullptr, 8));
      ASSERT_TRUE(batches.ok()) << batches.status().ToString();
      ASSERT_EQ(batches->size(), selected ? c.selected_windows : c.windows);
      std::vector<int64_t> ids;  // the probe's a of the rows out
      for (const Batch& out : *batches) {
        const arrow::Status valid = out.data->ValidateFull();
        ASSERT_TRUE(valid.ok()) << valid.ToString();
        EXPECT_LE(out.data->num_rows(), c.rows);
        for (int v = 2; v < 2 + c.values; ++v) {
          EXPECT_EQ(out.data->column(v)->data()->buffers[2]->data(),
                    batches->front().data->column(v)->data()->buffers[2]->data());
        }
        auto rows = Materialize(out, arrow::default_memory_pool());
        ASSERT_TRUE(rows.ok()) << rows.status().ToString();
        const auto& a = static_cast<const arrow::Int64Array&>(*(*rows)->column(0));
        for (int64_t r = 0; r < (*rows)->num_rows(); ++r) {
          ids.push_back(a.Value(r));
          for (int v = 2; v < 2 + c.values; ++v) {
            EXPECT_TRUE(static_cast<const arrow::BinaryArray&>(*(*rows)->column(v)).GetView(r) ==
                        value)
                << a.Value(r);
          }
          EXPECT_TRUE((*rows)->column(2 + c.values)->IsNull(r)) << a.Value(r);
        }
      }
      const std::vector<int64_t> expected =
          selected ? std::vector<int64_t>{0, 2, 6, 7, 8}
                   : std::vector<int64_t>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
      EXPECT_EQ(ids, expected);
    }
  }
}

// A one-row join's values, from the budget's pool, are held while its probes run, and go with its
// build's table: once the sink's parts are done (a part pipeline), or at the end of the input of a
// probe that prepares its build, which lets go of them too; both before Close. Every batch out is
// dropped as it comes, so nothing else holds the values. On one thread and on the pool.
TEST_F(HashJoinTest, OneRowValuesGoWithTheirBuild) {
  const auto pool = MakeThreadPool();
  const auto build_schema =
      arrow::schema({arrow::field("s", arrow::binary()), arrow::field("n", arrow::int64())});
  const auto table = KeyTable("p", 3, 4, [](int64_t i) -> std::optional<int64_t> { return i; });
  // Pulls `op` to its end, dropping every batch: the rows they selected.
  const auto rest = [](Operator& op) -> arrow::Result<int64_t> {
    int64_t rows = 0;
    while (true) {
      ARROW_ASSIGN_OR_RAISE(const Batch batch, op.Next());
      if (batch.end()) {
        return rows;
      }
      rows += batch.selected_rows();
    }
  };
  for (arrow::internal::Executor* executor : Executors(pool.get())) {
    for (const bool in_parts : {true, false}) {
      SCOPED_TRACE(std::string(in_parts ? "part pipeline" : "serial probe") +
                   (executor == nullptr ? ", one thread" : ", pool"));
      MemoryBudget budget(std::nullopt);
      ExecContext ctx = ContextOf(executor, 3, &budget);
      const auto build = DrainedBuild(
          plan::JoinKind::kOneRow, SourceOf(build_schema, {Strings({"value"}), Int64s({7})}), {});
      std::unique_ptr<Operator> op;
      if (in_parts) {
        const PartPipeline pipeline = ProbePipeline(table, build, {});
        op = std::make_unique<BuildsFirstOperator>(
            std::make_unique<PartUnionOperator>(pipeline, table->num_parts(), SchemaOf(pipeline),
                                                std::nullopt),
            std::vector<std::shared_ptr<JoinBuild>>{build});
      } else {
        op = MakeJoin(SourceOf(Int64Schema({"a"}), {Int64s({1, 2, 3, 4, 5})}), build, {},
                      /*prepares=*/true);
      }
      ASSERT_NE(op, nullptr);
      ASSERT_TRUE(op->Open(ctx).ok());
      int64_t rows = 0;
      {
        auto first = op->Next();
        ASSERT_TRUE(first.ok() && !first->end());
        rows += first->selected_rows();
        EXPECT_NE(build->values(), nullptr);
      }
      auto more = rest(*op);
      ASSERT_TRUE(more.ok()) << more.status().ToString();
      EXPECT_EQ(rows + *more, in_parts ? 12 : 5);
      EXPECT_EQ(build->values(), nullptr);
      EXPECT_EQ(budget.bytes_allocated(), 0);
      EXPECT_TRUE(op->Close().ok());
    }
  }
}

// Joins of every kind chain in one probe pipeline (the outermost build prepared first), and a semi
// join's pipeline is the input of an inner join's build, which takes its batches (they keep the
// probe's schema): both give the nested-loop reference, on one thread and on the pool.
TEST_F(HashJoinTest, NestedBuildsAndChainsOfEveryKind) {
  const auto pool = MakeThreadPool();
  const auto t1 = KeyTable("a", 4, 5, [](int64_t i) { return i % 6; });
  const auto t2 = KeyTable("b", 3, 4, [](int64_t i) { return i % 7; });
  const auto t3 = KeyTable("c", 2, 3, [](int64_t i) { return i % 5; });
  const std::vector<plan::AggregateCall> count_star = {
      {.kind = plan::AggKind::kCountStar, .arg = {}, .type = LogicalType::kBigInt}};
  for (arrow::internal::Executor* executor : Executors(pool.get())) {
    SCOPED_TRACE(executor == nullptr ? "one thread" : "pool");
    ExecContext ctx = ContextOf(executor);
    {
      // t1 semi t2 on ak = bk, anti t3 on aid = ck, then the row of COUNT(*) over t3.
      const auto semi = ScanBuild(plan::JoinKind::kSemi, t2, {0});
      const auto anti = ScanBuild(plan::JoinKind::kAnti, t3, {0});
      const auto one_row = DrainedBuild(
          plan::JoinKind::kOneRow,
          std::make_unique<ScalarAggregateOperator>(
              std::make_unique<TableScanOperator>(t3, std::vector<int>{0, 1}, std::nullopt),
              count_star),
          {});
      const PartPipeline chain =
          [t1, semi, anti, one_row](int64_t part) -> arrow::Result<std::unique_ptr<Operator>> {
        ARROW_ASSIGN_OR_RAISE(std::unique_ptr<HashJoinOperator> first,
                              HashJoinOperator::Make(std::make_unique<TableScanOperator>(
                                                         t1, std::vector<int>{0, 1}, part),
                                                     semi, {0}, BuildSide::kRight, {}, false));
        ARROW_ASSIGN_OR_RAISE(
            std::unique_ptr<HashJoinOperator> second,
            HashJoinOperator::Make(std::move(first), anti, {1}, BuildSide::kRight, {}, false));
        ARROW_ASSIGN_OR_RAISE(
            std::unique_ptr<HashJoinOperator> third,
            HashJoinOperator::Make(std::move(second), one_row, {}, BuildSide::kRight, {}, false));
        return third;
      };
      const Rows expected = ReferenceOneRow(
          ReferenceFilter(
              plan::JoinKind::kAnti,
              ReferenceFilter(plan::JoinKind::kSemi, RowsOf(*t1), RowsOf(*t2), {0}, {0}),
              RowsOf(*t3), {1}, {0}),
          {"6"});
      ASSERT_FALSE(expected.empty());
      auto result = RunUnion(chain, t1->num_parts(), {one_row, anti, semi}, ctx);
      ASSERT_TRUE(result.ok()) << result.status().ToString();
      EXPECT_EQ(RowsOf(**result), expected);
    }
    {
      // t1 joins (t2 semi t3 on bid = ck) on ak = bk.
      const auto semi = ScanBuild(plan::JoinKind::kSemi, t3, {0});
      const PartPipeline middle = ProbePipeline(t2, semi, {1});
      const auto outer = std::make_shared<JoinBuild>(
          SpecOf(SchemaOf(middle), {Column(0, "bk", LogicalType::kBigInt)}), middle,
          t2->num_parts(), std::vector<std::shared_ptr<JoinBuild>>{semi}, nullptr,
          plan::JoinKind::kInner);
      const Rows expected = ReferenceJoin(
          RowsOf(*t1), ReferenceFilter(plan::JoinKind::kSemi, RowsOf(*t2), RowsOf(*t3), {1}, {0}),
          {0}, {0});
      ASSERT_FALSE(expected.empty());
      auto result = RunUnion(ProbePipeline(t1, outer, {0}), t1->num_parts(), {outer}, ctx);
      ASSERT_TRUE(result.ok()) << result.status().ToString();
      EXPECT_EQ(RowsOf(**result), expected);
      EXPECT_EQ(semi->table(), nullptr);
    }
  }
}

// A semi, anti or null-aware anti probe adds find when it looks keys up, and nothing else; when it
// keeps every row or none without a lookup (an anti join over no build row, a null-aware anti join
// over an empty build input or one with a NULL key), not even find. A one-row probe adds gather,
// never find, and its build's finish makes its values. window_rows is the inner join's own.
TEST_F(HashJoinTest, ProfilesOfEveryKind) {
  const auto schema = Int64Schema({"k", "v"});
  const std::optional<int64_t> null;
  struct Case {
    plan::JoinKind kind;
    std::vector<std::optional<int64_t>> build;  // the keys of the build input
    bool find = false;
    int64_t rows = 0;  // of the probe's 4
  };
  const std::vector<Case> cases = {
      {.kind = plan::JoinKind::kSemi, .build = {1, 2}, .find = true, .rows = 2},
      {.kind = plan::JoinKind::kAnti, .build = {1, 2}, .find = true, .rows = 2},
      {.kind = plan::JoinKind::kAnti, .build = {null}, .rows = 4},
      {.kind = plan::JoinKind::kNullAwareAnti, .build = {1, 2}, .find = true, .rows = 1},
      {.kind = plan::JoinKind::kNullAwareAnti, .build = {}, .rows = 4},
      {.kind = plan::JoinKind::kNullAwareAnti, .build = {1, null}, .rows = 0},
      {.kind = plan::JoinKind::kOneRow, .build = {5}, .rows = 4},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(std::string(plan::ToString(c.kind)) + " over " + std::to_string(c.build.size()) +
                 " rows");
    const bool one_row = c.kind == plan::JoinKind::kOneRow;
    std::vector<Batch> batches;
    if (!c.build.empty()) {
      const std::vector<std::optional<int64_t>> values(c.build.size(), 7);
      batches.push_back(
          Batch{.data = BatchOf(schema, {Int64s(c.build), Int64s(values)}), .selection = nullptr});
    }
    ProfileNode build_node;
    ProfileNode probe_node;
    const std::vector<int> keys = one_row ? std::vector<int>{} : std::vector<int>{0};
    auto join = MakeJoin(
        SourceOf(schema, {Int64s({1, 2, 3, null}), Int64s({0, 1, 2, 3})}),
        DrainedBuild(c.kind, std::make_unique<ScriptedSource>(schema, batches), keys, &build_node),
        keys, /*prepares=*/true);
    ASSERT_NE(join, nullptr);
    join->set_profile(&probe_node);
    ExecContext ctx = ContextOf(nullptr);
    auto result = Drain(*join, ctx);
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    EXPECT_EQ((*result)->num_rows(), c.rows);
    EXPECT_EQ(MetricOf(probe_node, "find").has_value(), c.find);
    EXPECT_EQ(MetricOf(probe_node, "gather").has_value(), one_row);
    EXPECT_FALSE(MetricOf(probe_node, "window_rows").has_value());
    EXPECT_FALSE(MetricOf(probe_node, "residual").has_value());
    EXPECT_TRUE(MetricOf(build_node, "finish").has_value());
    if (one_row) {
      EXPECT_EQ(build_node.rows(), 1);
    }
  }
}

// ---- plans (HashJoinPlanTest): joins through the physical planner ----

class HashJoinPlanTest : public testing::ExecTest {};

plan::LogicalNodePtr Node(plan::LogicalNode node) {
  return std::make_shared<const plan::LogicalNode>(std::move(node));
}

// A table of `parts` parts (a batch each, of `rows` rows), i numbering the rows in part order:
// <prefix>k BIGINT key(i), <prefix>s VARCHAR (NULL if i % 9 == 4, else "x", "" or "y" by i % 3)
// and <prefix>id BIGINT i.
std::shared_ptr<MemoryTable> MixedTable(const std::string& prefix, int64_t parts, int64_t rows,
                                        const std::function<std::optional<int64_t>(int64_t)>& key) {
  static constexpr std::array<const char*, 3> kTexts = {"x", "", "y"};
  const auto schema = arrow::schema({arrow::field(prefix + "k", arrow::int64()),
                                     arrow::field(prefix + "s", arrow::binary()),
                                     arrow::field(prefix + "id", arrow::int64())});
  arrow::RecordBatchVector batches;
  for (int64_t part = 0; part < parts; ++part) {
    std::vector<std::optional<int64_t>> keys;
    std::vector<std::optional<std::string>> texts;
    std::vector<std::optional<int64_t>> ids;
    for (int64_t r = 0; r < rows; ++r) {
      const int64_t i = (part * rows) + r;
      keys.push_back(key(i));
      texts.push_back(i % 9 == 4
                          ? std::nullopt
                          : std::optional<std::string>(kTexts[static_cast<std::size_t>(i % 3)]));
      ids.emplace_back(i);
    }
    batches.push_back(BatchOf(schema, {Int64s(keys), Strings(texts), Int64s(ids)}));
  }
  return std::make_shared<MemoryTable>(schema, std::move(batches), /*split=*/true);
}

// Column `index` of a MixedTable's rows read at `offset` in a join's output: <prefix>k and
// <prefix>id BIGINT, <prefix>s VARCHAR.
plan::BoundColumn MixedColumn(const std::string& prefix, int index, int offset = 0) {
  static constexpr std::array<const char*, 3> kNames = {"k", "s", "id"};
  return Column(offset + index, prefix + kNames[static_cast<std::size_t>(index)],
                index == 1 ? LogicalType::kVarchar : LogicalType::kBigInt);
}

// The probe keys i % 13, NULL where i % 11 == 5.
std::optional<int64_t> ProbeKey(int64_t i) {
  if (i % 11 == 5) {
    return std::nullopt;
  }
  return i % 13;
}

plan::LogicalNodePtr ScanOf(const std::shared_ptr<plan::Table>& table, const std::string& name) {
  return Node(plan::ScanNode{.table = table, .table_name = name, .fields = AllFields(*table)});
}

// The inner join of `probe` and `build` where probe_keys[k] (a column of the probe's output) equals
// build_keys[k] (of the build's), building on `build_side`: the build is the left input with kLeft.
plan::LogicalNodePtr JoinOf(const plan::LogicalNodePtr& probe, const plan::LogicalNodePtr& build,
                            const std::vector<plan::BoundColumn>& probe_keys,
                            const std::vector<plan::BoundColumn>& build_keys,
                            BuildSide build_side = BuildSide::kRight,
                            std::vector<plan::ExprPtr> residual = {}) {
  const bool left = build_side == BuildSide::kLeft;
  plan::JoinNode join{.kind = plan::JoinKind::kInner,
                      .left = left ? build : probe,
                      .right = left ? probe : build,
                      .keys = {},
                      .residual = std::move(residual),
                      .build = build_side,
                      .span = {}};
  for (std::size_t k = 0; k < probe_keys.size(); ++k) {
    join.keys.push_back(plan::JoinKey{.left = left ? build_keys[k] : probe_keys[k],
                                      .right = left ? probe_keys[k] : build_keys[k]});
  }
  return Node(std::move(join));
}

// The join of `kind` of `probe` (left) and `build` (right) where probe_keys[k] (a column of the
// probe's output) equals build_keys[k] (of the build's); no keys for a one-row join.
plan::LogicalNodePtr JoinOf(plan::JoinKind kind, const plan::LogicalNodePtr& probe,
                            const plan::LogicalNodePtr& build,
                            const std::vector<plan::BoundColumn>& probe_keys,
                            const std::vector<plan::BoundColumn>& build_keys) {
  plan::JoinNode join{.kind = kind,
                      .left = probe,
                      .right = build,
                      .keys = {},
                      .residual = {},
                      .build = BuildSide::kRight,
                      .span = {}};
  for (std::size_t k = 0; k < probe_keys.size(); ++k) {
    join.keys.push_back(plan::JoinKey{.left = probe_keys[k], .right = build_keys[k]});
  }
  return Node(std::move(join));
}

// A plan of `root`, whose output has `width` columns.
plan::LogicalPlan PlanOf(plan::LogicalNodePtr root, std::size_t width) {
  plan::LogicalPlan plan{.root = std::move(root), .output = {}};
  for (std::size_t i = 0; i < width; ++i) {
    plan.output.push_back({.name = "c" + std::to_string(i), .type = LogicalType::kBigInt});
  }
  return plan;
}

// The rows of `plan`, run with `ctx` and profiled into `profile` (nullptr: not profiled).
arrow::Result<std::shared_ptr<arrow::Table>> RunPlan(const plan::LogicalPlan& plan, ExecContext ctx,
                                                     ProfileNode* profile = nullptr) {
  ARROW_ASSIGN_OR_RAISE(std::unique_ptr<Operator> op, BuildPhysicalPlan(plan, profile));
  return Drain(*op, ctx);
}

// The names of the tables whose parts were scanned, in scan order (from any thread).
class ScanLog {
 public:
  void Add(const std::string& name) {
    const std::scoped_lock lock(mutex_);
    names_.push_back(name);
  }
  [[nodiscard]] std::vector<std::string> names() const {
    const std::scoped_lock lock(mutex_);
    return names_;
  }
  void Clear() {
    const std::scoped_lock lock(mutex_);
    names_.clear();
  }

 private:
  mutable std::mutex mutex_;
  std::vector<std::string> names_;
};

// A MemoryTable that logs its name at every part scanned: the order in which a plan reads the
// parts of several tables.
class LoggedTable final : public plan::Table {
 public:
  LoggedTable(std::shared_ptr<MemoryTable> table, std::string name, std::shared_ptr<ScanLog> log)
      : table_(std::move(table)), name_(std::move(name)), log_(std::move(log)) {}

  const std::shared_ptr<arrow::Schema>& schema() const override { return table_->schema(); }
  std::optional<int64_t> exact_row_count() const override { return table_->exact_row_count(); }
  int64_t num_parts() const override { return table_->num_parts(); }
  std::optional<int64_t> part_rows(int64_t part) const override { return table_->part_rows(part); }
  std::string Describe() const override { return "logged"; }

 protected:
  arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> DoScan(
      const std::vector<int>& fields, int64_t batch_size, arrow::MemoryPool* pool) const override {
    return table_->Scan(fields, batch_size, pool);
  }
  arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> DoScanPart(
      int64_t part, const std::vector<int>& fields, int64_t batch_size,
      arrow::MemoryPool* pool) const override {
    log_->Add(name_);
    return table_->ScanPart(part, fields, batch_size, pool);
  }

 private:
  std::shared_ptr<MemoryTable> table_;
  std::string name_;
  std::shared_ptr<ScanLog> log_;
};

// `name` repeated `count` times.
std::vector<std::string> Repeated(const std::string& name, std::size_t count) {
  return std::vector<std::string>(count, name);
}

std::vector<std::string> Concat(std::vector<std::vector<std::string>> parts) {
  std::vector<std::string> out;
  for (std::vector<std::string>& part : parts) {
    out.insert(out.end(), part.begin(), part.end());
  }
  return out;
}

// The rows of `rows` whose cell `column` is a number of at least `value`.
Rows WhereAtLeast(const Rows& rows, std::size_t column, int64_t value) {
  Rows out;
  for (const std::vector<std::string>& row : rows) {
    if (row[column] != "null" && std::stoll(row[column]) >= value) {
      out.push_back(row);
    }
  }
  return out;
}

// Through the physical planner, an inner join gives the nested-loop reference in its order: the
// probe's rows in part order, each with its matches in the build's (part, row) order. A build on
// either side, of repeated or unique keys (the 1:N and 1:1 paths), on a BIGINT, a VARCHAR ('' is
// no NULL) or a two-column key, for any batch size.
TEST_F(HashJoinPlanTest, MatchesTheNestedLoopReference) {
  const auto probe_table = MixedTable("p", 6, 7, ProbeKey);
  const Rows probe_rows = RowsOf(*probe_table);
  const std::vector<std::vector<int>> keys = {{0}, {1}, {0, 1}};
  for (const bool unique : {false, true}) {
    const auto build_table =
        unique ? MixedTable("b", 2, 6, [](int64_t i) -> std::optional<int64_t> { return i; })
               : MixedTable("b", 3, 5, [](int64_t i) -> std::optional<int64_t> { return i % 10; });
    const Rows build_rows = RowsOf(*build_table);
    for (const std::vector<int>& columns : keys) {
      std::vector<plan::BoundColumn> probe_keys;
      std::vector<plan::BoundColumn> build_keys;
      for (const int c : columns) {
        probe_keys.push_back(MixedColumn("p", c));
        build_keys.push_back(MixedColumn("b", c));
      }
      for (const BuildSide side : {BuildSide::kRight, BuildSide::kLeft}) {
        const Rows expected = ReferenceJoin(probe_rows, build_rows, columns, columns, side);
        ASSERT_FALSE(expected.empty());
        const auto plan = PlanOf(JoinOf(ScanOf(probe_table, "p"), ScanOf(build_table, "b"),
                                        probe_keys, build_keys, side),
                                 6);
        for (const int64_t batch_size : {1, 3, 64}) {
          SCOPED_TRACE(std::to_string(columns.size()) + " key(s) from column " +
                       std::to_string(columns[0]) + (unique ? ", 1:1" : ", 1:N") +
                       (side == BuildSide::kLeft ? ", build left" : ", build right") +
                       ", batch size " + std::to_string(batch_size));
          auto result = RunPlan(plan, ContextOf(nullptr, batch_size));
          ASSERT_TRUE(result.ok()) << result.status().ToString();
          EXPECT_EQ(RowsOf(**result), expected);
        }
      }
    }
  }
}

// Every shape through which a join's rows reach a result gives the same rows on one thread and on
// four: the part union at the root, a Filter and a Project above the probe, every part sink (an
// aggregate, a GROUP BY, two levels with keys and without, the COUNT(DISTINCT) rewrite, a top-N),
// a Limit, a Sort, the partition top-N, a probe over a serial input, a drained build, a chain of
// two joins in one pipeline and a build whose input probes a build of its own. The joins and the
// aggregation in two levels without keys give the nested-loop reference.
TEST_F(HashJoinPlanTest, SameResultsOnOneAndFourThreads) {
  const auto pool = MakeThreadPool();
  const auto p = MixedTable("p", 6, 7, ProbeKey);
  const auto b = MixedTable("b", 3, 5, [](int64_t i) -> std::optional<int64_t> { return i % 10; });
  const auto d = MixedTable("d", 2, 6, [](int64_t i) -> std::optional<int64_t> { return i; });
  const auto pk = MixedColumn("p", 0);
  const auto bk = MixedColumn("b", 0);
  const auto bid = MixedColumn("b", 2);
  const auto dk = MixedColumn("d", 0);
  const auto join = JoinOf(ScanOf(p, "p"), ScanOf(b, "b"), {pk}, {bk});
  // The join's output: p's k, s, id, then b's.
  const auto out_pk = MixedColumn("p", 0);
  const auto out_pid = MixedColumn("p", 2);
  const auto out_bk = MixedColumn("b", 0, 3);
  const auto out_bid = MixedColumn("b", 2, 3);
  const plan::AggregateCall count{
      .kind = plan::AggKind::kCountStar, .arg = {}, .type = LogicalType::kBigInt};
  const plan::AggregateCall min_bid{
      .kind = plan::AggKind::kMin, .arg = out_bid, .type = LogicalType::kBigInt};
  const plan::AggregateCall max_pid{
      .kind = plan::AggKind::kMax, .arg = out_pid, .type = LogicalType::kBigInt};
  const plan::AggregateCall distinct_pid{
      .kind = plan::AggKind::kCountDistinct, .arg = out_pid, .type = LogicalType::kBigInt};
  const auto sorted = Node(plan::SortNode{
      .input = join,
      .keys = {{.column = out_pid, .descending = true}, {.column = out_bid, .descending = true}}});
  const auto grouped =
      Node(plan::GroupAggregateNode{.input = join, .keys = {out_bk}, .aggregates = {count}});
  // A probe input that is no part pipeline (p's keys, grouped), and a build input that is none.
  const auto probe_groups =
      Node(plan::GroupAggregateNode{.input = ScanOf(p, "p"), .keys = {pk}, .aggregates = {count}});
  const plan::AggregateCall min_of_bid{
      .kind = plan::AggKind::kMin, .arg = bid, .type = LogicalType::kBigInt};
  const auto build_groups = Node(
      plan::GroupAggregateNode{.input = ScanOf(b, "b"), .keys = {bk}, .aggregates = {min_of_bid}});
  // p joins b, then d on pid = dk; p joins (b joins d on bid = dk) on pk = bk.
  const auto chain = JoinOf(join, ScanOf(d, "d"), {out_pid}, {dk});
  const auto nested =
      JoinOf(ScanOf(p, "p"), JoinOf(ScanOf(b, "b"), ScanOf(d, "d"), {bid}, {dk}), {pk}, {bk});
  const std::vector<std::pair<std::string, plan::LogicalPlan>> shapes = {
      {"join", PlanOf(join, 6)},
      {"project, filter",
       PlanOf(Node(plan::ProjectNode{.input = Node(plan::FilterNode{
                                         .input = join,
                                         .predicates = {testing::Compare(
                                             out_pid, plan::CompareOp::kLt, testing::BigInt(30))}}),
                                     .columns = {out_bid, out_pid}}),
              2)},
      {"aggregate",
       PlanOf(Node(plan::AggregateNode{.input = join, .aggregates = {count, min_bid, max_pid}}),
              3)},
      {"group by", PlanOf(Node(plan::GroupAggregateNode{
                              .input = join, .keys = {out_bk}, .aggregates = {count, min_bid}}),
                          3)},
      {"two levels",
       PlanOf(Node(plan::GroupAggregateNode{
                  .input = join, .keys = {out_bk}, .aggregates = {distinct_pid, count}}),
              3)},
      // COUNT(DISTINCT) next to another call, without keys: a global aggregation in two levels.
      {"global two levels",
       PlanOf(Node(plan::AggregateNode{.input = join, .aggregates = {distinct_pid, count}}), 2)},
      {"count distinct",
       PlanOf(Node(plan::AggregateNode{.input = join, .aggregates = {distinct_pid}}), 1)},
      {"top-N", PlanOf(Node(plan::LimitNode{.input = sorted, .limit = 5}), 6)},
      {"limit", PlanOf(Node(plan::LimitNode{.input = join, .limit = 7}), 6)},
      {"sort", PlanOf(sorted, 6)},
      {"partition top-N",
       PlanOf(
           Node(plan::LimitNode{.input = Node(plan::SortNode{
                                    .input = grouped,
                                    .keys = {{.column = Column(1, "count", LogicalType::kBigInt),
                                              .descending = true},
                                             {.column = Column(0, "bk", LogicalType::kBigInt)}}}),
                                .limit = 3}),
           2)},
      {"serial probe",
       PlanOf(JoinOf(probe_groups, ScanOf(b, "b"), {Column(0, "pk", LogicalType::kBigInt)}, {bk}),
              5)},
      {"drained build",
       PlanOf(JoinOf(ScanOf(p, "p"), build_groups, {pk}, {Column(0, "bk", LogicalType::kBigInt)}),
              5)},
      {"chain", PlanOf(chain, 9)},
      {"nested build", PlanOf(nested, 9)},
  };
  for (const auto& [name, plan] : shapes) {
    SCOPED_TRACE(name);
    auto serial = RunPlan(plan, ContextOf(nullptr, 4));
    ASSERT_TRUE(serial.ok()) << serial.status().ToString();
    EXPECT_GT((*serial)->num_rows(), 0);
    auto parallel = RunPlan(plan, ContextOf(pool.get(), 4));
    ASSERT_TRUE(parallel.ok()) << parallel.status().ToString();
    EXPECT_TRUE((*parallel)->Equals(**serial));
    if (name == "join") {
      EXPECT_EQ(RowsOf(**serial), ReferenceJoin(RowsOf(*p), RowsOf(*b), {0}, {0}));
    } else if (name == "global two levels") {
      const Rows rows = ReferenceJoin(RowsOf(*p), RowsOf(*b), {0}, {0});
      std::set<std::string> ids;
      for (const std::vector<std::string>& row : rows) {
        ids.insert(row[2]);
      }
      EXPECT_EQ(RowsOf(**serial),
                (Rows{{std::to_string(ids.size()), std::to_string(rows.size())}}));
    } else if (name == "chain") {
      EXPECT_EQ(RowsOf(**serial), ReferenceJoin(ReferenceJoin(RowsOf(*p), RowsOf(*b), {0}, {0}),
                                                RowsOf(*d), {2}, {0}));
    } else if (name == "nested build") {
      EXPECT_EQ(
          RowsOf(**serial),
          ReferenceJoin(RowsOf(*p), ReferenceJoin(RowsOf(*b), RowsOf(*d), {2}, {0}), {0}, {0}));
    }
  }
}

// Through the physical planner, a semi, anti or null-aware anti join gives the nested-loop
// reference, the probe's rows in part order: on a BIGINT, a VARCHAR ('' is no NULL) or a
// two-column key (a null-aware anti join on one key, over the build table and over its rows
// without a NULL key), on unique or repeated build keys, for any batch size. A one-row join appends
// the row of an aggregate over the build table to every probe row.
TEST_F(HashJoinPlanTest, EveryKindMatchesTheNestedLoopReference) {
  const auto probe_table = MixedTable("p", 6, 7, ProbeKey);
  const Rows probe_rows = RowsOf(*probe_table);
  const std::vector<std::vector<int>> keys = {{0}, {1}, {0, 1}};
  for (const bool unique : {false, true}) {
    const auto build_table =
        unique ? MixedTable("b", 2, 6, [](int64_t i) -> std::optional<int64_t> { return i; })
               : MixedTable("b", 3, 5, [](int64_t i) -> std::optional<int64_t> { return i % 10; });
    const Rows build_rows = RowsOf(*build_table);
    for (const plan::JoinKind kind :
         {plan::JoinKind::kSemi, plan::JoinKind::kAnti, plan::JoinKind::kNullAwareAnti}) {
      for (const std::vector<int>& columns : keys) {
        if (kind == plan::JoinKind::kNullAwareAnti && columns.size() > 1) {
          continue;
        }
        std::vector<plan::BoundColumn> probe_keys;
        std::vector<plan::BoundColumn> build_keys;
        std::vector<plan::Predicate> not_null;
        for (const int c : columns) {
          probe_keys.push_back(MixedColumn("p", c));
          build_keys.push_back(MixedColumn("b", c));
          not_null.push_back(plan::Predicate{.kind = plan::Predicate::Kind::kIsNotNull,
                                             .column = MixedColumn("b", c)});
        }
        for (const bool filtered : {false, true}) {
          if (filtered && kind != plan::JoinKind::kNullAwareAnti) {
            continue;
          }
          Rows build = build_rows;
          plan::LogicalNodePtr build_input = ScanOf(build_table, "b");
          if (filtered) {
            std::erase_if(
                build, [&](const std::vector<std::string>& row) { return NullKey(row, columns); });
            build_input = Node(plan::FilterNode{.input = build_input, .predicates = not_null});
          }
          const Rows expected = ReferenceFilter(kind, probe_rows, build, columns, columns);
          const auto plan = PlanOf(
              JoinOf(kind, ScanOf(probe_table, "p"), build_input, probe_keys, build_keys), 3);
          for (const int64_t batch_size : {1, 3, 64}) {
            SCOPED_TRACE(std::string(plan::ToString(kind)) + " on " +
                         std::to_string(columns.size()) + " key(s) from column " +
                         std::to_string(columns[0]) + (unique ? ", unique" : ", repeated") +
                         (filtered ? ", no NULL key" : "") + ", batch size " +
                         std::to_string(batch_size));
            auto result = RunPlan(plan, ContextOf(nullptr, batch_size));
            ASSERT_TRUE(result.ok()) << result.status().ToString();
            EXPECT_EQ(RowsOf(**result), expected);
          }
        }
      }
    }
    // MIN(bk), COUNT(*) and MAX(bs) of the build table, appended to every probe row.
    const plan::LogicalNodePtr row = Node(plan::AggregateNode{
        .input = ScanOf(build_table, "b"),
        .aggregates = {
            {.kind = plan::AggKind::kMin, .arg = MixedColumn("b", 0), .type = LogicalType::kBigInt},
            {.kind = plan::AggKind::kCountStar, .arg = {}, .type = LogicalType::kBigInt},
            {.kind = plan::AggKind::kMax,
             .arg = MixedColumn("b", 1),
             .type = LogicalType::kVarchar}}});
    const auto plan =
        PlanOf(JoinOf(plan::JoinKind::kOneRow, ScanOf(probe_table, "p"), row, {}, {}), 6);
    for (const int64_t batch_size : {1, 3, 64}) {
      SCOPED_TRACE("one-row, batch size " + std::to_string(batch_size));
      auto result = RunPlan(plan, ContextOf(nullptr, batch_size));
      ASSERT_TRUE(result.ok()) << result.status().ToString();
      EXPECT_EQ(RowsOf(**result),
                ReferenceOneRow(probe_rows, {"0", std::to_string(build_rows.size()), "'y'"}));
    }
  }
}

// The shapes of SameResultsOnOneAndFourThreads where a join's kind matters give the same rows on
// one thread and on four, for every kind but inner: the part union at the root, an aggregate above
// the join, a probe over a serial input, a drained build, a chain with another join in one
// pipeline and a build whose input probes a join of the kind. A one-row join's build input is an
// aggregate (drained), or one row of a part pipeline.
TEST_F(HashJoinPlanTest, EveryKindGivesTheSameResultsOnOneAndFourThreads) {
  const auto pool = MakeThreadPool();
  const auto p = MixedTable("p", 6, 7, ProbeKey);
  const auto b = MixedTable("b", 3, 5, [](int64_t i) -> std::optional<int64_t> { return i % 10; });
  const auto d = MixedTable("d", 2, 6, [](int64_t i) -> std::optional<int64_t> { return i; });
  const auto pk = MixedColumn("p", 0);
  const auto pid = MixedColumn("p", 2);
  const auto bk = MixedColumn("b", 0);
  const auto bid = MixedColumn("b", 2);
  const auto dk = MixedColumn("d", 0);
  const auto did = MixedColumn("d", 2);
  const plan::AggregateCall count{
      .kind = plan::AggKind::kCountStar, .arg = {}, .type = LogicalType::kBigInt};
  const plan::AggregateCall max_pid{
      .kind = plan::AggKind::kMax, .arg = pid, .type = LogicalType::kBigInt};
  // A probe input that is no part pipeline (p's keys, grouped), and a build input that is none.
  const auto probe_groups =
      Node(plan::GroupAggregateNode{.input = ScanOf(p, "p"), .keys = {pk}, .aggregates = {count}});
  const auto build_groups = Node(plan::GroupAggregateNode{
      .input = ScanOf(b, "b"),
      .keys = {bk},
      .aggregates = {{.kind = plan::AggKind::kMin, .arg = bid, .type = LogicalType::kBigInt}}});
  // The one row of MIN(id), COUNT(*) over `input`.
  const auto row_of = [&](const plan::LogicalNodePtr& input, const plan::BoundColumn& id) {
    return Node(plan::AggregateNode{
        .input = input,
        .aggregates = {{.kind = plan::AggKind::kMin, .arg = id, .type = LogicalType::kBigInt},
                       count}});
  };
  std::vector<std::pair<std::string, plan::LogicalPlan>> shapes;
  for (const plan::JoinKind kind :
       {plan::JoinKind::kSemi, plan::JoinKind::kAnti, plan::JoinKind::kNullAwareAnti}) {
    const std::string name(plan::ToString(kind));
    const auto join = JoinOf(kind, ScanOf(p, "p"), ScanOf(b, "b"), {pk}, {bk});  // p's columns
    shapes.emplace_back(name + " join", PlanOf(join, 3));
    shapes.emplace_back(
        name + " aggregate",
        PlanOf(Node(plan::AggregateNode{.input = join, .aggregates = {count, max_pid}}), 2));
    shapes.emplace_back(name + " serial probe",
                        PlanOf(JoinOf(kind, probe_groups, ScanOf(b, "b"),
                                      {Column(0, "pk", LogicalType::kBigInt)}, {bk}),
                               2));
    shapes.emplace_back(name + " drained build",
                        PlanOf(JoinOf(kind, ScanOf(p, "p"), build_groups, {pk},
                                      {Column(0, "bk", LogicalType::kBigInt)}),
                               3));
    shapes.emplace_back(
        name + " chain",
        PlanOf(JoinOf(plan::JoinKind::kInner, join, ScanOf(d, "d"), {pid}, {dk}), 6));
    shapes.emplace_back(
        name + " nested build",
        PlanOf(JoinOf(plan::JoinKind::kInner, ScanOf(p, "p"),
                      JoinOf(kind, ScanOf(b, "b"), ScanOf(d, "d"), {bid}, {dk}), {pk}, {bk}),
               6));
  }
  // One-row: p's columns, then MIN(bid) and COUNT(*) over b.
  const auto one_row =
      JoinOf(plan::JoinKind::kOneRow, ScanOf(p, "p"), row_of(ScanOf(b, "b"), bid), {}, {});
  shapes.emplace_back("ONE-ROW join", PlanOf(one_row, 5));
  shapes.emplace_back(
      "ONE-ROW aggregate",
      PlanOf(Node(plan::AggregateNode{.input = one_row,
                                      .aggregates = {count,
                                                     max_pid,
                                                     {.kind = plan::AggKind::kMin,
                                                      .arg = Column(3, "min", LogicalType::kBigInt),
                                                      .type = LogicalType::kBigInt}}}),
             3));
  shapes.emplace_back("ONE-ROW serial probe", PlanOf(JoinOf(plan::JoinKind::kOneRow, probe_groups,
                                                            row_of(ScanOf(b, "b"), bid), {}, {}),
                                                     4));
  shapes.emplace_back(
      "ONE-ROW pipeline build",
      PlanOf(JoinOf(plan::JoinKind::kOneRow, ScanOf(p, "p"),
                    Node(plan::FilterNode{.input = ScanOf(d, "d"),
                                          .predicates = {testing::Compare(did, plan::CompareOp::kEq,
                                                                          testing::BigInt(3))}}),
                    {}, {}),
             6));
  shapes.emplace_back(
      "ONE-ROW chain",
      PlanOf(JoinOf(plan::JoinKind::kOneRow,
                    JoinOf(plan::JoinKind::kSemi, ScanOf(p, "p"), ScanOf(b, "b"), {pk}, {bk}),
                    row_of(ScanOf(d, "d"), did), {}, {}),
             5));
  shapes.emplace_back("ONE-ROW nested build",
                      PlanOf(JoinOf(plan::JoinKind::kInner, ScanOf(p, "p"),
                                    JoinOf(plan::JoinKind::kOneRow, ScanOf(b, "b"),
                                           row_of(ScanOf(d, "d"), did), {}, {}),
                                    {pk}, {bk}),
                             8));
  for (const auto& [name, plan] : shapes) {
    SCOPED_TRACE(name);
    auto serial = RunPlan(plan, ContextOf(nullptr, 4));
    ASSERT_TRUE(serial.ok()) << serial.status().ToString();
    EXPECT_GT((*serial)->num_rows(), 0);
    auto parallel = RunPlan(plan, ContextOf(pool.get(), 4));
    ASSERT_TRUE(parallel.ok()) << parallel.status().ToString();
    EXPECT_TRUE((*parallel)->Equals(**serial));
  }
}

// Filter pushdown (ADR 0020) stays with a Filter right on a scan, on either side of a join, each
// scan in its own parts: the probe's 6 parts and the build's 2 apply their own predicates while
// they read (no probe part number reaches the build, which has no part 5). A Filter above the join
// reads the join's columns and pushes nothing. The rows are the same either way.
TEST_F(HashJoinPlanTest, FilterPushdownReachesEachSideWithItsOwnParts) {
  const auto pool = MakeThreadPool();
  const auto pk = MixedColumn("p", 0);
  const auto bk = MixedColumn("b", 0);
  const auto at_least_2 = testing::Compare(pk, plan::CompareOp::kGe, testing::BigInt(2));
  const auto at_most_6 = testing::Compare(bk, plan::CompareOp::kLe, testing::BigInt(6));
  for (arrow::internal::Executor* executor : Executors(pool.get())) {
    SCOPED_TRACE(executor == nullptr ? "one thread" : "pool");
    const auto key = [](int64_t i) -> std::optional<int64_t> { return i % 10; };
    const auto p = MixedTable("p", 6, 7, ProbeKey);
    const auto b = MixedTable("b", 2, 6, key);
    const auto below = JoinOf(
        Node(plan::FilterNode{.input = ScanOf(p, "p"), .predicates = {at_least_2}}),
        Node(plan::FilterNode{.input = ScanOf(b, "b"), .predicates = {at_most_6}}), {pk}, {bk});
    ProfileNode profile;
    auto pushed = RunPlan(PlanOf(below, 6), ContextOf(executor), &profile);
    ASSERT_TRUE(pushed.ok()) << pushed.status().ToString();
    EXPECT_EQ(p->filtered_scans(), 6);
    EXPECT_EQ(b->filtered_scans(), 2);
    ASSERT_EQ(profile.children().size(), 2U);
    for (const ProfileNode* side : profile.children()) {
      const ProfileNode& filter = *side->children()[0];
      EXPECT_EQ(filter.name(), "Filter");
      EXPECT_TRUE(filter.children()[0]->detail().ends_with(", 1 pushed predicate"))
          << filter.children()[0]->detail();
    }
    const auto q = MixedTable("p", 6, 7, ProbeKey);
    const auto c = MixedTable("b", 2, 6, key);
    const auto above = Node(plan::FilterNode{
        .input = JoinOf(ScanOf(q, "p"), ScanOf(c, "b"), {pk}, {bk}),
        .predicates = {at_least_2, testing::Compare(MixedColumn("b", 0, 3), plan::CompareOp::kLe,
                                                    testing::BigInt(6))}});
    auto kept = RunPlan(PlanOf(above, 6), ContextOf(executor));
    ASSERT_TRUE(kept.ok()) << kept.status().ToString();
    EXPECT_EQ(q->filtered_scans(), 0);
    EXPECT_EQ(c->filtered_scans(), 0);
    EXPECT_EQ(RowsOf(**kept), RowsOf(**pushed));
    EXPECT_FALSE(RowsOf(**kept).empty());
  }
}

// Parts are skipped by the statistics of the Filters right on each side's scan, each side in its
// own parts (`skipped` on the operator that consumes them: the probe's sink, the build). A Filter
// above a join never skips a part: its columns are the join's, and the build's columns come first
// on a build on the left, so its predicate on the build's id (column 2) must not be read as one on
// the probe scan's column 2 (the probe's id), which would skip the probe's part 0.
TEST_F(HashJoinPlanTest, PartPruningReadsOnlyTheScansOwnFilters) {
  const auto pk = MixedColumn("p", 0);
  const auto pid = MixedColumn("p", 2);
  const auto bk = MixedColumn("b", 0);
  const auto bid = MixedColumn("b", 2);
  const auto key = [](int64_t i) -> std::optional<int64_t> { return i % 10; };
  {
    // pid >= 28 on the probe's scan: its parts 0 to 3 (pid 0 to 27) are skipped.
    const auto p = MixedTable("p", 6, 7, ProbeKey);
    const auto b = MixedTable("b", 3, 5, key);
    ProfileNode profile;
    auto result = RunPlan(
        PlanOf(JoinOf(Node(plan::FilterNode{.input = ScanOf(p, "p"),
                                            .predicates = {testing::Compare(
                                                pid, plan::CompareOp::kGe, testing::BigInt(28))}}),
                      ScanOf(b, "b"), {pk}, {bk}),
               6),
        ContextOf(nullptr), &profile);
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    EXPECT_EQ(MetricOf(profile, "skipped"), 4);
    EXPECT_EQ(MetricOf(*profile.children()[1], "skipped"), 0);
    EXPECT_EQ(p->scanned_parts(), (std::vector<int64_t>{4, 5}));
    Rows probe_rows;
    for (const std::vector<std::string>& row : RowsOf(*p)) {
      if (std::stoll(row[2]) >= 28) {
        probe_rows.push_back(row);
      }
    }
    EXPECT_EQ(RowsOf(**result), ReferenceJoin(probe_rows, RowsOf(*b), {0}, {0}));
  }
  {
    // bid >= 12 above a join that builds on the left: no part skipped on either side.
    const auto p = MixedTable("p", 6, 7, ProbeKey);
    const auto b = MixedTable("b", 3, 5, key);
    ProfileNode profile;
    auto result = RunPlan(
        PlanOf(
            Node(plan::FilterNode{
                .input = JoinOf(ScanOf(p, "p"), ScanOf(b, "b"), {pk}, {bk}, BuildSide::kLeft),
                .predicates = {testing::Compare(bid, plan::CompareOp::kGe, testing::BigInt(12))}}),
            6),
        ContextOf(nullptr), &profile);
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    EXPECT_EQ(profile.name(), "PartUnion");  // the Filter runs in the probe pipeline
    EXPECT_EQ(MetricOf(profile, "skipped"), 0);
    EXPECT_EQ(p->scanned_parts(), (std::vector<int64_t>{0, 1, 2, 3, 4, 5}));
    const Rows expected =
        WhereAtLeast(ReferenceJoin(RowsOf(*p), RowsOf(*b), {0}, {0}, BuildSide::kLeft), 2, 12);
    ASSERT_FALSE(expected.empty());
    EXPECT_EQ(RowsOf(**result), expected);
  }
  {
    // bid >= 10 on the build's scan: its parts 0 and 1 are skipped.
    const auto p = MixedTable("p", 6, 7, ProbeKey);
    const auto b = MixedTable("b", 3, 5, key);
    ProfileNode profile;
    auto result = RunPlan(
        PlanOf(JoinOf(ScanOf(p, "p"),
                      Node(plan::FilterNode{.input = ScanOf(b, "b"),
                                            .predicates = {testing::Compare(
                                                bid, plan::CompareOp::kGe, testing::BigInt(10))}}),
                      {pk}, {bk}),
               6),
        ContextOf(nullptr), &profile);
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    EXPECT_EQ(MetricOf(profile, "skipped"), 0);
    const ProfileNode& build = *profile.children()[1];
    EXPECT_EQ(build.name(), "HashBuild");
    EXPECT_EQ(MetricOf(build, "skipped"), 2);
    EXPECT_EQ(b->scanned_parts(), (std::vector<int64_t>{2}));
    EXPECT_EQ(RowsOf(**result),
              ReferenceJoin(RowsOf(*p), WhereAtLeast(RowsOf(*b), 2, 10), {0}, {0}));
  }
}

// Late materialization (ADR 0016) declines over a join: a top-N over a probe pipeline reads every
// column, where the same scan without the join reads its unused columns late (the positive
// control). The rows are those of the plain top-N.
TEST_F(HashJoinPlanTest, LateMaterializationDeclinesOverAJoin) {
  const auto p = MixedTable("p", 6, 7, ProbeKey);
  const auto b = MixedTable("b", 3, 5, [](int64_t i) -> std::optional<int64_t> { return i % 10; });
  const auto detail = [](const plan::LogicalPlan& plan) {
    ProfileNode root;
    EXPECT_TRUE(BuildPhysicalPlan(plan, &root).ok());
    return root.detail();
  };
  const auto pid = MixedColumn("p", 2);
  const auto alone =
      PlanOf(Node(plan::LimitNode{
                 .input = Node(plan::SortNode{.input = ScanOf(p, "p"),
                                              .keys = {{.column = pid, .descending = true}}}),
                 .limit = 2}),
             3);
  EXPECT_EQ(detail(alone), "Sort pid DESC NULLS LAST Limit 2 late=2 columns");
  const auto joined = PlanOf(
      Node(plan::LimitNode{.input = Node(plan::SortNode{
                               .input = JoinOf(ScanOf(p, "p"), ScanOf(b, "b"),
                                               {MixedColumn("p", 0)}, {MixedColumn("b", 0)}),
                               .keys = {{.column = pid, .descending = true},
                                        {.column = MixedColumn("b", 2, 3), .descending = true}}}),
                           .limit = 2}),
      6);
  EXPECT_EQ(detail(joined), "Sort pid DESC NULLS LAST, bid DESC NULLS LAST Limit 2");
  auto result = RunPlan(joined, ContextOf(nullptr));
  ASSERT_TRUE(result.ok()) << result.status().ToString();
  Rows expected = ReferenceJoin(RowsOf(*p), RowsOf(*b), {0}, {0});
  std::ranges::sort(expected,
                    [](const std::vector<std::string>& x, const std::vector<std::string>& y) {
                      return std::pair(std::stoll(x[2]), std::stoll(x[5])) >
                             std::pair(std::stoll(y[2]), std::stoll(y[5]));
                    });
  expected.resize(2);
  EXPECT_EQ(RowsOf(**result), expected);
}

// A COUNT(DISTINCT) of one column alone over a join is the GROUP BY of that column over the join's
// pipeline (its build prepared first), then counted: a scalar aggregate over a partitioned GROUP BY
// whose parts probe the build.
TEST_F(HashJoinPlanTest, CountDistinctRewriteRunsOverAJoin) {
  const auto pool = MakeThreadPool();
  const auto p = MixedTable("p", 6, 7, ProbeKey);
  const auto b = MixedTable("b", 3, 5, [](int64_t i) -> std::optional<int64_t> { return i % 10; });
  const auto plan =
      PlanOf(Node(plan::AggregateNode{.input = JoinOf(ScanOf(p, "p"), ScanOf(b, "b"),
                                                      {MixedColumn("p", 0)}, {MixedColumn("b", 0)}),
                                      .aggregates = {{.kind = plan::AggKind::kCountDistinct,
                                                      .arg = MixedColumn("b", 2, 3),
                                                      .type = LogicalType::kBigInt}}}),
             1);
  std::set<std::string> ids;
  for (const std::vector<std::string>& row : ReferenceJoin(RowsOf(*p), RowsOf(*b), {0}, {0})) {
    ids.insert(row[5]);
  }
  for (arrow::internal::Executor* executor : Executors(pool.get())) {
    SCOPED_TRACE(executor == nullptr ? "one thread" : "pool");
    ProfileNode root;
    auto result = RunPlan(plan, ContextOf(executor), &root);
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    EXPECT_EQ(testing::SingleInt64(**result), static_cast<int64_t>(ids.size()));
    EXPECT_EQ(root.name(), "ScalarAggregate");
    ASSERT_EQ(root.children().size(), 1U);
    const ProfileNode& groups = *root.children()[0];
    EXPECT_EQ(groups.name(), "PartGroupAggregate");
    ASSERT_EQ(groups.children().size(), 2U);
    EXPECT_EQ(groups.children()[0]->name(), "HashJoin");
    EXPECT_TRUE(groups.children()[0]->per_part());
    EXPECT_EQ(groups.children()[1]->name(), "HashBuild");
  }
}

// A top-N right above a GROUP BY over a join keeps each partition's first rows (PartitionTopN,
// ADR 0011) in the GROUP BY over the join's pipeline: the rows of the full sort.
TEST_F(HashJoinPlanTest, PartitionTopNRunsOverAJoin) {
  const auto pool = MakeThreadPool();
  const auto p = MixedTable("p", 6, 7, ProbeKey);
  const auto b = MixedTable("b", 3, 5, [](int64_t i) -> std::optional<int64_t> { return i % 10; });
  const auto grouped = Node(plan::GroupAggregateNode{
      .input = JoinOf(ScanOf(p, "p"), ScanOf(b, "b"), {MixedColumn("p", 0)}, {MixedColumn("b", 0)}),
      .keys = {MixedColumn("p", 0)},
      .aggregates = {
          {.kind = plan::AggKind::kCountStar, .arg = {}, .type = LogicalType::kBigInt}}});
  const auto plan = PlanOf(
      Node(plan::LimitNode{
          .input = Node(plan::SortNode{
              .input = grouped,
              .keys = {{.column = Column(1, "count", LogicalType::kBigInt), .descending = true},
                       {.column = Column(0, "pk", LogicalType::kBigInt)}}}),
          .limit = 3}),
      2);
  // The reference: the matches of each probe key, most first, then by key.
  std::map<int64_t, int64_t> matches;
  for (const std::vector<std::string>& row : ReferenceJoin(RowsOf(*p), RowsOf(*b), {0}, {0})) {
    ++matches[std::stoll(row[0])];
  }
  std::vector<std::pair<int64_t, int64_t>> order;  // (-count, key)
  order.reserve(matches.size());
  for (const auto& [k, n] : matches) {
    order.emplace_back(-n, k);
  }
  std::ranges::sort(order);
  Rows expected;
  for (std::size_t i = 0; i < 3; ++i) {
    expected.push_back({std::to_string(order[i].second), std::to_string(-order[i].first)});
  }
  for (arrow::internal::Executor* executor : Executors(pool.get())) {
    SCOPED_TRACE(executor == nullptr ? "one thread" : "pool");
    ProfileNode root;
    auto result = RunPlan(plan, ContextOf(executor), &root);
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    EXPECT_EQ(RowsOf(**result), expected);
    EXPECT_EQ(root.name(), "TopN");
    ASSERT_EQ(root.children().size(), 1U);
    EXPECT_EQ(root.children()[0]->name(), "PartGroupAggregate");
    EXPECT_EQ(root.children()[0]->detail(),
              "GroupAggregate keys=[pk] COUNT(*) top-N per partition keep=3");
  }
}

// A LIMIT over a join builds first, then stops the probe's parts once it has its rows: every build
// part is read, and the probe's parts only up to the window of parts in flight (the first one
// alone on one thread). LIMIT 0 reads neither table: the operator that runs the probe pipeline is
// never pulled, so no build runs (as in DuckDB).
TEST_F(HashJoinPlanTest, LimitOverAJoinBuildsFirstThenStopsProbeParts) {
  const auto pool = MakeThreadPool();
  const auto pk = MixedColumn("p", 0);
  const auto bk = MixedColumn("b", 0);
  const auto every = [](int64_t i) -> std::optional<int64_t> { return i % 5; };
  const auto key = [](int64_t i) -> std::optional<int64_t> { return i % 10; };
  for (arrow::internal::Executor* executor : Executors(pool.get())) {
    SCOPED_TRACE(executor == nullptr ? "one thread" : "pool");
    const auto p = MixedTable("p", 20, 7, every);
    const auto b = MixedTable("b", 3, 5, key);
    const auto join = JoinOf(ScanOf(p, "p"), ScanOf(b, "b"), {pk}, {bk});
    auto result =
        RunPlan(PlanOf(Node(plan::LimitNode{.input = join, .limit = 3}), 6), ContextOf(executor));
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    EXPECT_EQ((*result)->num_rows(), 3);
    EXPECT_EQ(b->scanned_parts(), (std::vector<int64_t>{0, 1, 2}));
    if (executor == nullptr) {
      EXPECT_EQ(p->scanned_parts(), std::vector<int64_t>{0});
    } else {
      EXPECT_LE(p->scanned_parts().size(), static_cast<std::size_t>((2 * kThreads) + 1));
    }
    // LIMIT 0, over the join and over a sort of it.
    for (const bool sorted : {false, true}) {
      const auto q = MixedTable("p", 20, 7, every);
      const auto c = MixedTable("b", 3, 5, key);
      plan::LogicalNodePtr input = JoinOf(ScanOf(q, "p"), ScanOf(c, "b"), {pk}, {bk});
      if (sorted) {
        input = Node(plan::SortNode{.input = input, .keys = {{.column = pk}}});
      }
      auto none = RunPlan(PlanOf(Node(plan::LimitNode{.input = input, .limit = 0}), 6),
                          ContextOf(executor));
      ASSERT_TRUE(none.ok()) << none.status().ToString();
      EXPECT_EQ((*none)->num_rows(), 0);
      EXPECT_TRUE(q->scanned_parts().empty()) << sorted;
      EXPECT_TRUE(c->scanned_parts().empty()) << sorted;
    }
  }
}

// A build's error is that of its first failing part in part order, on one thread and on four; it
// comes before any probe part runs, so it beats the probe's own error. In a chain the outer join's
// build, prepared first, decides, and the inner one is never read.
TEST_F(HashJoinPlanTest, FirstBuildErrorInPartOrderWins) {
  const auto pool = MakeThreadPool();
  const auto key = [](int64_t i) -> std::optional<int64_t> { return i % 3; };
  const auto pk = MixedColumn("p", 0);
  for (arrow::internal::Executor* executor : Executors(pool.get())) {
    SCOPED_TRACE(executor == nullptr ? "one thread" : "pool");
    const auto b = MixedTable("b", 6, 2, key);
    b->FailPart(2);
    b->FailPart(1);
    const auto p = MixedTable("p", 3, 2, key);
    p->FailPart(0);
    auto result =
        RunPlan(PlanOf(JoinOf(ScanOf(p, "p"), ScanOf(b, "b"), {pk}, {MixedColumn("b", 0)}), 6),
                ContextOf(executor));
    EXPECT_TRUE(result.status().IsIOError()) << result.status().ToString();
    EXPECT_EQ(result.status().message(), "part 1 is broken");
    EXPECT_TRUE(p->scanned_parts().empty());
    // p joins i, then o: o's build comes first.
    const auto o = MixedTable("o", 4, 2, key);
    o->FailPart(3);
    const auto i = MixedTable("i", 2, 2, key);
    i->FailPart(0);
    const auto q = MixedTable("p", 3, 2, key);
    auto chain =
        RunPlan(PlanOf(JoinOf(JoinOf(ScanOf(q, "p"), ScanOf(i, "i"), {pk}, {MixedColumn("i", 0)}),
                              ScanOf(o, "o"), {pk}, {MixedColumn("o", 0)}),
                       9),
                ContextOf(executor));
    EXPECT_EQ(chain.status().message(), "part 3 is broken");
    EXPECT_TRUE(i->scanned_parts().empty());
    EXPECT_TRUE(q->scanned_parts().empty());
  }
}

// Every build part runs out of memory on a worker (as many parts at once might): each runs again
// alone, on the calling thread, and the rows are those of a run without failures; a drained build's
// parts too. The probe reads a serial input (a Limit over its scan, whose parts take nothing from
// the pool) on the calling thread, so every failure on a worker is a build part's.
TEST_F(HashJoinPlanTest, ABuildPartRunsAgainAloneAfterOutOfMemory) {
  const auto pool = MakeThreadPool();
  const auto p = KeyTable("p", 5, 10, [](int64_t i) { return i; });
  const auto probe = Node(plan::LimitNode{.input = ScanOf(p, "p"), .limit = std::nullopt});
  const auto pk = Column(0, "pk", LogicalType::kBigInt);
  const auto bk = Column(0, "bk", LogicalType::kBigInt);
  for (const bool drained : {false, true}) {
    SCOPED_TRACE(drained ? "drained" : "pipeline");
    const auto b = KeyTable("b", 6, 20, [](int64_t i) { return i % 37; });
    const Rows expected = ReferenceJoin(RowsOf(*p), RowsOf(*b), {0}, {0});
    const auto build = drained
                           ? Node(plan::LimitNode{.input = ScanOf(b, "b"), .limit = std::nullopt})
                           : ScanOf(b, "b");
    MemoryBudget budget(std::nullopt);
    WorkerFailingPool failing(&budget);
    {
      ExecContext ctx{.pool = &failing,
                      .batch_size = 16,
                      .executor = pool.get(),
                      .threads = kThreads,
                      .budget = &budget};
      auto result = RunPlan(PlanOf(JoinOf(probe, build, {pk}, {bk}), 4), ctx);
      ASSERT_TRUE(result.ok()) << result.status().ToString();
      EXPECT_EQ(RowsOf(**result), expected);
    }
    // At least one failure per build part: the drained build's parts are its 12 batches of at
    // most 16 rows. A part of the pipeline build is read on a worker, then again alone.
    EXPECT_GE(failing.failures(), drained ? 12 : 6);
    if (!drained) {
      const std::vector<int64_t> scanned = b->scanned_parts();
      for (int64_t part = 0; part < 6; ++part) {
        EXPECT_GE(std::ranges::count(scanned, part), 2) << part;
      }
    }
    EXPECT_EQ(budget.bytes_allocated(), 0);
  }
}

// A join's residuals are evaluated by its probe, in order, each on the rows the ones before it
// kept, in a part pipeline and over a serial probe input, a build on either side: bid < 2, then
// bid * 2^62 > 0 keeps the matches with bid 1, and never meets a larger bid, whose product
// overflows; the other order fails.
TEST_F(HashJoinPlanTest, TheProbeEvaluatesTheResidualsInOrder) {
  const auto p = MixedTable("p", 6, 7, ProbeKey);
  const auto b = MixedTable("b", 3, 5, [](int64_t i) -> std::optional<int64_t> { return i % 10; });
  constexpr int64_t kTwoTo62 = 4611686018427387904;
  for (const BuildSide side : {BuildSide::kRight, BuildSide::kLeft}) {
    const int bid = side == BuildSide::kRight ? 5 : 2;  // the build's id in the join's output
    const plan::ExprPtr narrow =
        Condition(plan::CompareOp::kLt, 2, ColumnAt(bid, LogicalType::kBigInt));
    const plan::ExprPtr product = Condition(
        plan::CompareOp::kGt, 0, Times(ColumnAt(bid, LogicalType::kBigInt), ConstantOf(kTwoTo62)));
    Rows expected;
    for (const std::vector<std::string>& row :
         ReferenceJoin(RowsOf(*p), RowsOf(*b), {0}, {0}, side)) {
      if (row[static_cast<std::size_t>(bid)] == "1") {
        expected.push_back(row);
      }
    }
    ASSERT_FALSE(expected.empty());
    for (const bool serial : {false, true}) {
      SCOPED_TRACE(std::string(serial ? "serial probe" : "part pipeline") +
                   (side == BuildSide::kLeft ? ", build left" : ", build right"));
      const plan::LogicalNodePtr probe =
          serial ? Node(plan::LimitNode{.input = ScanOf(p, "p"), .limit = std::nullopt})
                 : ScanOf(p, "p");
      const auto run = [&](std::vector<plan::ExprPtr> residual) {
        return RunPlan(PlanOf(JoinOf(probe, ScanOf(b, "b"), {MixedColumn("p", 0)},
                                     {MixedColumn("b", 0)}, side, std::move(residual)),
                              6),
                       ContextOf(nullptr));
      };
      auto kept = run({narrow, product});
      ASSERT_TRUE(kept.ok()) << kept.status().ToString();
      EXPECT_EQ(RowsOf(**kept), expected);
      EXPECT_TRUE(run({product, narrow}).status().IsExecutionError());
    }
  }
}

// The builds of one probe pipeline are prepared outermost first, all before any probe part runs,
// and a build whose input probes a build of its own prepares that one first (post-order); each is
// profiled under the operator that prepares it. The same order on one thread and on four.
TEST_F(HashJoinPlanTest, BuildsArePreparedOutermostFirstAndInPostOrder) {
  const auto pool = MakeThreadPool();
  const auto log = std::make_shared<ScanLog>();
  const auto key = [](int64_t i) -> std::optional<int64_t> { return i % 6; };
  const auto logged = [&](const std::string& name, int64_t parts) {
    return std::make_shared<LoggedTable>(MixedTable(name, parts, 4, key), name, log);
  };
  const auto p = logged("p", 5);
  const auto b = logged("b", 3);
  const auto c = logged("c", 2);
  const auto pk = MixedColumn("p", 0);
  const auto pid = MixedColumn("p", 2);
  const auto bk = MixedColumn("b", 0);
  const auto bid = MixedColumn("b", 2);
  const auto ck = MixedColumn("c", 0);
  // p joins b on pk = bk, then c on pid = ck: one pipeline over p, c's build first.
  const auto inner = JoinOf(ScanOf(p, "p"), ScanOf(b, "b"), {pk}, {bk});
  const auto chain = JoinOf(inner, ScanOf(c, "c"), {pid}, {ck});
  // p joins (b joins c on bid = ck) on pk = bk: b's build probes c's.
  const auto nested_build = JoinOf(ScanOf(b, "b"), ScanOf(c, "c"), {bid}, {ck});
  const auto nested = JoinOf(ScanOf(p, "p"), nested_build, {pk}, {bk});
  for (arrow::internal::Executor* executor : Executors(pool.get())) {
    SCOPED_TRACE(executor == nullptr ? "one thread" : "pool");
    {
      log->Clear();
      ProfileNode root;
      auto result = RunPlan(PlanOf(chain, 9), ContextOf(executor), &root);
      ASSERT_TRUE(result.ok()) << result.status().ToString();
      EXPECT_EQ(log->names(), Concat({Repeated("c", 2), Repeated("b", 3), Repeated("p", 5)}));
      ASSERT_EQ(root.children().size(), 3U);
      EXPECT_EQ(root.children()[0]->name(), "HashJoin");
      EXPECT_EQ(root.children()[0]->children()[0]->name(), "HashJoin");
      EXPECT_EQ(root.children()[1]->name(), "HashBuild");
      EXPECT_EQ(root.children()[1]->detail(), plan::ExplainNode(*chain));
      EXPECT_EQ(root.children()[2]->name(), "HashBuild");
      EXPECT_EQ(root.children()[2]->detail(), plan::ExplainNode(*inner));
    }
    {
      log->Clear();
      ProfileNode root;
      auto result = RunPlan(PlanOf(nested, 9), ContextOf(executor), &root);
      ASSERT_TRUE(result.ok()) << result.status().ToString();
      EXPECT_EQ(log->names(), Concat({Repeated("c", 2), Repeated("b", 3), Repeated("p", 5)}));
      ASSERT_EQ(root.children().size(), 2U);
      const ProfileNode& build = *root.children()[1];
      EXPECT_EQ(build.name(), "HashBuild");
      EXPECT_EQ(build.detail(), plan::ExplainNode(*nested));
      ASSERT_EQ(build.children().size(), 2U);
      EXPECT_EQ(build.children()[0]->name(), "HashJoin");
      EXPECT_TRUE(build.children()[0]->per_part());
      EXPECT_EQ(build.children()[1]->name(), "HashBuild");
      EXPECT_EQ(build.children()[1]->detail(), plan::ExplainNode(*nested_build));
    }
  }
}

// A probe over a serial input (here a GROUP BY's rows) prepares its build at its first Next, not in
// Open, and only then opens its input: the build's parts are read first, the probe's after.
TEST_F(HashJoinPlanTest, ProbeOverASerialInputBuildsAtItsFirstNext) {
  const auto pool = MakeThreadPool();
  const auto log = std::make_shared<ScanLog>();
  const auto key = [](int64_t i) -> std::optional<int64_t> { return i % 6; };
  const auto p = std::make_shared<LoggedTable>(MixedTable("p", 5, 4, key), "p", log);
  const auto b = std::make_shared<LoggedTable>(MixedTable("b", 3, 4, key), "b", log);
  const auto grouped = Node(plan::GroupAggregateNode{
      .input = ScanOf(p, "p"),
      .keys = {MixedColumn("p", 0)},
      .aggregates = {
          {.kind = plan::AggKind::kCountStar, .arg = {}, .type = LogicalType::kBigInt}}});
  const auto plan = PlanOf(JoinOf(grouped, ScanOf(b, "b"), {Column(0, "pk", LogicalType::kBigInt)},
                                  {MixedColumn("b", 0)}),
                           5);
  for (arrow::internal::Executor* executor : Executors(pool.get())) {
    SCOPED_TRACE(executor == nullptr ? "one thread" : "pool");
    log->Clear();
    auto op = BuildPhysicalPlan(plan);
    ASSERT_TRUE(op.ok()) << op.status().ToString();
    EXPECT_NE(dynamic_cast<const HashJoinOperator*>(op->get()), nullptr);
    ExecContext ctx = ContextOf(executor);
    ASSERT_TRUE((*op)->Open(ctx).ok());
    EXPECT_TRUE(log->names().empty());
    auto first = (*op)->Next();
    ASSERT_TRUE(first.ok()) << first.status().ToString();
    EXPECT_FALSE(first->end());
    EXPECT_EQ(log->names(), Concat({Repeated("b", 3), Repeated("p", 5)}));
    ASSERT_TRUE((*op)->Close().ok());
  }
}

}  // namespace
}  // namespace antb1::exec
