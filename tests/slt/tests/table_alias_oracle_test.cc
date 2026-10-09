// sql::Parse against DuckDB itself (parse only, through json_serialize_sql) for every word of
// duckdb_keywords() in every place of a FROM list where a table alias may stand, after a table or a
// derived table: DuckDB reads the word as the item's alias exactly when sql::Parse does. A DuckDB
// update that gives a word a meaning after a FROM item (a new kind of join, say) fails here until
// the word joins the parser's table of words that are never aliases (kNotAnAlias), so that antb1
// never answers such a query as an inner join with an alias. In every place, also after an alias,
// after an ON condition and where the word would call a table function or qualify a name, and as a
// CTE's name or in a column alias list, a statement that sql::Parse accepts must parse in DuckDB,
// and one that it rejects as malformed must fail in DuckDB's parser too. Only a parser error counts
// as a failure to parse: any other error of json_serialize_sql fails the test.

#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "antb1/sql/error.h"
#include "antb1/sql/parser.h"

#include "duckdb_engine.h"

namespace antb1::slt {
namespace {

struct Place {
  std::string_view before;  // the statement up to the word
  std::string_view after;   // the rest of it
  bool alias = true;        // the word stands where an alias may: compare the readings
  std::size_t item = 0;     // the FROM item (0 or 1) whose alias the word would be
};

constexpr auto kPlaces = std::to_array<Place>({
    {.before = "SELECT 1 FROM t ", .after = ""},
    {.before = "SELECT 1 FROM t AS ", .after = ""},
    {.before = "SELECT 1 FROM 'p.parquet' ", .after = " WHERE x = 1"},
    {.before = "SELECT 1 FROM t, u ", .after = "", .item = 1},
    {.before = "SELECT 1 FROM t JOIN u ", .after = " ON t.a = u.a", .item = 1},
    {.before = "SELECT 1 FROM t LEFT JOIN u AS ", .after = " ON t.a = u.a", .item = 1},
    {.before = "SELECT 1 FROM t a ", .after = "", .alias = false},
    {.before = "SELECT 1 FROM t JOIN u ON t.a = u.a ", .after = "", .alias = false},
    // The word as a table function where a FROM item starts: DuckDB takes some reserved words as
    // function names (FROM t, left(1)), so antb1 must not call those calls malformed.
    {.before = "SELECT 1 FROM ", .after = "(1)", .alias = false},
    {.before = "SELECT 1 FROM t, ", .after = "(1)", .alias = false},
    {.before = "SELECT 1 FROM t JOIN ", .after = "(1) f ON t.a = f.a", .alias = false},
    {.before = "SELECT 1 FROM t, LATERAL ", .after = "(1)", .alias = false},
    {.before = "SELECT 1 FROM (", .after = "(1) f CROSS JOIN t)", .alias = false},
    {.before = "SELECT 1 FROM (LATERAL ", .after = "(1) f CROSS JOIN t)", .alias = false},
    // The word as a qualifier where a FROM item starts: DuckDB takes some reserved words for
    // qualifiers (FROM t, over.x), so antb1 must not call those names malformed either.
    {.before = "SELECT 1 FROM t, ", .after = ".x", .alias = false},
    {.before = "SELECT 1 FROM ", .after = ".f(1)", .alias = false},
    {.before = "SELECT 1 FROM t, LATERAL ", .after = ".f(1)", .alias = false},
    {.before = "SELECT 1 FROM (", .after = ".x CROSS JOIN t)", .alias = false},
    {.before = "SELECT 1 FROM (LATERAL ", .after = ".f(1) CROSS JOIN t)", .alias = false},
    // After a derived table, its alias and its column alias list, in that list, and as a CTE's
    // name or column alias: the words that are aliases there are the same.
    {.before = "SELECT 1 FROM (SELECT 1 FROM t) ", .after = ""},
    {.before = "SELECT 1 FROM (SELECT 1 FROM t) AS ", .after = ""},
    {.before = "SELECT 1 FROM t, (SELECT 1 FROM t) ", .after = "", .item = 1},
    {.before = "SELECT 1 FROM (SELECT 1 FROM t) s ", .after = "", .alias = false},
    {.before = "SELECT 1 FROM (SELECT 1 FROM t) AS s(x) ", .after = "", .alias = false},
    {.before = "SELECT 1 FROM (SELECT 1 FROM t) AS s(", .after = ")", .alias = false},
    {.before = "WITH ", .after = " AS (SELECT 1 FROM t) SELECT 1 FROM t", .alias = false},
    {.before = "WITH c(", .after = ") AS (SELECT 1 FROM t) SELECT 1 FROM t", .alias = false},
});

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

std::string Lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

TEST(TableAliasOracle, DuckDbReadsAKeywordAsATableAliasExactlyWhenAntb1Does) {
  const std::filesystem::path dir = std::filesystem::path(::testing::TempDir()) / "alias_oracle";
  std::filesystem::create_directories(dir);
  auto duckdb = DuckDbEngine::Make({}, dir, dir);
  ASSERT_TRUE(duckdb.has_value()) << duckdb.error();
  std::size_t words = 0;
  std::size_t aliases = 0;
  for (const Place& place : kPlaces) {
    SCOPED_TRACE(std::string(place.before) + "<word>" + std::string(place.after));
    const std::string table = place.item == 0 ? "$.statements[0].node.from_table"
                                              : "$.statements[0].node.from_table.right";
    // One row per keyword: the word, whether DuckDB failed, the error's type and the item's alias.
    const std::string sql = "SELECT keyword_name, j->>'$.error', j->>'$.error_type', j->>'" +
                            table + ".alias' FROM (SELECT keyword_name, " + "json_serialize_sql(" +
                            SqlString(place.before) + " || keyword_name || " +
                            SqlString(place.after) +
                            ")::JSON AS j FROM duckdb_keywords()) ORDER BY keyword_name";
    auto result = (*duckdb)->Execute(sql);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_GT(result->rows.size(), 400U);  // DuckDB 1.5.5 has 489 keywords
    for (const auto& row : result->rows) {
      ASSERT_EQ(row.size(), 4U);
      const std::string word = row[0].value_or("");
      SCOPED_TRACE(word);
      const std::string statement = std::string(place.before) + word + std::string(place.after);
      const bool duckdb_parses = row[1] == std::optional<std::string>("false");
      if (!duckdb_parses && row[2] != std::optional<std::string>("parser")) {
        ADD_FAILURE() << statement << ": DuckDB fails with a " << row[2].value_or("NULL")
                      << " error, not a parser error";
        continue;
      }
      const bool duckdb_alias = duckdb_parses && row[3].has_value() && Lower(*row[3]) == word;
      auto ours = sql::Parse(statement);
      if (place.alias) {
        const bool our_alias = ours.has_value() && place.item < ours->from.size() &&
                               ours->from[place.item].alias.has_value() &&
                               Lower(*ours->from[place.item].alias) == word;
        EXPECT_EQ(our_alias, duckdb_alias) << statement;
        aliases += our_alias ? 1U : 0U;
      }
      if (ours.has_value()) {
        EXPECT_TRUE(duckdb_parses) << statement << " parses in antb1 only";
      } else if (ours.error().kind == sql::ParseError::Kind::kSyntax) {
        EXPECT_FALSE(duckdb_parses) << statement << ": " << ours.error().message;
      }
      ++words;
    }
  }
  EXPECT_GT(words, kPlaces.size() * 400);
  // The words that are aliases in neither engine are 105 of 489 in DuckDB 1.5.5.
  EXPECT_GT(aliases, 9U * 300U);
}

}  // namespace
}  // namespace antb1::slt
