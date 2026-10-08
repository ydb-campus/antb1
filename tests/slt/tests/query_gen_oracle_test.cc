// Generated joins against DuckDB's parser (parse only, through json_serialize_sql): for joins of
// the target grammar over the tables of join_tables.h, DuckDB parses the query and its canonical
// form (sql::ToSql of sql::Parse), and json_deserialize_sql gives both the same text. That
// normalization collapses what the canonical form respells (comments, case, quoting, AS, JOIN as
// INNER JOIN, the unary minus, casts), so a difference is a statement that antb1's parser and
// DuckDB's read differently. One respelling it keeps: an ORDER BY item with ASC written, which
// sql::OrderItem does not record and DuckDB orders as one without it (default_order); its JSON
// counts as the default here. No table is read.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "antb1/sql/parser.h"
#include "antb1/sql/unparse.h"

#include "duckdb_engine.h"
#include "join_tables.h"
#include "query_gen.h"
#include "supported_features.h"

namespace antb1::slt {
namespace {

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

// How DuckDB reads `sql`: the text json_deserialize_sql makes of its json_serialize_sql, or why
// DuckDB cannot parse it.
std::expected<std::string, std::string> DuckDbReading(DuckDbEngine& duckdb,
                                                      const std::string& sql) {
  const std::string serialized = "json_serialize_sql(" + SqlString(sql) + ")";
  auto parsed = duckdb.Execute("SELECT j->>'$.error', j->>'$.error_message' FROM (SELECT " +
                               serialized + "::JSON AS j)");
  if (!parsed.has_value()) {
    return std::unexpected(parsed.error().message);
  }
  if (parsed->rows.size() != 1 || parsed->rows[0].size() != 2) {
    return std::unexpected("json_serialize_sql gave no single row");
  }
  if (parsed->rows[0][0] != std::optional<std::string>("false")) {
    return std::unexpected("DuckDB cannot parse it: " + parsed->rows[0][1].value_or("?"));
  }
  auto text =
      duckdb.Execute("SELECT json_deserialize_sql(replace(" + serialized +
                     R"(::VARCHAR, '"type":"ASCENDING"', '"type":"ORDER_DEFAULT"')::JSON))");
  if (!text.has_value()) {
    return std::unexpected(text.error().message);
  }
  if (text->rows.size() != 1 || text->rows[0].size() != 1) {
    return std::unexpected("json_deserialize_sql gave no single row");
  }
  const std::optional<std::string>& value = text->rows[0][0];
  if (!value.has_value()) {
    return std::unexpected("json_deserialize_sql gave NULL");
  }
  return *value;
}

TEST(QueryGenOracle, DuckDbReadsEveryGeneratedJoinAndItsCanonicalFormAlike) {
  const std::filesystem::path dir =
      std::filesystem::path(::testing::TempDir()) / "query_gen_oracle";
  std::filesystem::create_directories(dir);
  auto duckdb = DuckDbEngine::Make({}, dir, dir);
  ASSERT_TRUE(duckdb.has_value()) << duckdb.error();
  const auto gen = QueryGenerator::Make(JoinTables(), 41,
                                        {.supported = kSupportedFeatures, .target_percent = 100});
  ASSERT_TRUE(gen.has_value()) << gen.error();
  int joins = 0;
  for (uint64_t i = 0; joins < 1500 && i < 10'000; ++i) {
    const GeneratedQuery q = gen->Generate(i);
    if (!q.features.Has(Feature::kCommaJoin) && !q.features.Has(Feature::kJoinOn)) {
      continue;
    }
    ++joins;
    SCOPED_TRACE(q.sql);
    const auto ast = sql::Parse(q.sql);
    ASSERT_TRUE(ast.has_value()) << ast.error().message;
    const std::string canonical = sql::ToSql(*ast);
    const auto original = DuckDbReading(**duckdb, q.sql);
    const auto again = DuckDbReading(**duckdb, canonical);
    ASSERT_TRUE(original.has_value()) << original.error();
    ASSERT_TRUE(again.has_value()) << canonical << "\n  " << again.error();
    EXPECT_EQ(*original, *again) << "canonical form: " << canonical;
  }
  EXPECT_EQ(joins, 1500);
}

}  // namespace
}  // namespace antb1::slt
