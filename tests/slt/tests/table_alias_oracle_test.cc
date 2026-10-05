// sql::Parse against DuckDB itself (parse only, through json_serialize_sql) for every word of
// duckdb_keywords() in every place of a FROM list where a table alias may stand: DuckDB reads the
// word as the item's alias exactly when sql::Parse does. A DuckDB update that gives a word a
// meaning after a FROM item (a new kind of join, say) fails here until the word joins the parser's
// table of words that are never aliases (kNotAnAlias), so that antb1 never answers such a query
// as an inner join with an alias. In every place, also after an alias and after an ON condition,
// a statement that sql::Parse accepts must parse in DuckDB, and one that it rejects as malformed
// must fail in DuckDB too.

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
    // One row per keyword: the word, whether DuckDB failed to parse, and the item's alias.
    const std::string sql = "SELECT keyword_name, j->>'$.error', j->>'" + table +
                            ".alias' FROM (SELECT keyword_name, " + "json_serialize_sql(" +
                            SqlString(place.before) + " || keyword_name || " +
                            SqlString(place.after) +
                            ")::JSON AS j FROM duckdb_keywords()) ORDER BY keyword_name";
    auto result = (*duckdb)->Execute(sql);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_GT(result->rows.size(), 400U);  // DuckDB 1.5.5 has 489 keywords
    for (const auto& row : result->rows) {
      ASSERT_EQ(row.size(), 3U);
      const std::string word = row[0].value_or("");
      SCOPED_TRACE(word);
      const bool duckdb_parses = row[1] == std::optional<std::string>("false");
      const bool duckdb_alias = duckdb_parses && row[2].has_value() && Lower(*row[2]) == word;
      const std::string statement = std::string(place.before) + word + std::string(place.after);
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
  EXPECT_GT(aliases, 6U * 300U);
}

}  // namespace
}  // namespace antb1::slt
