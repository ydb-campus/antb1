// plan::DuckDbFloatOf against DuckDB itself: for seeded random numeric literals, the FLOAT that
// DuckDB casts the literal to, and whether DuckDB types it as DOUBLE (then antb1 compares in
// DOUBLE and DuckDbFloatOf returns std::nullopt).

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "antb1/plan/literal.h"

#include "canonical.h"
#include "duckdb_engine.h"

namespace antb1::slt {
namespace {

// splitmix64: the fixed-seed generator of the harness (no <random> distributions).
class Rng {
 public:
  explicit Rng(uint64_t seed) : state_(seed) {}
  uint64_t Next() {
    uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
  }
  std::size_t Below(std::size_t n) {
    const std::size_t r = Next() % n;  // uint64_t and size_t: both 64 bits on every target
    return r;
  }

 private:
  uint64_t state_;
};

std::string Digits(Rng& rng, std::size_t n) {
  std::string s;
  for (std::size_t i = 0; i < n; ++i) {
    s += static_cast<char>('0' + rng.Below(10));
  }
  return s;
}

// Numbers around the edges of DuckDB's conversion paths: 2^24 (the DECIMAL fast path), 2^63 and
// 2^64 (BIGINT and the hugeint halves), 2^127 and 2^128 (HUGEINT, UHUGEINT) and the FLOAT maximum.
constexpr auto kEdges = std::to_array<std::string_view>(
    {"16777216", "16777217", "9223372036854775807", "9223372036854775808", "18446744073709551616",
     "170141183460469231731687303715884105727", "170141183460469231731687303715884105728",
     "340282366920938463463374607431768211455", "340282356779733661637539395458142568447"});

// The text of a literal without its sign.
std::string RandomLiteral(Rng& rng) {
  switch (rng.Below(4)) {
    case 0:  // an integer of 1 to 40 digits
      return Digits(rng, 1 + rng.Below(40));
    case 1: {  // a decimal of up to 40 digits: "i.f", ".f" or "i."
      const std::size_t integer_digits = rng.Below(21);
      const std::size_t fraction_digits = rng.Below(21);
      std::string text = Digits(rng, integer_digits) + "." + Digits(rng, fraction_digits);
      return text == "." ? "0.0" : text;
    }
    case 2: {  // an edge, with its last digits changed or a fraction appended
      std::string text(kEdges.at(rng.Below(kEdges.size())));
      const std::size_t tail = rng.Below(4);
      text.replace(text.size() - tail, tail, Digits(rng, tail));
      if (rng.Below(2) == 0) {
        text += "." + Digits(rng, 1 + rng.Below(8));
      }
      return text;
    }
    default: {  // a short decimal near 2^24 with a long fraction
      return std::format("{}.{}", 16777200 + rng.Below(40), Digits(rng, 1 + rng.Below(20)));
    }
  }
}

TEST(FloatLiteralOracle, MatchesDuckDbOnRandomLiterals) {
  const std::filesystem::path dir = std::filesystem::path(::testing::TempDir()) / "float_oracle";
  std::filesystem::create_directories(dir);
  auto duckdb = DuckDbEngine::Make({}, dir, dir);
  ASSERT_TRUE(duckdb.has_value()) << duckdb.error();

  Rng rng(20260926);
  constexpr int kQueries = 80;
  constexpr int kPerQuery = 250;
  int checked_floats = 0;
  for (int q = 0; q < kQueries; ++q) {
    struct Literal {
      std::string text;
      bool negative;
    };
    std::vector<Literal> literals;
    std::string sql = "SELECT ";
    for (int i = 0; i < kPerQuery; ++i) {
      Literal lit{.text = RandomLiteral(rng), .negative = rng.Below(2) == 0};
      const std::string written = (lit.negative ? "-" : "") + lit.text;
      sql += std::format("{}typeof({}), TRY_CAST({} AS FLOAT)::DOUBLE", i > 0 ? ", " : "", written,
                         written);
      literals.push_back(std::move(lit));
    }
    auto result = (*duckdb)->Execute(sql);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->rows.size(), 1U);
    const auto& row = result->rows.front();
    for (std::size_t i = 0; i < literals.size(); ++i) {
      const Literal& lit = literals[i];
      const std::string written = (lit.negative ? "-" : "") + lit.text;
      const auto ours = plan::DuckDbFloatOf(lit.text, lit.negative);
      const bool duckdb_double = row[2 * i] == "DOUBLE";
      ASSERT_EQ(!ours.has_value(), duckdb_double)
          << written << " is " << row[2 * i].value_or("NULL");
      if (ours.has_value()) {
        EXPECT_EQ(CanonicalDouble(static_cast<double>(*ours)), row[(2 * i) + 1].value_or("NULL"))
            << written;
        ++checked_floats;
      }
    }
  }
  // Most literals are converted; the rest are DOUBLE (too many digits or out of range).
  EXPECT_GT(checked_floats, kQueries * kPerQuery / 2);
}

}  // namespace
}  // namespace antb1::slt
