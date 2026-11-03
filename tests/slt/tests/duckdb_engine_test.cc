// The DuckDB adapter (duckdb_engine.h) against the DuckDB library: a DECIMAL is an exact D column
// named DECIMAL(p,s), and its canonical text is the text DuckDB prints, at every physical width
// DuckDB stores a DECIMAL in (16, 32, 64 and 128 bits). A memory limit is DuckDB's memory_limit,
// locked with the rest of the configuration, beyond which DuckDB spills to the temp dir.

#include "duckdb_engine.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <duckdb.h>
#include <gtest/gtest.h>

#include "engine.h"
#include "result_diff.h"

namespace antb1::slt {
namespace {

using Row = std::vector<std::optional<std::string>>;

class DuckDbEngineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const std::filesystem::path dir =
        std::filesystem::path(::testing::TempDir()) / "duckdb_engine_test";
    std::filesystem::create_directories(dir);
    auto engine = DuckDbEngine::Make({}, dir, dir);
    ASSERT_TRUE(engine.has_value()) << engine.error();
    duckdb_ = *std::move(engine);
  }

  ResultSet Run(const std::string& sql) {
    auto result = duckdb_->Execute(sql);
    EXPECT_TRUE(result.has_value()) << sql << "\n"
                                    << (result.has_value() ? "" : result.error().message);
    return result.value_or(ResultSet{});
  }

  std::unique_ptr<DuckDbEngine> duckdb_;
};

// `digits` / 10^scale with an integer digit before the point ("0.05"), which DuckDB reads into a
// DECIMAL of any width, negated if `negative`.
std::string Text(std::string digits, std::size_t scale, bool negative) {
  if (digits.size() <= scale) {
    digits.insert(0, scale + 1 - digits.size(), '0');
  }
  if (scale > 0) {
    digits.insert(digits.size() - scale, ".");
  }
  return (negative ? "-" : "") + digits;
}

TEST_F(DuckDbEngineTest, DecimalsAreExactColumnsNamedWithWidthAndScale) {
  const ResultSet r = Run(
      "SELECT CAST(17 AS DECIMAL(15,2)), CAST(-0.25 AS DECIMAL(15,2)), CAST(0.5 AS DECIMAL(3,3)), "
      "CAST(-0.5 AS DECIMAL(3,3)), CAST(-12 AS DECIMAL(4,0)), 0.5, CAST(NULL AS DECIMAL(5,1)), "
      "CAST(1 AS HUGEINT), CAST(0.5 AS DOUBLE)");
  EXPECT_EQ(Letters(r), "DDDDDDDIR");
  EXPECT_EQ(r.type_names,
            (std::vector<std::string>{"DECIMAL(15,2)", "DECIMAL(15,2)", "DECIMAL(3,3)",
                                      "DECIMAL(3,3)", "DECIMAL(4,0)", "DECIMAL(2,1)",
                                      "DECIMAL(5,1)", "HUGEINT", "DOUBLE"}));
  EXPECT_EQ(r.rows, (std::vector<Row>{{"17.00", "-0.25", ".500", "-.500", "-12", "0.5",
                                       std::nullopt, "1", "0.5"}}));
}

TEST_F(DuckDbEngineTest, DecimalTextIsDuckDbsVarcharText) {
  // The edges of every physical width (4, 9, 18 and 38 digits), scales 0, 1, half and all of the
  // width, and the values 0, one unit, a mix of digits and the largest, with both signs. The text
  // must be DuckDB's own CAST(d AS VARCHAR): long runs of ties compare DECIMALs as that text
  // (ordered_compare.h).
  for (const std::size_t width : {1U, 4U, 5U, 9U, 10U, 18U, 19U, 38U}) {
    for (const std::size_t scale : std::set<std::size_t>{0, 1, width / 2, width}) {
      const std::string type = std::format("DECIMAL({},{})", width, scale);
      std::string values;
      for (const std::string& digits :
           {std::string("0"), std::string("1"),
            std::string("12345678901234567890123456789012345678").substr(0, width),
            std::string(width, '9')}) {
        for (const bool negative : {false, true}) {
          values += std::format("{}(CAST('{}' AS {}))", values.empty() ? "" : ", ",
                                Text(digits, scale, negative), type);
        }
      }
      const ResultSet r =
          Run(std::format("SELECT d, CAST(d AS VARCHAR) FROM (VALUES {}) AS t(d)", values));
      EXPECT_EQ(Letters(r), "DT") << type;
      ASSERT_FALSE(r.type_names.empty()) << type;
      EXPECT_EQ(r.type_names[0], type);
      ASSERT_EQ(r.rows.size(), 8U) << type;
      for (const Row& row : r.rows) {
        EXPECT_EQ(row[0], row[1]) << type;
      }
    }
  }
}

// A directory of its own under TempDir() for each test: ctest runs them in parallel.
std::filesystem::path TestDir(std::string_view name) {
  const std::filesystem::path dir = std::filesystem::path(::testing::TempDir()) / name;
  std::filesystem::create_directories(dir);
  return dir;
}

constexpr int64_t kMiB = int64_t{1024} * 1024;

TEST(DuckDbEngineMemoryLimitTest, IsDuckDbsMemoryLimitLockedWithTheRest) {
  const std::filesystem::path dir = TestDir("duckdb_engine_memory_limit");
  auto engine = DuckDbEngine::Make({}, dir, dir, 64 * kMiB);
  ASSERT_TRUE(engine.has_value()) << engine.error();
  const ExecResult setting = (*engine)->Execute("SELECT current_setting('memory_limit')");
  ASSERT_TRUE(setting.has_value()) << setting.error().message;
  EXPECT_EQ(setting->rows, (std::vector<Row>{{"64.0 MiB"}}));
  const ExecResult set = (*engine)->Execute("SET memory_limit = '1GiB'");
  ASSERT_FALSE(set.has_value());
  EXPECT_TRUE(set.error().message.contains("the configuration has been locked"))
      << set.error().message;
}

// Beyond its memory limit DuckDB spills to the temp dir. With a temp dir DuckDB cannot create (one
// below a regular file), a DISTINCT of a million keys answers under the limit, and one of ten
// million, whose hash table outgrows it, fails; with its temp dir, the same limit answers the
// larger one too.
TEST(DuckDbEngineMemoryLimitTest, SpillsToTheTempDirBeyondIt) {
  const std::filesystem::path dir = TestDir("duckdb_engine_spill");
  std::ofstream(dir / "file") << "not a directory\n";
  const auto distinct = [](int64_t keys) {
    return std::format("SELECT COUNT(*) FROM (SELECT DISTINCT i FROM range({}) t(i))", keys);
  };
  auto blocked = DuckDbEngine::Make({}, dir, dir / "file" / "tmp", 64 * kMiB);
  ASSERT_TRUE(blocked.has_value()) << blocked.error();
  const ExecResult fits = (*blocked)->Execute(distinct(1'000'000));
  ASSERT_TRUE(fits.has_value()) << fits.error().message;
  EXPECT_EQ(fits->rows, (std::vector<Row>{{"1000000"}}));
  EXPECT_FALSE((*blocked)->Execute(distinct(10'000'000)).has_value())
      << "the query fits the limit, so it shows no spill";
  auto engine = DuckDbEngine::Make({}, dir, dir / "tmp", 64 * kMiB);
  ASSERT_TRUE(engine.has_value()) << engine.error();
  const ExecResult answered = (*engine)->Execute(distinct(10'000'000));
  ASSERT_TRUE(answered.has_value()) << answered.error().message;
  EXPECT_EQ(answered->rows, (std::vector<Row>{{"10000000"}}));
}

TEST(DuckDbEngineMemoryLimitTest, IsAtLeastOneByte) {
  const std::filesystem::path dir = TestDir("duckdb_engine_bad_memory_limit");
  for (const int64_t bytes : {int64_t{0}, int64_t{-1}}) {
    const auto engine = DuckDbEngine::Make({}, dir, dir, bytes);
    ASSERT_FALSE(engine.has_value()) << bytes;
    EXPECT_EQ(engine.error(),
              std::format("duckdb: the memory limit must be at least 1 byte, not {}", bytes));
  }
}

// The canonical text of every cell of `result`, one per line; "" with a test failure for an error.
std::string Cells(const ExecResult& result, std::string_view sql) {
  EXPECT_TRUE(result.has_value()) << sql << "\n"
                                  << (result.has_value() ? "" : result.error().message);
  std::string text;
  for (const Row& row : result.value_or(ResultSet{}).rows) {
    for (const auto& cell : row) {
      text += cell.value_or("NULL") + "\n";
    }
  }
  return text;
}

// The differential test's retries (difftest.h), in the order it asks them, run fewer optimizers:
// without join_order a comma join is still a hash join, in the written order, and with
// filter_pushdown alone too, where without it the join would stay a cross product.
TEST(DuckDbEngineOptimizersTest, RetriesRunFewerOptimizers) {
  const std::filesystem::path dir = TestDir("duckdb_engine_optimizers");
  auto oracle = DuckDbEngine::Make({}, dir, dir);
  ASSERT_TRUE(oracle.has_value()) << oracle.error();
  auto retries = DuckDbEngine::MakeRetries({}, dir, dir);
  ASSERT_TRUE(retries.has_value()) << retries.error();
  ASSERT_EQ(retries->size(), 2U);
  const auto run = [](DuckDbEngine& engine, const std::string& sql) {
    return Cells(engine.Execute(sql), sql);
  };
  const std::string disabled = "SELECT current_setting('disabled_optimizers')";
  EXPECT_EQ(run(**oracle, disabled), "late_materialization\n");
  EXPECT_EQ(run(*(*retries)[0], disabled), "join_order,late_materialization\n");
  const std::string filter_pushdown_only = run(*(*retries)[1], disabled);
  for (const std::string_view off :
       {"join_order", "late_materialization", "statistics_propagation", "build_side_probe_side"}) {
    EXPECT_TRUE(filter_pushdown_only.contains(off)) << off << " in " << filter_pushdown_only;
  }
  EXPECT_FALSE(("," + filter_pushdown_only.substr(0, filter_pushdown_only.size() - 1) + ",")
                   .contains(",filter_pushdown,"))
      << filter_pushdown_only;
  const std::string plan =
      "EXPLAIN SELECT COUNT(*) FROM range(5) AS a(i), range(5) AS b(j) WHERE i = j";
  for (DuckDbEngine* engine : {oracle->get(), (*retries)[0].get(), (*retries)[1].get()}) {
    const std::string text = run(*engine, plan);
    EXPECT_TRUE(text.contains("HASH_JOIN")) << text;
    EXPECT_FALSE(text.contains("CROSS_PRODUCT")) << text;
  }
}

// Runs `sql` in an in-memory DuckDB database of its own (the oracle refuses COPY): "" on success,
// else the error.
std::string RunInNewDatabase(const std::string& sql) {
  duckdb_database db = nullptr;
  if (duckdb_open(nullptr, &db) == DuckDBError) {
    return "duckdb: cannot open a database";
  }
  std::string error;
  duckdb_connection conn = nullptr;
  if (duckdb_connect(db, &conn) == DuckDBError) {
    error = "duckdb: cannot connect";
  } else {
    duckdb_result result{};
    if (duckdb_query(conn, sql.c_str(), &result) == DuckDBError) {
      const char* text = duckdb_result_error(&result);
      error = text != nullptr ? text : "unknown error";
    }
    duckdb_destroy_result(&result);
    duckdb_disconnect(&conn);
  }
  duckdb_close(&db);
  return error;
}

// DuckDB 1.5.6 fails a join of three tables with an OR across two of them under a GROUP BY with an
// internal error, as in a nightly differential test of 2026-11-02, while its retries answer it.
// When the oracle answers too, DuckDB has fixed the bug: drop the first expectation.
TEST(DuckDbEngineOptimizersTest, RetriesAnswerAQueryTheOracleFailsWithAnInternalError) {
  const std::filesystem::path dir = TestDir("duckdb_engine_retries");
  std::vector<TableDef> tables;
  for (const auto& [name, rows] :
       {std::pair{"r", "SELECT i AS rd_id, (i % 3 + 1)::SMALLINT AS rd_city FROM range(240) t(i)"},
        std::pair{"z",
                  "SELECT (i + 1)::INTEGER AS zn_id, (i % 5 + 1)::SMALLINT AS zn_city "
                  "FROM range(40) t(i)"},
        std::pair{"c",
                  "SELECT (i + 1)::SMALLINT AS ct_id, 'l' || i AS label FROM range(6) t(i)"}}) {
    const std::string path = (dir / (std::string(name) + ".parquet")).string();
    ASSERT_EQ(RunInNewDatabase(std::format("COPY ({}) TO '{}' (FORMAT parquet)", rows, path)), "");
    tables.push_back(TableDef{.name = name, .files = {path}});
  }
  const std::string sql =
      "SELECT c.label, COUNT(*) FROM r, z, c WHERE c.ct_id = r.rd_city AND z.zn_city = c.ct_id AND "
      "(c.label <> 'x' OR z.zn_city > 2) GROUP BY c.label ORDER BY c.label";
  auto oracle = DuckDbEngine::Make(tables, dir, dir);
  ASSERT_TRUE(oracle.has_value()) << oracle.error();
  const ExecResult failed = (*oracle)->Execute(sql);
  ASSERT_FALSE(failed.has_value()) << "DuckDB answers: drop this expectation";
  EXPECT_EQ(failed.error().kind, "INTERNAL Error") << failed.error().message;
  auto retries = DuckDbEngine::MakeRetries(tables, dir, dir);
  ASSERT_TRUE(retries.has_value()) << retries.error();
  for (const auto& retry : *retries) {
    EXPECT_EQ(Cells(retry->Execute(sql), sql), "l0\n640\nl1\n640\nl2\n640\n");
  }
}

// A query that fails with an internal error while it runs leaves DuckDB's database invalidated, so
// the engine opens it again, with its views and settings, and the next query answers. With
// filter_pushdown alone, DuckDB 1.5.6 fails this IN over an aggregate while it runs; when it
// answers, DuckDB has fixed that bug: find another.
TEST(DuckDbEngineOptimizersTest, AnInternalErrorOpensTheDatabaseAgain) {
  const std::filesystem::path dir = TestDir("duckdb_engine_reopen");
  auto engine =
      DuckDbEngine::Make({}, dir, dir, std::nullopt, DuckDbEngine::Optimizers::kFilterPushdownOnly);
  ASSERT_TRUE(engine.has_value()) << engine.error();
  const ExecResult failed =
      (*engine)->Execute("SELECT 0 FROM range(3) t(i) HAVING 0 IN (SELECT 0 FROM range(3) s(j))");
  ASSERT_FALSE(failed.has_value()) << "DuckDB answers: find another such query";
  EXPECT_EQ(failed.error().kind, "INTERNAL Error") << failed.error().message;
  EXPECT_EQ(Cells((*engine)->Execute("SELECT 42"), "SELECT 42"), "42\n");
  const std::string disabled = "SELECT current_setting('disabled_optimizers')";
  EXPECT_TRUE(Cells((*engine)->Execute(disabled), disabled).contains("join_order"))
      << "the same settings";
  const ExecResult locked = (*engine)->Execute("SET threads = 4");
  ASSERT_FALSE(locked.has_value());
  EXPECT_TRUE(locked.error().message.contains("the configuration has been locked"))
      << locked.error().message;
}

}  // namespace
}  // namespace antb1::slt
