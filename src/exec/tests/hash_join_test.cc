// The inner hash join (docs/adr/0022-joins-and-query-blocks.md, "Execution"): the build
// (JoinBuild) from a part pipeline or a drained input, PrepareBuilds, the operator that prepares a
// probe pipeline's builds (BuildsFirstOperator) over every part sink, and the probe
// (HashJoinOperator) on its 1:1 and 1:N paths, with residuals, NULL and multi-column keys, empty
// builds, nested builds and chains, errors in the serial order, memory and profiles, on one thread
// and on a 4-thread pool.

#include "../hash_join.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
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
#include "antb1/exec/profile.h"
#include "antb1/exec/scalar_aggregate.h"
#include "antb1/exec/table_scan.h"
#include "antb1/plan/logical_plan.h"

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

// The inner join of `probe` and `build` on these key columns (equal cells, NULL never matching):
// every probe row in order, each with its matches in build order; a row is the left input's cells,
// then the right input's.
Rows ReferenceJoin(const Rows& probe, const Rows& build, const std::vector<int>& probe_keys,
                   const std::vector<int>& build_keys, BuildSide build_side = BuildSide::kRight) {
  Rows out;
  for (const std::vector<std::string>& p : probe) {
    for (const std::vector<std::string>& b : build) {
      bool match = true;
      for (std::size_t k = 0; k < probe_keys.size() && match; ++k) {
        const std::string& x = p[static_cast<std::size_t>(probe_keys[k])];
        const std::string& y = b[static_cast<std::size_t>(build_keys[k])];
        match = x != "null" && y != "null" && x == y;
      }
      if (!match) {
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

// The build of `table`'s parts on these keys (BIGINT columns of the table).
std::shared_ptr<JoinBuild> ScanBuild(const std::shared_ptr<MemoryTable>& table,
                                     const std::vector<int>& keys, ProfileNode* profile = nullptr) {
  std::vector<plan::BoundColumn> columns;
  columns.reserve(keys.size());
  for (const int key : keys) {
    columns.push_back(Column(key, table->schema()->field(key)->name(), LogicalType::kBigInt));
  }
  return std::make_shared<JoinBuild>(SpecOf(table->schema(), std::move(columns)),
                                     ScanPipeline(table), table->num_parts(),
                                     std::vector<std::shared_ptr<JoinBuild>>{}, profile);
}

// The build of what `source` returns, on these BIGINT keys.
std::shared_ptr<JoinBuild> DrainedBuild(std::unique_ptr<Operator> source,
                                        const std::vector<int>& keys,
                                        ProfileNode* profile = nullptr) {
  const std::shared_ptr<arrow::Schema> schema = source->output_schema();
  std::vector<plan::BoundColumn> columns;
  columns.reserve(keys.size());
  for (const int key : keys) {
    columns.push_back(Column(key, schema->field(key)->name(), LogicalType::kBigInt));
  }
  return std::make_shared<JoinBuild>(SpecOf(schema, std::move(columns)), std::move(source),
                                     profile);
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
                         std::make_shared<JoinBuild>(spec, SourceOf(build_schema, build), nullptr),
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
                         std::make_shared<JoinBuild>(spec, SourceOf(build_schema, build), nullptr),
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
        table->num_parts(), std::vector<std::shared_ptr<JoinBuild>>{}, nullptr);
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
      const auto inner =
          std::make_shared<JoinBuild>(SpecOf(t3->schema(), {Column(0, "ck", LogicalType::kBigInt)}),
                                      logged("c", ScanPipeline(t3)), t3->num_parts(),
                                      std::vector<std::shared_ptr<JoinBuild>>{}, nullptr);
      const PartPipeline middle = ProbePipeline(t2, inner, {1});
      const auto outer = std::make_shared<JoinBuild>(
          SpecOf(SchemaOf(middle), {Column(0, "bk", LogicalType::kBigInt)}), logged("b", middle),
          t2->num_parts(), std::vector<std::shared_ptr<JoinBuild>>{inner}, nullptr);
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
// probe without its build prepared, and Next before Open or after Close.
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
// that fails or stops early. A failed Prepare holds nothing while its build exists.
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
        broken_input->num_parts(), std::vector<std::shared_ptr<JoinBuild>>{inner}, nullptr);
    MemoryBudget counted(std::nullopt);
    ExecContext counting = ContextOf(executor, 3, &counted);
    const arrow::Status status = outer->Prepare(counting);
    EXPECT_TRUE(status.IsIOError()) << status.ToString();
    EXPECT_EQ(inner->table(), nullptr);
    EXPECT_EQ(counted.bytes_allocated(), 0);
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
    EXPECT_EQ(MetricOf(build_node, "direct"), 0);  // 112..114 make the range sparse
    for (const char* name : {"part_time", "wait", "lanes_tail", "finish"}) {
      EXPECT_TRUE(MetricOf(build_node, name).has_value()) << name;
    }
    // A probe of 8 rows in windows of 3, 4 of them matching, one residual.
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

}  // namespace
}  // namespace antb1::exec
