// The comparison of a query with ORDER BY (ordered_compare.h): the augmented oracle query, ties in
// any order, ties at the edges of a LIMIT/OFFSET window, and the growing oracle limit.

#include "ordered_compare.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "engine.h"
#include "result_diff.h"

namespace antb1::slt {
namespace {

using Row = std::vector<std::optional<std::string>>;

ResultSet Result(std::vector<ColumnClass> classes, std::vector<Row> rows) {
  std::vector<std::string> names(classes.size(), "BIGINT");
  return ResultSet{
      .classes = std::move(classes), .type_names = std::move(names), .rows = std::move(rows)};
}

constexpr auto kI = ColumnClass::kInteger;
constexpr auto kR = ColumnClass::kReal;
constexpr auto kT = ColumnClass::kText;
constexpr auto kD = ColumnClass::kDecimal;

TEST(MakeOrderedQuery, AppendsTheKeysAndDropsLimitAndOffset) {
  const auto q = MakeOrderedQuery(
      "select a as k, count(*) as c from t group by a order by c desc, K nulls first, max(b) "
      "limit 5 offset 2");
  ASSERT_TRUE(q.has_value());
  const OrderedQuery ordered = q.value_or(OrderedQuery{});
  EXPECT_EQ(ordered.augmented_sql,
            "SELECT a AS \"k\", COUNT(*) AS \"c\", COUNT(*) AS \"__antb1_key0\", a AS "
            "\"__antb1_key1\", MAX(b) AS \"__antb1_key2\" FROM t GROUP BY a ORDER BY c DESC, K "
            "NULLS FIRST, MAX(b)");
  EXPECT_EQ(ordered.keys, 3U);
  EXPECT_EQ(ordered.limit, 5);
  EXPECT_EQ(ordered.offset, 2);
  EXPECT_EQ(WithLimit(ordered, 7), ordered.augmented_sql + " LIMIT 7");
  // The last select item with an alias wins; SELECT * keeps the star.
  EXPECT_EQ(MakeOrderedQuery("SELECT a AS x, b AS x FROM t ORDER BY x")
                .value_or(OrderedQuery{})
                .augmented_sql,
            "SELECT a AS \"x\", b AS \"x\", b AS \"__antb1_key0\" FROM t ORDER BY x");
  EXPECT_EQ(MakeOrderedQuery("SELECT * FROM t WHERE a > 1 ORDER BY b OFFSET 3")
                .value_or(OrderedQuery{})
                .augmented_sql,
            "SELECT *, b AS \"__antb1_key0\" FROM t WHERE a > 1 ORDER BY b");
  EXPECT_EQ(MakeOrderedQuery("SELECT a FROM t LIMIT 5"), std::nullopt) << "no ORDER BY";
  EXPECT_EQ(MakeOrderedQuery("SELECT a FROM t ORDER BY a || 'x'"), std::nullopt) << "not parsed";
  EXPECT_EQ(MakeOrderedQuery("SELECT a FROM t ORDER BY a + 1 DESC")
                .value_or(OrderedQuery{})
                .augmented_sql,
            "SELECT a, a + 1 AS \"__antb1_key0\" FROM t ORDER BY a + 1 DESC")
      << "an expression key";
}

// The oracle side of `SELECT v FROM t ORDER BY k [LIMIT/OFFSET]` for (v, k) rows in DuckDB's order.
class Oracle {
 public:
  Oracle(std::vector<ColumnClass> classes, std::vector<Row> ranked)
      : classes_(std::move(classes)), ranked_(std::move(ranked)) {}

  // Answers "q" and "q LIMIT <n>" (the augmented query of Query()).
  ExecResult operator()(const std::string& sql) {
    ++runs_;
    std::optional<int64_t> limit;
    if (sql.starts_with("q LIMIT ")) {
      limit = std::stoll(sql.substr(8));
    } else if (sql != "q") {
      return std::unexpected(EngineError{.kind = "Parser", .message = "unexpected: " + sql});
    }
    limits_.push_back(limit);
    std::vector<Row> rows = ranked_;
    if (limit.has_value() && std::cmp_less(*limit, rows.size())) {
      rows.resize(static_cast<std::size_t>(*limit));
    }
    return Result(classes_, rows);
  }
  // The answer of the query itself: the ranked rows in [offset, offset + limit), without the key.
  [[nodiscard]] ResultSet Answer(std::size_t width, std::size_t offset,
                                 std::optional<std::size_t> limit) const {
    std::vector<Row> rows;
    for (std::size_t i = offset; i < ranked_.size() && (!limit || i < offset + *limit); ++i) {
      rows.emplace_back(ranked_[i].begin(),
                        ranked_[i].begin() + static_cast<std::ptrdiff_t>(width));
    }
    return Result(std::vector<ColumnClass>(classes_.begin(),
                                           classes_.begin() + static_cast<std::ptrdiff_t>(width)),
                  rows);
  }

  int runs_ = 0;
  std::vector<std::optional<int64_t>> limits_;

 private:
  std::vector<ColumnClass> classes_;
  std::vector<Row> ranked_;
};

OrderedQuery Query(std::optional<int64_t> limit, int64_t offset) {
  return OrderedQuery{.augmented_sql = "q", .keys = 1, .limit = limit, .offset = offset};
}

// Ranked (value, key) rows: keys 1, 2, 2, 2, 3.
Oracle Ranked() {
  return Oracle({kT, kI}, {{"a", "1"}, {"b", "2"}, {"c", "2"}, {"d", "2"}, {"e", "3"}});
}

TEST(CompareOrdered, TiesComeInAnyOrder) {
  Oracle oracle = Ranked();
  const auto query = Query(std::nullopt, 0);
  const auto answer = oracle.Answer(1, 0, std::nullopt);
  EXPECT_FALSE(CompareOrdered(answer, answer, query, std::ref(oracle)));
  EXPECT_EQ(oracle.runs_, 0) << "an equal answer needs no augmented query";
  EXPECT_FALSE(CompareOrdered(answer, Result({kT}, {{"a"}, {"d"}, {"b"}, {"c"}, {"e"}}), query,
                              std::ref(oracle)));
  EXPECT_EQ(oracle.limits_, (std::vector<std::optional<int64_t>>{std::nullopt}));
  // Out of order across different keys, a row twice, a foreign row.
  for (const std::vector<Row>& rows :
       std::vector<std::vector<Row>>{{{"b"}, {"a"}, {"c"}, {"d"}, {"e"}},
                                     {{"a"}, {"b"}, {"b"}, {"d"}, {"e"}},
                                     {{"a"}, {"b"}, {"c"}, {"x"}, {"e"}}}) {
    const auto d = CompareOrdered(answer, Result({kT}, rows), query, std::ref(oracle));
    ASSERT_TRUE(d.has_value());
    EXPECT_TRUE(d.value_or(Discrepancy{}).mismatch);
  }
}

// Tied rows whose R values differ by less than the tolerance but are not equal: every antb1 row
// must find its own oracle row. A greedy first-fit match within the tolerance let the first antb1
// row take a neighbouring oracle value, and a later row then had none left: a false mismatch (the
// ClickBench-shaped LocalEventTime / 4 steps by 0.25, about the tolerance at 3.4e8). Exact matches
// come first now; a foreign value still fails.
TEST(CompareOrdered, CloseValuesFindTheirOwnRows) {
  Oracle oracle(
      {kR, kI},
      {{"1000000000", "1"}, {"1000000000.75", "1"}, {"1000000001.5", "1"}, {"1000000002.25", "1"}});
  const auto query = Query(std::nullopt, 0);
  const auto answer = oracle.Answer(1, 0, std::nullopt);
  // Not equal position by position (1000000001.5 against 1000000000), a right order of the ties.
  EXPECT_FALSE(CompareOrdered(
      answer,
      Result({kR}, {{"1000000000.75"}, {"1000000001.5"}, {"1000000000"}, {"1000000002.25"}}), query,
      std::ref(oracle)));
  // Within the tolerance of its own row (rounding), still right.
  EXPECT_FALSE(CompareOrdered(
      answer,
      Result({kR}, {{"1000000000.7500001"}, {"1000000001.5"}, {"1000000000"}, {"1000000002.25"}}),
      query, std::ref(oracle)));
  // No row equal to its oracle row at all (each rounded), in a right order of the ties: the rows
  // left for the tolerance are matched by value (each to the smallest oracle value within its
  // tolerance, both in ascending order), not first fit.
  EXPECT_FALSE(CompareOrdered(answer,
                              Result({kR}, {{"1000000000.7500001"},
                                            {"1000000001.5000001"},
                                            {"1000000000.0000001"},
                                            {"1000000002.2500001"}}),
                              query, std::ref(oracle)));
  const auto d = CompareOrdered(
      answer, Result({kR}, {{"1000000000.75"}, {"1000000001.5"}, {"1000000000"}, {"1000000009"}}),
      query, std::ref(oracle));
  ASSERT_TRUE(d.has_value());
  EXPECT_TRUE(d.value_or(Discrepancy{}).mismatch);
}

TEST(CompareOrdered, AnyTiedRowsAtTheWindowEdges) {
  Oracle oracle = Ranked();
  // LIMIT 2 OFFSET 2: ranks 2 and 3 hold key 2, whose run spans ranks 1 to 3.
  const auto query = Query(2, 2);
  const auto answer = oracle.Answer(1, 2, 2);
  EXPECT_FALSE(CompareOrdered(answer, Result({kT}, {{"b"}, {"c"}}), query, std::ref(oracle)));
  EXPECT_FALSE(CompareOrdered(answer, Result({kT}, {{"d"}, {"b"}}), query, std::ref(oracle)));
  EXPECT_TRUE(CompareOrdered(answer, Result({kT}, {{"a"}, {"b"}}), query, std::ref(oracle)));
  EXPECT_TRUE(CompareOrdered(answer, Result({kT}, {{"b"}, {"e"}}), query, std::ref(oracle)));
  EXPECT_TRUE(CompareOrdered(answer, Result({kT}, {{"b"}}), query, std::ref(oracle)))
      << "fewer rows";
}

TEST(CompareOrdered, GrowsTheOracleLimitUntilTheLastRunEnds) {
  // 3000 rows with key 0 then one with key 1: LIMIT 1 needs every tied row.
  std::vector<Row> ranked;
  ranked.reserve(3001);
  for (int i = 0; i < 3000; ++i) {
    ranked.push_back({std::to_string(i), "0"});
  }
  ranked.push_back({"last", "1"});
  Oracle oracle({kI, kI}, ranked);
  const auto query = Query(1, 0);
  const auto answer = oracle.Answer(1, 0, 1);
  EXPECT_FALSE(CompareOrdered(answer, Result({kI}, {{"2999"}}), query, std::ref(oracle)));
  EXPECT_EQ(oracle.limits_, (std::vector<std::optional<int64_t>>{1025, 4100}));
  EXPECT_TRUE(CompareOrdered(answer, Result({kI}, {{"last"}}), query, std::ref(oracle)));
}

TEST(CompareOrdered, RealValuesAndKeysUseTheTolerance) {
  // Keys 0.1 + 0.2 and 0.3 are one run.
  Oracle oracle({kR, kR}, {{"1.5", "0.30000000000000004"}, {"2.5", "0.3"}, {"9", "7"}});
  const auto query = Query(1, 0);
  const auto answer = oracle.Answer(1, 0, 1);
  EXPECT_FALSE(
      CompareOrdered(answer, Result({kR}, {{"2.5000000000000004"}}), query, std::ref(oracle)));
  EXPECT_TRUE(CompareOrdered(answer, Result({kR}, {{"9"}}), query, std::ref(oracle)));
}

TEST(CompareOrdered, DecimalValuesCompareExactly) {
  // (value, key) rows: two DECIMAL values tied on key 1, then one more.
  Oracle oracle({kD, kI}, {{"1234567890.12", "1"}, {"-0.25", "1"}, {".500", "2"}});
  const auto query = Query(std::nullopt, 0);
  const auto answer = oracle.Answer(1, 0, std::nullopt);
  EXPECT_FALSE(CompareOrdered(answer, Result({kD}, {{"-0.25"}, {"1234567890.12"}, {".500"}}), query,
                              std::ref(oracle)))
      << "ties in any order";
  // A wrong last digit or scale is another value.
  for (const std::string_view wrong : {"1234567890.13", "1234567890.120"}) {
    const auto d = CompareOrdered(answer, Result({kD}, {{"-0.25"}, {std::string(wrong)}, {".500"}}),
                                  query, std::ref(oracle));
    ASSERT_TRUE(d.has_value()) << wrong;
    EXPECT_TRUE(d.value_or(Discrepancy{}).mismatch) << wrong;
  }
  // As R values, the same wrong cent is within the tolerance.
  Oracle real({kR, kI}, {{"1234567890.12", "1"}, {"-0.25", "1"}, {"0.5", "2"}});
  EXPECT_FALSE(CompareOrdered(real.Answer(1, 0, std::nullopt),
                              Result({kR}, {{"-0.25"}, {"1234567890.13"}, {"0.5"}}), query,
                              std::ref(real)));
}

TEST(CompareOrdered, DecimalKeysWithinTheRealToleranceAreDifferentRuns) {
  // Keys 1234567890.12 and 1234567890.13: one run of ties as R keys, two runs as D keys, so the
  // order of rows 1 and 2 is fixed.
  const auto query = Query(std::nullopt, 0);
  Oracle real({kI, kR}, {{"1", "1234567890.12"}, {"2", "1234567890.13"}});
  EXPECT_FALSE(CompareOrdered(real.Answer(1, 0, std::nullopt), Result({kI}, {{"2"}, {"1"}}), query,
                              std::ref(real)));
  Oracle decimal({kI, kD}, {{"1", "1234567890.12"}, {"2", "1234567890.13"}});
  const auto d = CompareOrdered(decimal.Answer(1, 0, std::nullopt), Result({kI}, {{"2"}, {"1"}}),
                                query, std::ref(decimal));
  ASSERT_TRUE(d.has_value());
  EXPECT_TRUE(d.value_or(Discrepancy{}).mismatch);
}

TEST(CompareOrdered, ReportsOracleFailuresAndBadShapes) {
  const auto answer = Result({kI}, {{"1"}, {"2"}});
  const auto other = Result({kI}, {{"2"}, {"1"}});
  const auto failing = [](const std::string&) -> ExecResult {
    return std::unexpected(EngineError{.kind = "Binder", .message = "Binder: no"});
  };
  const auto d = CompareOrdered(answer, other, Query(std::nullopt, 0), failing);
  ASSERT_TRUE(d.has_value());
  EXPECT_NE(d.value_or(Discrepancy{}).what.find("DuckDB fails"), std::string::npos);
  const auto narrow = [&](const std::string&) -> ExecResult { return answer; };
  const auto shape = CompareOrdered(answer, other, Query(std::nullopt, 0), narrow);
  ASSERT_TRUE(shape.has_value());
  EXPECT_NE(shape.value_or(Discrepancy{}).what.find("harness bug"), std::string::npos);
  auto hugeint = Result({kI}, {{"2"}, {"1"}});
  hugeint.type_names[0] = "HUGEINT";
  EXPECT_TRUE(CompareOrdered(answer, hugeint, Query(std::nullopt, 0), narrow));
}

}  // namespace
}  // namespace antb1::slt
