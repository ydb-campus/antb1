// CompareOrdered against the DuckDB library when a run of ties at the window's end is longer than
// the rows it fetches in order: the rows antb1 returns there are checked with one query that writes
// their cells as SQL literals (ordered_compare.h).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "duckdb_engine.h"
#include "engine.h"
#include "ordered_compare.h"
#include "result_diff.h"

namespace antb1::slt {
namespace {

using Row = std::vector<std::optional<std::string>>;

// 12 rows; k = 1 for the first two, then 0 for the ten others (a long run of ties). r is a DOUBLE
// (alone, the decimal literals would make it a DECIMAL(3,1), which compares exactly).
constexpr std::string_view kTable =
    "(VALUES (1, 'it''s', DATE '2013-07-01', CAST(0.5 AS DOUBLE), 1), (2, 'b', DATE '2013-07-02', "
    "1.5, 1), (3, 'c', NULL, 2.5, 0), (4, 'd', DATE '2013-07-04', 3.5, 0), (5, NULL, DATE "
    "'2013-07-05', 4.5, 0), (6, 'f', DATE '2013-07-06', 0.1, 0), (7, 'g', DATE '2013-07-07', "
    "6.5, 0), (8, 'h', DATE '2013-07-08', 7.5, 0), (9, 'é', DATE '2013-07-09', 8.5, 0), "
    "(10, 'j', DATE '2013-07-10', 9.5, 0), (11, 'k', DATE '2013-07-11', 10.5, 0), (12, 'l', "
    "DATE '2013-07-12', 11.5, 0)) AS t(id, s, d, r, k)";

// 12 rows with DECIMAL keys m (1.25 for the first two, then a long run of -0.05) and DECIMAL(3,3)
// values f, which print without a leading zero (.500).
constexpr std::string_view kDecimalTable =
    "(SELECT id, CAST(m AS DECIMAL(5,2)) AS m, CAST(f AS DECIMAL(3,3)) AS f FROM (VALUES "
    "(1, 1.25, 0.5), (2, 1.25, -0.5), (3, -0.05, NULL), (4, -0.05, 0), (5, -0.05, 0.001), "
    "(6, -0.05, -0.999), (7, -0.05, 0.25), (8, -0.05, 0.75), (9, -0.05, -0.25), (10, -0.05, 0.1), "
    "(11, -0.05, 0.2), (12, -0.05, 0.999)) AS v(id, m, f)) AS t";

class OrderedCompareOracleTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const std::filesystem::path dir =
        std::filesystem::path(::testing::TempDir()) / "ordered_compare_oracle";
    std::filesystem::create_directories(dir);
    auto engine = DuckDbEngine::Make({}, dir, dir);
    ASSERT_TRUE(engine.has_value()) << engine.error();
    duckdb_ = *std::move(engine);
  }

  // `SELECT <items> FROM <table> ORDER BY <key> DESC LIMIT 3 OFFSET 1` compared with `antb1`,
  // fetching at most 4 rows in order.
  std::optional<Discrepancy> Compare(const std::vector<Row>& antb1_rows,
                                     std::string_view table = kTable,
                                     std::string_view items = "id, s, d, r",
                                     std::string_view key = "k") {
    const std::string sql =
        std::format("SELECT {} FROM {} ORDER BY {} DESC LIMIT 3 OFFSET 1", items, table, key);
    const OrderedQuery q{
        .augmented_sql = std::format("SELECT {}, {} AS __antb1_key0 FROM {} ORDER BY {} DESC",
                                     items, key, table, key),
        .keys = 1,
        .limit = 3,
        .offset = 1,
        .max_rows = 4};
    auto oracle = duckdb_->Execute(sql);
    EXPECT_TRUE(oracle.has_value()) << oracle.error().message;
    ResultSet antb1 = oracle.value_or(ResultSet{});
    antb1.rows = antb1_rows;
    return CompareOrdered(oracle.value_or(ResultSet{}), antb1, q, [&](const std::string& query) {
      ++queries_;
      auto result = duckdb_->Execute(query);
      last_rows_ = result.has_value() ? result->rows.size() : 0;
      return result;
    });
  }

  std::unique_ptr<DuckDbEngine> duckdb_;
  int queries_ = 0;
  std::size_t last_rows_ = 0;  // rows of the last oracle query that CompareOrdered ran
};

TEST_F(OrderedCompareOracleTest, RowsOfALongRunAreCheckedByQuery) {
  // Rank 1 is (2, k = 1); ranks 2 and 3 are any two distinct rows with k = 0.
  EXPECT_FALSE(Compare({{"2", "b", "2013-07-02", "1.5"},
                        {"12", "l", "2013-07-12", "11.5"},
                        {"3", "c", std::nullopt, "2.5"}}));
  EXPECT_EQ(queries_, 2) << "the first 5 ranked rows, then the rows of the run of k = 0";
  EXPECT_FALSE(Compare({{"2", "b", "2013-07-02", "1.5"},
                        {"9", "é", "2013-07-09", "8.5"},
                        {"6", "f", "2013-07-06", "0.10000000000000002"}}))
      << "R cells match within the tolerance";
  EXPECT_FALSE(Compare({{"2", "b", "2013-07-02", "1.5"},
                        {"5", std::nullopt, "2013-07-05", "4.5"},
                        {"4", "d", "2013-07-04", "3.5"}}));
}

TEST_F(OrderedCompareOracleTest, WrongRowsOfALongRunFail) {
  // Not in the run (k = 1), a value that no row has, the same row twice, a wrong R value.
  for (const std::vector<Row>& rows :
       std::vector<std::vector<Row>>{{{"2", "b", "2013-07-02", "1.5"},
                                      {"1", "it's", "2013-07-01", "0.5"},
                                      {"3", "c", std::nullopt, "2.5"}},
                                     {{"2", "b", "2013-07-02", "1.5"},
                                      {"12", "x", "2013-07-12", "11.5"},
                                      {"3", "c", std::nullopt, "2.5"}},
                                     {{"2", "b", "2013-07-02", "1.5"},
                                      {"12", "l", "2013-07-12", "11.5"},
                                      {"12", "l", "2013-07-12", "11.5"}},
                                     {{"2", "b", "2013-07-02", "1.5"},
                                      {"12", "l", "2013-07-12", "11.25"},
                                      {"3", "c", std::nullopt, "2.5"}}}) {
    const auto d = Compare(rows);
    ASSERT_TRUE(d.has_value());
    EXPECT_TRUE(d.value_or(Discrepancy{}).mismatch) << d.value_or(Discrepancy{}).what;
  }
  // Text that SQL cannot hold is reported as a harness limitation, never as a pass.
  const auto bytes = Compare({{"2", "b", "2013-07-02", "1.5"},
                              {"12", "\xFF", "2013-07-12", "11.5"},
                              {"3", "c", std::nullopt, "2.5"}});
  ASSERT_TRUE(bytes.has_value());
  EXPECT_NE(bytes.value_or(Discrepancy{}).what.find("harness limitation"), std::string::npos);
}

// `SELECT id, m, f FROM kDecimalTable ORDER BY m DESC LIMIT 3 OFFSET 1`, fetching at most 4 rows.
TEST_F(OrderedCompareOracleTest, DecimalRowsOfALongRunAreCheckedByQuery) {
  // Rank 1 is a row with m = 1.25; ranks 2 and 3 are any two distinct rows of the run of -0.05.
  EXPECT_FALSE(
      Compare({{"2", "1.25", "-.500"}, {"12", "-0.05", ".999"}, {"3", "-0.05", std::nullopt}},
              kDecimalTable, "id, m, f", "m"));
  EXPECT_EQ(queries_, 2) << "the first 5 ranked rows, then the rows of the run of -0.05";
  EXPECT_FALSE(Compare({{"1", "1.25", ".500"}, {"6", "-0.05", "-.999"}, {"4", "-0.05", ".000"}},
                       kDecimalTable, "id, m, f", "m"));
}

TEST_F(OrderedCompareOracleTest, WrongDecimalsInALongRunFail) {
  // A wrong last digit, a wrong scale, a value out of the type's range, other spellings of a run
  // member's key, and a wrong value at rank 1 (fetched in order). Without the VARCHAR cast of D
  // columns (ordered_compare.cc, Operand), '9.999' would fail the members query with a conversion
  // error, which is no mismatch; the other spellings are tested by
  // LongRunMembersMatchDecimalsAsText.
  for (const std::vector<Row>& rows : std::vector<std::vector<Row>>{
           {{"2", "1.25", "-.500"}, {"12", "-0.05", ".998"}, {"3", "-0.05", std::nullopt}},
           {{"2", "1.25", "-.500"}, {"12", "-0.05", ".99"}, {"3", "-0.05", std::nullopt}},
           {{"2", "1.25", "-.500"}, {"12", "-0.05", "9.999"}, {"3", "-0.05", std::nullopt}},
           {{"2", "1.25", "-.500"}, {"12", "-.05", ".999"}, {"3", "-0.05", std::nullopt}},
           {{"2", "1.25", "-.500"}, {"12", "-0.050", ".999"}, {"3", "-0.05", std::nullopt}},
           {{"2", "1.25", "-.50"}, {"12", "-0.05", ".999"}, {"3", "-0.05", std::nullopt}}}) {
    const auto d = Compare(rows, kDecimalTable, "id, m, f", "m");
    ASSERT_TRUE(d.has_value());
    EXPECT_TRUE(d.value_or(Discrepancy{}).mismatch) << d.value_or(Discrepancy{}).what;
  }
}

TEST_F(OrderedCompareOracleTest, LongRunMembersMatchDecimalsAsText) {
  // The members query of the run of -0.05 finds only rows whose DECIMAL cells have antb1's text.
  // Compared with a DECIMAL, the strings '-.05' and '-0.050' would be cast to -0.05 and row 12
  // would match too.
  for (const std::string_view m : {"-.05", "-0.050"}) {
    queries_ = 0;
    const auto d = Compare(
        {{"2", "1.25", "-.500"}, {"12", std::string(m), ".999"}, {"3", "-0.05", std::nullopt}},
        kDecimalTable, "id, m, f", "m");
    ASSERT_TRUE(d.has_value()) << m;
    EXPECT_TRUE(d.value_or(Discrepancy{}).mismatch) << m;
    EXPECT_EQ(queries_, 2) << m;
    EXPECT_EQ(last_rows_, 1U) << m << ": only row 3";
  }
}

}  // namespace
}  // namespace antb1::slt
