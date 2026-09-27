// CompareOrdered against the DuckDB library when a run of ties at the window's end is longer than
// the rows it fetches in order: the rows antb1 returns there are checked with one query that writes
// their cells as SQL literals (ordered_compare.h).

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

// 12 rows; k = 1 for the first two, then 0 for the ten others (a long run of ties).
constexpr std::string_view kTable =
    "(VALUES (1, 'it''s', DATE '2013-07-01', 0.5, 1), (2, 'b', DATE '2013-07-02', 1.5, 1), "
    "(3, 'c', NULL, 2.5, 0), (4, 'd', DATE '2013-07-04', 3.5, 0), (5, NULL, DATE "
    "'2013-07-05', 4.5, 0), (6, 'f', DATE '2013-07-06', 0.1, 0), (7, 'g', DATE '2013-07-07', "
    "6.5, 0), (8, 'h', DATE '2013-07-08', 7.5, 0), (9, 'é', DATE '2013-07-09', 8.5, 0), "
    "(10, 'j', DATE '2013-07-10', 9.5, 0), (11, 'k', DATE '2013-07-11', 10.5, 0), (12, 'l', "
    "DATE '2013-07-12', 11.5, 0)) AS t(id, s, d, r, k)";

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

  // `SELECT id, s, d, r FROM t ORDER BY k DESC LIMIT 3 OFFSET 1` compared with `antb1`, fetching
  // at most 4 rows in order.
  std::optional<Discrepancy> Compare(const std::vector<Row>& antb1_rows) {
    const std::string sql =
        std::format("SELECT id, s, d, r FROM {} ORDER BY k DESC LIMIT 3 OFFSET 1", kTable);
    const OrderedQuery q{
        .augmented_sql =
            std::format("SELECT id, s, d, r, k AS __antb1_key0 FROM {} ORDER BY k DESC", kTable),
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
      return duckdb_->Execute(query);
    });
  }

  std::unique_ptr<DuckDbEngine> duckdb_;
  int queries_ = 0;
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

}  // namespace
}  // namespace antb1::slt
