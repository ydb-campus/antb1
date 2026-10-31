// Generated joins against DuckDB's parser (parse only, through json_serialize_sql): for joins of
// the target grammar over the tables of join_tables.h, DuckDB parses the query and its canonical
// form (sql::ToSql of sql::Parse), and json_deserialize_sql gives both the same text. That
// normalization collapses what the canonical form respells (comments, case, quoting, AS, JOIN as
// INNER JOIN, the unary minus, casts), so a difference is a statement that antb1's parser and
// DuckDB's read differently. One respelling it keeps: an ORDER BY item with ASC written, which
// sql::OrderItem does not record and DuckDB orders as one without it (default_order); its JSON
// counts as the default here. A fixed list does the same for the compounds the generator never
// writes (ADR 0036): at the top level, with a WITH list or a tail, and as a subquery's query. No
// table is read.

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
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
  int unions = 0;  // of them compounds (ADR 0036), always a nested block's query
  for (uint64_t i = 0; joins < 1500 && i < 10'000; ++i) {
    const GeneratedQuery q = gen->Generate(i);
    if (!q.features.Has(Feature::kCommaJoin) && !q.features.Has(Feature::kJoinOn) &&
        !q.features.Has(Feature::kLeftJoin) && !q.features.Has(Feature::kDerivedTable) &&
        !q.features.Has(Feature::kCommonTableExpr)) {
      continue;
    }
    ++joins;
    unions += q.features.Has(Feature::kUnionAll) ? 1 : 0;
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
  EXPECT_GT(unions, 0) << "compounds";
}

// Compounds over the tables of join_tables.h that the generator never writes: the WITH list and
// the tail (ORDER BY, LIMIT, OFFSET) are the top-level compound's, a derived table's compound has a
// tail of its own, and a compound is the query of each subquery form.
constexpr auto kCompounds = std::to_array<std::string_view>({
    "WITH c AS (SELECT d_id FROM dims) SELECT d_id FROM c UNION ALL SELECT f_dim FROM facts "
    "ORDER BY 1 LIMIT 2 OFFSET 1",
    "SELECT f_id FROM facts UNION ALL SELECT d_id FROM dims UNION ALL SELECT s_dim FROM slots "
    "LIMIT 3",
    "SELECT x FROM (SELECT d_id AS x FROM dims UNION ALL SELECT f_dim FROM facts ORDER BY x DESC "
    "LIMIT 5 OFFSET 2) AS s",
    "SELECT f_id FROM facts WHERE f_dim IN (SELECT d_id FROM dims UNION ALL SELECT d_parent FROM "
    "dims)",
    "SELECT f_id FROM facts WHERE EXISTS (SELECT d_id FROM dims UNION ALL SELECT c_rank FROM "
    "codes)",
    "SELECT f_id FROM facts WHERE f_dim > (SELECT MAX(d_id) FROM dims UNION ALL SELECT MAX(f_dim) "
    "FROM facts ORDER BY 1 DESC LIMIT 1)",
});

// Stars among the select items, which the generator does not write: quoted, spaced and repeated
// stars, a qualified star over a derived table and over a CTE, and stars in an EXISTS block, an IN
// block and UNION ALL branches.
constexpr auto kStars = std::to_array<std::string_view>({
    R"(SELECT "D".*, *, f_id FROM dims AS "D" JOIN facts ON "D".d_id = facts.f_dim)",
    "SELECT dims . /* all */ *, d_id FROM dims",
    "SELECT *, *, d_id AS x FROM dims",
    "SELECT s.*, 1 FROM (SELECT d_id, d_parent FROM dims) AS s",
    "WITH c AS (SELECT d_id FROM dims) SELECT c.*, * FROM c",
    "SELECT f_id FROM facts WHERE EXISTS (SELECT dims.*, 1 FROM dims WHERE dims.d_id = "
    "facts.f_dim)",
    "SELECT f_id FROM facts WHERE f_dim IN (SELECT q.* FROM (SELECT d_id FROM dims) AS q)",
    "SELECT d_id, * FROM dims UNION ALL SELECT *, d_id FROM dims",
});

// DuckDB reads every query of `queries` and antb1's canonical form of it alike.
void ExpectDuckDbReadsTheCanonicalFormsAlike(std::string_view name,
                                             std::span<const std::string_view> queries) {
  const std::filesystem::path dir = std::filesystem::path(::testing::TempDir()) / name;
  std::filesystem::create_directories(dir);
  auto duckdb = DuckDbEngine::Make({}, dir, dir);
  ASSERT_TRUE(duckdb.has_value()) << duckdb.error();
  for (const std::string_view text : queries) {
    const std::string sql(text);
    SCOPED_TRACE(sql);
    const auto ast = sql::Parse(sql);
    ASSERT_TRUE(ast.has_value()) << ast.error().message;
    const std::string canonical = sql::ToSql(*ast);
    const auto original = DuckDbReading(**duckdb, sql);
    const auto again = DuckDbReading(**duckdb, canonical);
    ASSERT_TRUE(original.has_value()) << original.error();
    ASSERT_TRUE(again.has_value()) << canonical << "\n  " << again.error();
    EXPECT_EQ(*original, *again) << "canonical form: " << canonical;
  }
}

TEST(QueryGenOracle, DuckDbReadsEveryFixedCompoundAndItsCanonicalFormAlike) {
  ExpectDuckDbReadsTheCanonicalFormsAlike("query_gen_oracle_compounds", kCompounds);
}

TEST(QueryGenOracle, DuckDbReadsEveryFixedStarFormAndItsCanonicalFormAlike) {
  ExpectDuckDbReadsTheCanonicalFormsAlike("query_gen_oracle_stars", kStars);
}

}  // namespace
}  // namespace antb1::slt
