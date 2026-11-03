// plan::CommonValueType and plan::DuckDbCaseMixes (ADR 0037's NL1) against the DuckDB library: for
// every pair of the engine's types, and NULL, the type of a UNION ALL column, and whether a CASE
// takes the two values and at which type. The values are expressions over a table of no row, so
// DuckDB types them without running a cast (a number below a DATE column binds, then fails).

#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "antb1/plan/type_fold.h"
#include "antb1/plan/types.h"

#include "duckdb_engine.h"

namespace antb1::slt {
namespace {

using plan::LogicalType;
using plan::ValueType;
using enum LogicalType::Id;

// A value of each type, and an expression DuckDB types as it, over `range(0) t(i)`.
std::vector<std::pair<ValueType, std::string>> Values() {
  return {
      {std::nullopt, "NULL"},
      {LogicalType(kSmallInt), "CAST(i AS SMALLINT)"},
      {LogicalType(kUSmallInt), "CAST(i AS USMALLINT)"},
      {LogicalType(kInteger), "CAST(i AS INTEGER)"},
      {LogicalType(kBigInt), "CAST(i AS BIGINT)"},
      {LogicalType(kHugeInt), "CAST(i AS HUGEINT)"},
      {LogicalType::Decimal(4, 1), "CAST(i AS DECIMAL(4,1))"},
      {LogicalType::Decimal(18, 3), "CAST(i AS DECIMAL(18,3))"},
      {LogicalType::Decimal(38, 10), "CAST(i AS DECIMAL(38,10))"},
      {LogicalType(kDouble), "CAST(i AS DOUBLE)"},
      {LogicalType(kVarchar), "CAST(i AS VARCHAR)"},
      {LogicalType(kDate), "DATE '2020-01-01' + CAST(i AS INTEGER)"},
      {LogicalType(kTimestamp), "CAST(DATE '2020-01-01' + CAST(i AS INTEGER) AS TIMESTAMP)"},
      {LogicalType(kBoolean), "i = 0"},
  };
}

class TypeFoldOracle : public ::testing::Test {
 protected:
  void SetUp() override {
    const std::filesystem::path dir = std::filesystem::path(::testing::TempDir()) / "type_fold";
    std::filesystem::create_directories(dir);
    auto engine = DuckDbEngine::Make({}, dir, dir);
    ASSERT_TRUE(engine.has_value()) << engine.error();
    duckdb_ = *std::move(engine);
  }

  // The type DuckDB gives the one column of `sql`, or its error.
  std::pair<std::optional<std::string>, std::string> TypeOf(const std::string& sql) {
    const ExecResult result = duckdb_->Execute(sql);
    if (!result.has_value()) {
      return {std::nullopt, result.error().message};
    }
    EXPECT_EQ(result->type_names.size(), 1U) << sql;
    return {result->type_names.empty() ? std::string() : result->type_names.front(), ""};
  }

  std::unique_ptr<DuckDbEngine> duckdb_;
};

TEST_F(TypeFoldOracle, UnionAllTypesAColumnAsTheFoldDoes) {
  int checked = 0;
  for (const auto& [a, a_sql] : Values()) {
    for (const auto& [b, b_sql] : Values()) {
      if (!a.has_value() && !b.has_value()) {
        continue;  // the NULL type, which no result column of the harness holds
      }
      const std::string sql = std::format(
          "SELECT x FROM (SELECT {} AS x FROM range(0) t(i) UNION ALL SELECT {} FROM range(0) "
          "t(i))",
          a_sql, b_sql);
      const auto [type, error] = TypeOf(sql);
      const ValueType folded = plan::CommonValueType(a, b);
      if (!folded.has_value()) {
        ADD_FAILURE() << "no type for " << sql;
        continue;
      }
      EXPECT_EQ(type, plan::ToString(*folded)) << sql << "\n" << error;
      ++checked;
    }
  }
  EXPECT_EQ(checked, 195);  // 14 x 14, NULL with NULL left out
}

TEST_F(TypeFoldOracle, CaseTakesTheMixesTheFoldSaysAtItsType) {
  for (const auto& [a, a_sql] : Values()) {
    for (const auto& [b, b_sql] : Values()) {
      if (!a.has_value() && !b.has_value()) {
        continue;
      }
      const std::string sql = std::format(
          "SELECT CASE WHEN i = 0 THEN {} ELSE {} END AS x FROM range(0) t(i)", a_sql, b_sql);
      const auto [type, error] = TypeOf(sql);
      if (plan::DuckDbCaseMixes(a, b)) {
        const ValueType folded = plan::CommonValueType(a, b);
        if (!folded.has_value()) {
          ADD_FAILURE() << "no type for " << sql;
          continue;
        }
        EXPECT_EQ(type, plan::ToString(*folded)) << sql << "\n" << error;
      } else {
        EXPECT_FALSE(type.has_value()) << sql << ": DuckDB takes the mix as " << type.value_or("");
        EXPECT_TRUE(error.contains("Cannot mix values of type")) << sql << "\n" << error;
      }
    }
  }
}

}  // namespace
}  // namespace antb1::slt
