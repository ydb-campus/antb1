// The comparison of a query with ORDER BY (ordered_compare.h): the augmented oracle query, ties in
// any order, ties at the edges of a LIMIT/OFFSET window, and the growing oracle limit.

#include "ordered_compare.h"

#include <cstdint>
#include <optional>
#include <string>
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
  EXPECT_EQ(MakeOrderedQuery("SELECT a FROM t ORDER BY a + 1"), std::nullopt) << "not parsed";
}

// The oracle side of `SELECT v FROM t ORDER BY k [LIMIT/OFFSET]` for (v, k) rows in DuckDB's order.
class Oracle {
 public:
  Oracle(std::vector<ColumnClass> classes, std::vector<Row> ranked)
      : classes_(std::move(classes)), ranked_(std::move(ranked)) {}

  ExecResult operator()(std::optional<int64_t> limit) {
    ++runs_;
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

TEST(CompareOrdered, ReportsOracleFailuresAndBadShapes) {
  const auto answer = Result({kI}, {{"1"}, {"2"}});
  const auto other = Result({kI}, {{"2"}, {"1"}});
  const auto failing = [](std::optional<int64_t>) -> ExecResult {
    return std::unexpected(EngineError{.kind = "Binder", .message = "Binder: no"});
  };
  const auto d = CompareOrdered(answer, other, Query(std::nullopt, 0), failing);
  ASSERT_TRUE(d.has_value());
  EXPECT_NE(d.value_or(Discrepancy{}).what.find("DuckDB fails"), std::string::npos);
  const auto narrow = [&](std::optional<int64_t>) -> ExecResult { return answer; };
  const auto shape = CompareOrdered(answer, other, Query(std::nullopt, 0), narrow);
  ASSERT_TRUE(shape.has_value());
  EXPECT_NE(shape.value_or(Discrepancy{}).what.find("harness bug"), std::string::npos);
  auto hugeint = Result({kI}, {{"2"}, {"1"}});
  hugeint.type_names[0] = "HUGEINT";
  EXPECT_TRUE(CompareOrdered(answer, hugeint, Query(std::nullopt, 0), narrow));
}

}  // namespace
}  // namespace antb1::slt
