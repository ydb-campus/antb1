// exec::CaseConvertValue against DuckDB itself: upper() and lower() of every codepoint outside the
// surrogates. The table in src/exec/unicode_case.inc comes from DuckDB's CLI
// (tools/unicode/case_tables.py); this pins it, and its UTF-8 decoding and encoding, to the DuckDB
// library the tests link, so a DuckDB upgrade that changes a mapping fails here.

#include <cstddef>
#include <filesystem>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include "antb1/exec/case_mapping.h"

#include "duckdb_engine.h"

namespace antb1::slt {
namespace {

std::string Utf8(char32_t codepoint) {
  std::string out;
  const auto put = [&out](unsigned value) { out.push_back(static_cast<char>(value)); };
  if (codepoint < 0x80) {
    put(codepoint);
  } else if (codepoint < 0x800) {
    put(0xC0U | (codepoint >> 6U));
    put(0x80U | (codepoint & 0x3FU));
  } else if (codepoint < 0x10000) {
    put(0xE0U | (codepoint >> 12U));
    put(0x80U | ((codepoint >> 6U) & 0x3FU));
    put(0x80U | (codepoint & 0x3FU));
  } else {
    put(0xF0U | (codepoint >> 18U));
    put(0x80U | ((codepoint >> 12U) & 0x3FU));
    put(0x80U | ((codepoint >> 6U) & 0x3FU));
    put(0x80U | (codepoint & 0x3FU));
  }
  return out;
}

TEST(CaseMappingOracle, MatchesDuckDbOnEveryCodepoint) {
  const std::filesystem::path dir = std::filesystem::path(::testing::TempDir()) / "case_oracle";
  std::filesystem::create_directories(dir);
  auto duckdb = DuckDbEngine::Make({}, dir, dir);
  ASSERT_TRUE(duckdb.has_value()) << duckdb.error();

  // Only the codepoints DuckDB changes come back; every other one must stay as it is.
  for (const bool upper : {true, false}) {
    const std::string_view function = upper ? "upper" : "lower";
    auto result = (*duckdb)->Execute(
        std::format("SELECT r::VARCHAR, {0}(chr(r::INTEGER)) FROM range(1, 1114112) t(r) "
                    "WHERE (r < 55296 OR r > 57343) AND {0}(chr(r::INTEGER)) <> chr(r::INTEGER)",
                    function));
    ASSERT_TRUE(result.has_value()) << result.error().message;
    std::map<char32_t, std::string> changed;
    for (const auto& row : result->rows) {
      ASSERT_EQ(row.size(), 2U);
      ASSERT_TRUE(row[0].has_value() && row[1].has_value());
      changed.emplace(static_cast<char32_t>(std::stoul(*row[0])), *row[1]);
    }
    // Both directions change well over a thousand codepoints, ASCII's 26 among them.
    EXPECT_GT(changed.size(), 1000U) << function;

    std::size_t mismatches = 0;
    std::string ours;
    for (char32_t codepoint = 1; codepoint <= 0x10FFFF; ++codepoint) {
      if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
        continue;
      }
      const std::string value = Utf8(codepoint);
      ASSERT_TRUE(exec::CaseConvertValue(value, upper, ours))
          << std::hex << static_cast<unsigned>(codepoint);
      const auto found = changed.find(codepoint);
      const std::string& expected = found == changed.end() ? value : found->second;
      if (ours != expected && ++mismatches <= 20) {
        ADD_FAILURE() << function << "(U+" << std::hex << static_cast<unsigned>(codepoint)
                      << ") differs from DuckDB";
      }
    }
    EXPECT_EQ(mismatches, 0U) << function;
  }
}

}  // namespace
}  // namespace antb1::slt
