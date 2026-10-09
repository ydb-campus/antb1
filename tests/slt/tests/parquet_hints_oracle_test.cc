// io::ParquetTable::part_distinct_count against DuckDB: DuckDB writes a Parquet file under the
// test's temporary directory, with distinct counts in its dictionary-encoded column chunks and none
// in the others, and for every part and field antb1 reads exactly the count that DuckDB's
// parquet_metadata reports for the field's column chunk. A nested field has no chunk of its own and
// no hint; the struct before the other fields shifts their Parquet leaf indices.

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <arrow/api.h>
#include <duckdb.h>
#include <gtest/gtest.h>

#include "antb1/io/parquet_table.h"

#include "duckdb_engine.h"

namespace antb1::slt {
namespace {

namespace fs = std::filesystem;

// `text` as a SQL string literal.
std::string SqlString(std::string_view text) {
  std::string out = "'";
  for (const char c : text) {
    out += c;
    if (c == '\'') {
      out += '\'';
    }
  }
  return out + "'";
}

// Runs `sql` in an in-memory DuckDB database of its own (the oracle of duckdb_engine.h refuses
// COPY), single-threaded and without extension autoinstall or autoload: "" on success, else the
// error.
std::string RunInNewDatabase(const std::string& sql) {
  duckdb_config config = nullptr;
  if (duckdb_create_config(&config) == DuckDBError) {
    return "duckdb: cannot create a config";
  }
  for (const auto& [key, value] :
       {std::pair{"threads", "1"}, std::pair{"autoinstall_known_extensions", "false"},
        std::pair{"autoload_known_extensions", "false"}}) {
    if (duckdb_set_config(config, key, value) == DuckDBError) {
      duckdb_destroy_config(&config);
      return std::string("duckdb: cannot set ") + key;
    }
  }
  duckdb_database db = nullptr;
  char* open_error = nullptr;
  const duckdb_state opened = duckdb_open_ext(nullptr, &db, config, &open_error);
  duckdb_destroy_config(&config);
  if (opened == DuckDBError) {
    std::string message = open_error != nullptr ? open_error : "unknown error";
    duckdb_free(open_error);
    return "duckdb: cannot open a database: " + message;
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

int64_t IntegerOf(const std::string& text) {
  int64_t value = 0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  EXPECT_TRUE(ec == std::errc() && end == text.data() + text.size()) << text;
  return value;
}

TEST(ParquetDistinctCountOracle, HintsAreTheDistinctCountsDuckDbStores) {
  const fs::path dir = fs::path(::testing::TempDir()) / "parquet_hints_oracle";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const std::string path = (dir / "hints.parquet").string();
  // 10000 rows in row groups of 4096 rows: three parts. DuckDB dictionary-encodes the columns of
  // few values (k with NULLs, a VARCHAR, a DECIMAL, a SMALLINT, a DATE and the struct's fields)
  // and stores their distinct counts, NULL not counted; i, whose values all differ, and n, whose
  // values are all NULL, get none.
  const std::string rows =
      "SELECT i, {'x': i % 3, 'y': i % 4} AS st, CASE WHEN i % 2 = 0 THEN i % 7 END AS k, "
      "'v' || (i % 5) AS s, (i % 3)::DECIMAL(5,2) AS d, NULL::INTEGER AS n, "
      "(i % 11)::SMALLINT AS sm, DATE '2020-01-01' + (i % 6)::INTEGER AS dt "
      "FROM range(10000) t(i)";
  ASSERT_EQ(RunInNewDatabase("COPY (" + rows + ") TO " + SqlString(path) +
                             " (FORMAT parquet, ROW_GROUP_SIZE 4096)"),
            "");

  auto duckdb = DuckDbEngine::Make({}, dir, dir);
  ASSERT_TRUE(duckdb.has_value()) << duckdb.error();
  const auto metadata = (*duckdb)->Execute(
      "SELECT row_group_id, path_in_schema, stats_distinct_count FROM parquet_metadata(" +
      SqlString(path) + ") ORDER BY row_group_id, column_id");
  ASSERT_TRUE(metadata.has_value()) << metadata.error().message;
  // (row group, path in schema) -> DuckDB's distinct count, std::nullopt for NULL.
  std::map<std::pair<int64_t, std::string>, std::optional<int64_t>> stored;
  for (const auto& row : metadata->rows) {
    ASSERT_EQ(row.size(), 3U);
    const std::optional<std::string>& count = row[2];
    stored[{IntegerOf(row[0].value_or("")), row[1].value_or("")}] =
        count.has_value() ? std::optional(IntegerOf(count.value())) : std::nullopt;
  }
  ASSERT_EQ(stored.size(), 27U);  // 3 row groups of 9 leaves

  auto table = io::ParquetTable::Open({path});
  ASSERT_TRUE(table.ok()) << table.status().ToString();
  ASSERT_EQ((*table)->num_parts(), 3);
  const arrow::Schema& schema = *(*table)->schema();
  ASSERT_EQ(schema.num_fields(), 8);
  std::size_t hints = 0;
  for (int64_t part = 0; part < (*table)->num_parts(); ++part) {
    for (int field = 0; field < schema.num_fields(); ++field) {
      const std::string& name = schema.field(field)->name();
      const auto it = stored.find({part, name});
      // The struct's leaves are "st, x" and "st, y": no chunk is the field itself.
      EXPECT_EQ(it == stored.end(), name == "st") << name;
      const std::optional<int64_t> want = it != stored.end() ? it->second : std::nullopt;
      EXPECT_EQ((*table)->part_distinct_count(part, field), want)
          << "part " << part << ", field " << name;
      if (want.has_value()) {
        ++hints;
      }
    }
  }
  // Not vacuous: k, s, d, sm and dt have a hint in each of the three parts, i and n none.
  EXPECT_EQ(hints, 15U);
  EXPECT_EQ((*table)->part_distinct_count(0, 2), std::optional<int64_t>(7));  // k
  EXPECT_EQ((*table)->part_distinct_count(2, 3), std::optional<int64_t>(5));  // s
}

}  // namespace
}  // namespace antb1::slt
