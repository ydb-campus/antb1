// The DuckDB adapter (duckdb_engine.h) against the DuckDB library: a DECIMAL is an exact D column
// named DECIMAL(p,s), and its canonical text is the text DuckDB prints, at every physical width
// DuckDB stores a DECIMAL in (16, 32, 64 and 128 bits).

#include "duckdb_engine.h"

#include <cstddef>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "engine.h"
#include "result_diff.h"

namespace antb1::slt {
namespace {

using Row = std::vector<std::optional<std::string>>;

class DuckDbEngineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const std::filesystem::path dir =
        std::filesystem::path(::testing::TempDir()) / "duckdb_engine_test";
    std::filesystem::create_directories(dir);
    auto engine = DuckDbEngine::Make({}, dir, dir);
    ASSERT_TRUE(engine.has_value()) << engine.error();
    duckdb_ = *std::move(engine);
  }

  ResultSet Run(const std::string& sql) {
    auto result = duckdb_->Execute(sql);
    EXPECT_TRUE(result.has_value()) << sql << "\n"
                                    << (result.has_value() ? "" : result.error().message);
    return result.value_or(ResultSet{});
  }

  std::unique_ptr<DuckDbEngine> duckdb_;
};

// `digits` / 10^scale with an integer digit before the point ("0.05"), which DuckDB reads into a
// DECIMAL of any width, negated if `negative`.
std::string Text(std::string digits, std::size_t scale, bool negative) {
  if (digits.size() <= scale) {
    digits.insert(0, scale + 1 - digits.size(), '0');
  }
  if (scale > 0) {
    digits.insert(digits.size() - scale, ".");
  }
  return (negative ? "-" : "") + digits;
}

TEST_F(DuckDbEngineTest, DecimalsAreExactColumnsNamedWithWidthAndScale) {
  const ResultSet r = Run(
      "SELECT CAST(17 AS DECIMAL(15,2)), CAST(-0.25 AS DECIMAL(15,2)), CAST(0.5 AS DECIMAL(3,3)), "
      "CAST(-0.5 AS DECIMAL(3,3)), CAST(-12 AS DECIMAL(4,0)), 0.5, CAST(NULL AS DECIMAL(5,1)), "
      "CAST(1 AS HUGEINT), CAST(0.5 AS DOUBLE)");
  EXPECT_EQ(Letters(r), "DDDDDDDIR");
  EXPECT_EQ(r.type_names,
            (std::vector<std::string>{"DECIMAL(15,2)", "DECIMAL(15,2)", "DECIMAL(3,3)",
                                      "DECIMAL(3,3)", "DECIMAL(4,0)", "DECIMAL(2,1)",
                                      "DECIMAL(5,1)", "HUGEINT", "DOUBLE"}));
  EXPECT_EQ(r.rows, (std::vector<Row>{{"17.00", "-0.25", ".500", "-.500", "-12", "0.5",
                                       std::nullopt, "1", "0.5"}}));
}

TEST_F(DuckDbEngineTest, DecimalTextIsDuckDbsVarcharText) {
  // The edges of every physical width (4, 9, 18 and 38 digits), scales 0, 1, half and all of the
  // width, and the values 0, one unit, a mix of digits and the largest, with both signs. The text
  // must be DuckDB's own CAST(d AS VARCHAR): long runs of ties compare DECIMALs as that text
  // (ordered_compare.h).
  for (const std::size_t width : {1U, 4U, 5U, 9U, 10U, 18U, 19U, 38U}) {
    for (const std::size_t scale : std::set<std::size_t>{0, 1, width / 2, width}) {
      const std::string type = std::format("DECIMAL({},{})", width, scale);
      std::string values;
      for (const std::string& digits :
           {std::string("0"), std::string("1"),
            std::string("12345678901234567890123456789012345678").substr(0, width),
            std::string(width, '9')}) {
        for (const bool negative : {false, true}) {
          values += std::format("{}(CAST('{}' AS {}))", values.empty() ? "" : ", ",
                                Text(digits, scale, negative), type);
        }
      }
      const ResultSet r =
          Run(std::format("SELECT d, CAST(d AS VARCHAR) FROM (VALUES {}) AS t(d)", values));
      EXPECT_EQ(Letters(r), "DT") << type;
      ASSERT_FALSE(r.type_names.empty()) << type;
      EXPECT_EQ(r.type_names[0], type);
      ASSERT_EQ(r.rows.size(), 8U) << type;
      for (const Row& row : r.rows) {
        EXPECT_EQ(row[0], row[1]) << type;
      }
    }
  }
}

}  // namespace
}  // namespace antb1::slt
