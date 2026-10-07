#include "query_gen.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <gtest/gtest.h>
#include <parquet/arrow/writer.h>

#include "canonical.h"
#include "supported_features.h"
#include "tables.h"

namespace antb1::slt {
namespace {

std::string Lower(std::string s) {
  std::ranges::transform(s, s.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

// A small table (path form allowed) and a large one, with one column of every kind (two DECIMALs:
// one with spare digits, one at the 38-digit cap); EventDate is DATE only through the clickbench
// option.
std::vector<GenTable> Tables() {
  GenTable small{.name = "small", .path = "/data/small.parquet", .rows = 12, .columns = {}};
  small.columns = {
      {.name = "i",
       .kind = ValueKind::kInteger,
       .min = -32768,
       .max = 32767,
       .samples = {"-5", "7"}},
      {.name = "d",
       .kind = ValueKind::kDouble,
       .samples = {"0.5", "0.1000000000000000055511151231257827", "-9007199254740992"}},
      {.name = "s", .kind = ValueKind::kVarchar, .samples = {"it's", "naïve"}},
      {.name = "EventDate",
       .kind = ValueKind::kDate,
       .via_override = true,
       .samples = {"2013-07-02"}},
      {.name = "m",
       .kind = ValueKind::kDecimal,
       .samples = {"17.00", "-0.25", "0.05"},
       .precision = 15,
       .scale = 2},
      {.name = "w",
       .kind = ValueKind::kDecimal,
       .samples = {"-5.0000000005", "0.0000000000"},
       .precision = 38,
       .scale = 10},
  };
  GenTable big = small;
  big.name = "big";
  big.path = "/data/big-*.parquet";
  big.rows = 10'000;
  return {small, big};
}

GenKeyStats Stats(int64_t non_null, int64_t distinct, int64_t max_multiplicity) {
  return GenKeyStats{
      .non_null = non_null, .distinct = distinct, .max_multiplicity = max_multiplicity};
}

QueryGenerator Make(uint64_t seed, GeneratorOptions options) {
  auto gen = QueryGenerator::Make(Tables(), seed, options);
  EXPECT_TRUE(gen.has_value()) << gen.error();
  return *std::move(gen);
}

TEST(QueryGenerator, EachIndexIsAPureFunctionOfTheSeed) {
  const auto a = Make(42, {.supported = kSupportedFeatures, .target_percent = 50});
  const auto b = Make(42, {.supported = kSupportedFeatures, .target_percent = 50});
  const auto c = Make(43, {.supported = kSupportedFeatures, .target_percent = 50});
  int differ = 0;
  for (uint64_t i = 0; i < 200; ++i) {
    const auto q = a.Generate(i);
    EXPECT_EQ(q.index, i);
    EXPECT_EQ(q.sql, b.Generate(i).sql) << i;
    EXPECT_EQ(q.sql, a.Generate(i).sql) << i;
    differ += q.sql != c.Generate(i).sql ? 1 : 0;
  }
  EXPECT_GT(differ, 50) << "another seed must give other queries";
}

TEST(QueryGenerator, SupportedQueriesUseOnlySupportedFeatures) {
  const FeatureSet richer = {Feature::kCountStar,      Feature::kSum,
                             Feature::kTableName,      Feature::kWhere,
                             Feature::kIntegerColumns, Feature::kIntegerLiteral,
                             Feature::kMultipleItems,  Feature::kKeywordCase};
  for (const FeatureSet& supported : {kSupportedFeatures, richer}) {
    const auto gen = Make(7, {.supported = supported, .target_percent = 0});
    FeatureSet seen;
    for (uint64_t i = 0; i < 500; ++i) {
      const auto q = gen.Generate(i);
      EXPECT_FALSE(q.target_sample);
      EXPECT_FALSE(q.sql.empty());
      EXPECT_TRUE(supported.Contains(q.features))
          << q.sql << "\n  uses " << q.features.Minus(supported).Names();
      seen.Add(q.features);
    }
    EXPECT_EQ(seen, supported) << "seen: " << seen.Names();
  }
}

TEST(QueryGenerator, TargetSamplesCoverTheWholeGrammar) {
  // A Feature without generator support fails here (runner/query_gen.cc must learn it), unless it
  // waits in kGeneratorPending.
  const auto gen = Make(11, {.supported = kSupportedFeatures, .target_percent = 100});
  FeatureSet seen;
  for (uint64_t i = 0; i < 4000; ++i) {
    const auto q = gen.Generate(i);
    EXPECT_TRUE(q.target_sample);
    seen.Add(q.features);
  }
  const FeatureSet expected = FeatureSet::All().Minus(kNeverGenerated).Minus(kGeneratorPending);
  EXPECT_EQ(seen, expected) << "never generated: " << expected.Minus(seen).Names();
}

TEST(QueryGenerator, QueriesRespectTheSemanticsBothEnginesShare) {
  const auto gen = Make(3, {.supported = kSupportedFeatures, .target_percent = 100});
  int decimal_cases = 0;
  int decimal_sums = 0;
  int decimal_arithmetics = 0;
  int decimal_divisions = 0;
  int decimal_literal_operands = 0;
  int decimal_constants = 0;
  int decimal_column_comparisons = 0;
  int decimal_double_comparisons = 0;
  int decimal_double_lists = 0;
  for (uint64_t i = 0; i < 3000; ++i) {
    const auto q = gen.Generate(i);
    const std::string sql = Lower(q.sql);
    const bool aggregate = std::ranges::any_of(
        std::to_array({Feature::kCountStar, Feature::kCountColumn, Feature::kSum, Feature::kAvg,
                       Feature::kMin, Feature::kMax, Feature::kCountDistinct}),
        [&](Feature f) { return q.features.Has(f); });
    // A select list of constants, arithmetic, string functions or CASE only is a projection too: a
    // row per table row.
    const bool rows =
        q.features.Has(Feature::kColumns) || q.features.Has(Feature::kStar) ||
        q.features.Has(Feature::kGroupBy) ||
        ((q.features.Has(Feature::kConstant) || q.features.Has(Feature::kArithmetic) ||
          q.features.Has(Feature::kStringFunctions) || q.features.Has(Feature::kCase) ||
          q.features.Has(Feature::kTimestamps)) &&
         !aggregate);
    const bool ordered = q.features.Has(Feature::kOrderBy);
    EXPECT_EQ(q.sort, rows && !ordered ? SortMode::kRowSort : SortMode::kNoSort) << q.sql;
    EXPECT_EQ(
        q.unordered_limit,
        rows && !ordered && (q.features.Has(Feature::kLimit) || q.features.Has(Feature::kOffset)))
        << q.sql;
    if (ordered) {
      // Sort keys with I or T values only: no AVG, no SUM or MIN/MAX of a DOUBLE column.
      EXPECT_FALSE(sql.substr(sql.find("order")).contains("avg")) << q.sql;
    }
    if (q.features.Has(Feature::kTablePath)) {
      // DuckDB reads the raw file there: a column typed through the clickbench option differs.
      EXPECT_FALSE(sql.contains("eventdate") || q.features.Has(Feature::kStar)) << q.sql;
    }
    if (q.features.Has(Feature::kStar) && q.table == "big") {
      EXPECT_TRUE(q.features.Has(Feature::kLimit)) << q.sql;
    }
    // DECIMAL columns: CASE values; arithmetic with integer and decimal literals, / // %
    // included; no literal with more than 10 fraction digits (w's scale; m has spare digits),
    // which DuckDB would compare in a DECIMAL capped at 38 digits (divergence D13). Layout
    // comments could hide a match.
    if (!q.features.Has(Feature::kLayout)) {
      static const std::regex decimal_case(R"re(then "?[mw]\b|else "?[mw]\b)re");
      decimal_cases += std::regex_search(sql, decimal_case) ? 1 : 0;
      static const std::regex decimal_sum(R"re((sum|avg)\( ?"?[mw]"?\))re");
      static const std::regex decimal_arithmetic(R"re("?\b[mw]\b"? ?[-+*] ?[0-9]+(?![.0-9]))re");
      static const std::regex decimal_division(R"re("?\b[mw]\b"? ?(/|//|%) ?[0-9])re");
      static const std::regex decimal_literal_operand(R"re("?\b[mw]\b"? ?[-+*%] ?[0-9]+\.[0-9])re");
      // SUM and AVG never add the inexact doubles a division makes (their rounding follows the
      // order of the additions), and a DECIMAL aggregate argument keeps its scale (no decimal
      // literal: HAVING writes literals of the column's scale, D13).
      static const std::regex summed_division(
          R"re((sum|avg)\((distinct )?"?([mw]"? ?//? ?[0-9]|i"? ?(/ ?1\.5|// ?2\.5)))re");
      static const std::regex aggregated_decimal_literal(
          R"re((sum|avg|min|max|count)\((distinct )?"?[mw]"? ?[-+*%] ?[0-9]+\.)re");
      EXPECT_FALSE(std::regex_search(sql, summed_division)) << q.sql;
      EXPECT_FALSE(std::regex_search(sql, aggregated_decimal_literal)) << q.sql;
      static const std::regex decimal_constant(
          R"re((select|,) (-? ?(2\.5|0\.25)|007\.50|\.125)( |,|$))re");
      decimal_sums += std::regex_search(sql, decimal_sum) ? 1 : 0;
      decimal_arithmetics += std::regex_search(sql, decimal_arithmetic) ? 1 : 0;
      decimal_divisions += std::regex_search(sql, decimal_division) ? 1 : 0;
      decimal_literal_operands += std::regex_search(sql, decimal_literal_operand) ? 1 : 0;
      decimal_constants += std::regex_search(sql, decimal_constant) ? 1 : 0;
      // A DECIMAL against another column of numbers, and against numbers with an exponent (alone
      // or in an IN list, compared in DOUBLE); the integer column i never gets an exponent, which
      // it would compare with nearest doubles (divergence D7), and BETWEEN never mixes one with
      // the other numbers.
      static const std::regex column_comparison(
          R"re("?\b[mw]\b"? ?(=|<>|!=|<=|>=|<|>) ?"?\b[idmw]\b"?(?!\.|\())re");
      static const std::regex reversed_comparison(
          R"re("?\b[id]\b"? ?(=|<>|!=|<=|>=|<|>) ?"?\b[mw]\b"?)re");
      static const std::regex double_comparison(
          R"re("?\b[mw]\b"? ?(=|<>|!=|<=|>=|<|>) ?-?[0-9.]+e)re");
      static const std::regex double_list(R"re("?\b[mw]\b"? (not )?in ?\([^)]*[0-9]e)re");
      static const std::regex integer_exponent(
          R"re("?\bi\b"? ?(=|<>|!=|<=|>=|<|>) ?-?[0-9.]+e|"?\bi\b"? (not )?in ?\([^)]*[0-9]e)re");
      static const std::regex mixed_between(
          R"re(between -?[0-9.]+e[-0-9]+ and -?[0-9.]+(?![0-9.]*e)|between -?[0-9.]+(?![0-9.]*e) and -?[0-9.]+e)re");
      EXPECT_FALSE(std::regex_search(sql, integer_exponent)) << q.sql;
      EXPECT_FALSE(std::regex_search(sql, mixed_between)) << q.sql;
      decimal_column_comparisons +=
          std::regex_search(sql, column_comparison) || std::regex_search(sql, reversed_comparison)
              ? 1
              : 0;
      decimal_double_comparisons += std::regex_search(sql, double_comparison) ? 1 : 0;
      decimal_double_lists += std::regex_search(sql, double_list) ? 1 : 0;
    }
    static const std::regex long_fraction(R"re(\.[0-9]{11})re");
    EXPECT_FALSE(std::regex_search(sql, long_fraction)) << q.sql;
    // Doubles only get literals that both engines convert to the same value.
    EXPECT_FALSE(q.sql.contains("0.1000000000000000055511151231257827")) << q.sql;
    EXPECT_FALSE(q.sql.contains("it's")) << "quotes in string literals are doubled: " << q.sql;
  }
  EXPECT_GT(decimal_cases, 10) << "DECIMAL CASE values";
  EXPECT_GT(decimal_sums, 10) << "SUM and AVG of DECIMAL columns";
  EXPECT_GT(decimal_arithmetics, 10) << "DECIMAL arithmetic with integer constants";
  EXPECT_GT(decimal_divisions, 10) << "/ // % of DECIMAL columns";
  EXPECT_GT(decimal_literal_operands, 10) << "decimal literals in arithmetic";
  EXPECT_GT(decimal_constants, 10) << "decimal select constants";
  EXPECT_GT(decimal_column_comparisons, 10) << "a DECIMAL column against another column";
  EXPECT_GT(decimal_double_comparisons, 10) << "a DECIMAL column against a DOUBLE literal";
  EXPECT_GT(decimal_double_lists, 10) << "an IN list with a DOUBLE literal";
}

// A DECIMAL column meets another column of numbers only where DuckDB's common type of the two
// (ADR 0021 rule 10) stays within 38 digits; beyond it DuckDB fails on a value it cannot hold
// (divergence D13): h DECIMAL(38,0) with x DECIMAL(9,2) needs 40 digits, n DECIMAL(38,30) with
// BIGINT 49 and with INTEGER 40, n with h 68. A DOUBLE column compares with any of them.
TEST(QueryGenerator, DecimalColumnComparisonsStayWithinDuckDbsCommonType) {
  GenTable t{.name = "t", .path = {}, .rows = 10, .columns = {}};
  t.columns = {
      {.name = "h", .kind = ValueKind::kDecimal, .samples = {"5"}, .precision = 38, .scale = 0},
      {.name = "x", .kind = ValueKind::kDecimal, .samples = {"1.50"}, .precision = 9, .scale = 2},
      {.name = "n",
       .kind = ValueKind::kDecimal,
       .samples = {"1." + std::string(30, '0')},
       .precision = 38,
       .scale = 30},
      {.name = "b",
       .kind = ValueKind::kInteger,
       .min = std::numeric_limits<int64_t>::min(),
       .max = std::numeric_limits<int64_t>::max(),
       .samples = {"7"}},
      {.name = "k",
       .kind = ValueKind::kInteger,
       .min = std::numeric_limits<int32_t>::min(),
       .max = std::numeric_limits<int32_t>::max(),
       .samples = {"3"}},
      {.name = "f", .kind = ValueKind::kDouble, .samples = {"0.5"}},
  };
  auto gen = QueryGenerator::Make({t}, 13, {.supported = kSupportedFeatures, .target_percent = 0});
  ASSERT_TRUE(gen.has_value()) << gen.error();
  const auto pair = [](std::string_view a, std::string_view b) {
    const std::string ops = "(=|<>|!=|<=|>=|<|>)";
    return std::regex(std::format(
        R"re("?\b{0}\b"? ?{2} ?"?\b{1}\b"?(?!\.|\()|"?\b{1}\b"? ?{2} ?"?\b{0}\b"?(?!\.|\())re", a,
        b, ops));
  };
  const std::vector<std::regex> capped = {pair("h", "x"), pair("n", "b"), pair("n", "k"),
                                          pair("n", "h")};
  const std::vector<std::regex> allowed = {pair("x", "b"), pair("h", "b"), pair("h", "k"),
                                           pair("n", "x"), pair("n", "f")};
  std::vector<int> seen(allowed.size(), 0);
  for (uint64_t i = 0; i < 5000; ++i) {
    const auto q = gen->Generate(i);
    if (q.features.Has(Feature::kLayout)) {
      continue;  // layout comments could split a match
    }
    const std::string sql = Lower(q.sql);
    for (const std::regex& r : capped) {
      EXPECT_FALSE(std::regex_search(sql, r)) << q.sql;
    }
    for (std::size_t k = 0; k < allowed.size(); ++k) {
      seen[k] += std::regex_search(sql, allowed[k]) ? 1 : 0;
    }
  }
  for (std::size_t k = 0; k < allowed.size(); ++k) {
    EXPECT_GT(seen[k], 5) << "allowed pair " << k;
  }
}

// The values of each CASE in a lower-case query without layout comments: the operand after each
// THEN and ELSE (a minus kept with its literal, quotes dropped), the first THEN value first, and
// whether the CASE is an aggregate's argument.
struct CaseValues {
  std::vector<std::string> values;
  bool aggregated = false;
  bool summed = false;
};
std::vector<CaseValues> CasesOf(const std::string& sql) {
  static const std::regex case_expr(R"re(\bcase\b(.*?)\bend\b)re");
  static const std::regex value(R"re(\b(then|else) ?(-? ?"?[a-z0-9_.]+"?))re");
  std::vector<CaseValues> out;
  for (auto it = std::sregex_iterator(sql.begin(), sql.end(), case_expr);
       it != std::sregex_iterator(); ++it) {
    CaseValues c;
    const std::string before = sql.substr(0, static_cast<std::size_t>(it->position()));
    static const std::regex aggregate(R"re((sum|avg|min|max|count)\((distinct )?$)re");
    static const std::regex sum(R"re((sum|avg)\($)re");
    c.aggregated = std::regex_search(before, aggregate);
    c.summed = std::regex_search(before, sum);
    const std::string body = (*it)[1].str();
    for (auto v = std::sregex_iterator(body.begin(), body.end(), value);
         v != std::sregex_iterator(); ++v) {
      std::string text = (*v)[2].str();
      std::erase(text, '"');
      std::erase(text, ' ');
      c.values.push_back(std::move(text));
    }
    out.push_back(std::move(c));
  }
  return out;
}

// DECIMAL CASE values are cast to DuckDB's folded type (ADR 0021 rule 10), and the generator writes
// none that could fail: b (BIGINT values of 13 digits) never next to n (DECIMAL(38,30): 8 integer
// digits), k (INTEGER values of 4 digits) may be. As an aggregate's argument a CASE keeps its first
// value's scale (x next to n or h would change it), and under SUM and AVG no value's sum leaves
// 38 digits (h's would).
TEST(QueryGenerator, DecimalCaseValuesNeverFailToCast) {
  GenTable t{.name = "t", .path = {}, .rows = 10, .columns = {}};
  Int128 h_max = 1;
  for (int i = 0; i < 38; ++i) {
    h_max *= 10;
  }
  h_max -= 1;
  Int128 n_max = 1;
  for (int i = 0; i < 30; ++i) {
    n_max *= 10;
  }
  t.columns = {
      {.name = "h",
       .kind = ValueKind::kDecimal,
       .samples = {"5"},
       .precision = 38,
       .scale = 0,
       .abs_max = h_max},
      {.name = "x",
       .kind = ValueKind::kDecimal,
       .samples = {"1.50"},
       .precision = 9,
       .scale = 2,
       .abs_max = Int128{150}},
      {.name = "n",
       .kind = ValueKind::kDecimal,
       .samples = {"1." + std::string(30, '0')},
       .precision = 38,
       .scale = 30,
       .abs_max = n_max},
      {.name = "b",
       .kind = ValueKind::kInteger,
       .min = std::numeric_limits<int64_t>::min(),
       .max = std::numeric_limits<int64_t>::max(),
       .samples = {"7"},
       .data_min = -1'000'000'000'000,
       .data_max = 1'000'000'000'000},
      {.name = "k",
       .kind = ValueKind::kInteger,
       .min = std::numeric_limits<int32_t>::min(),
       .max = std::numeric_limits<int32_t>::max(),
       .samples = {"3"},
       .data_min = -1000,
       .data_max = 1000},
  };
  auto gen = QueryGenerator::Make({t}, 17, {.supported = kSupportedFeatures, .target_percent = 0});
  ASSERT_TRUE(gen.has_value()) << gen.error();
  int n_with_k = 0;
  int x_with_b = 0;
  int aggregated = 0;
  for (uint64_t i = 0; i < 10000; ++i) {
    const auto q = gen->Generate(i);
    if (q.features.Has(Feature::kLayout)) {
      continue;  // layout comments could split a match
    }
    for (const CaseValues& c : CasesOf(Lower(q.sql))) {
      const auto has = [&](std::string_view name) { return std::ranges::contains(c.values, name); };
      EXPECT_FALSE(has("n") && has("b")) << q.sql;
      if (c.aggregated && !c.values.empty() && c.values.front() == "x") {
        EXPECT_FALSE(has("n") || has("h")) << q.sql;
      }
      if (c.summed) {
        EXPECT_FALSE(has("h")) << q.sql;
      }
      n_with_k += has("n") && has("k") ? 1 : 0;
      x_with_b += has("x") && has("b") ? 1 : 0;
      aggregated += c.aggregated && c.values.size() > 1 ? 1 : 0;
    }
  }
  EXPECT_GT(n_with_k, 5);
  EXPECT_GT(x_with_b, 5);
  EXPECT_GT(aggregated, 5);
}

TEST(QueryGenerator, DecimalLiteralsStayWithinTheColumnsDigits) {
  GenTable t{.name = "t", .path = {}, .rows = 10, .columns = {}};
  t.columns = {{.name = "n",
                .kind = ValueKind::kDecimal,
                .samples = {"99999999." + std::string(30, '0'), "-1.5" + std::string(29, '0')},
                .precision = 38,
                .scale = 30}};
  auto gen = QueryGenerator::Make({t}, 5, {.supported = kSupportedFeatures, .target_percent = 0});
  ASSERT_TRUE(gen.has_value()) << gen.error();
  static const std::regex number_pattern(R"re([0-9]+(\.[0-9]+)?)re");
  int betweens = 0;
  for (uint64_t i = 0; i < 3000; ++i) {
    const std::string sql = Lower(gen->Generate(i).sql);
    betweens += sql.contains("between") ? 1 : 0;
    // Layout comments hold no digits; the table and column names none either.
    for (auto it = std::sregex_iterator(sql.begin(), sql.end(), number_pattern);
         it != std::sregex_iterator(); ++it) {
      const std::string number = it->str();
      const std::size_t integer_digits = number.contains('.') ? number.find('.') : number.size();
      // LIMIT and OFFSET counts and select positions are small, the select constant 3000000000 is
      // no literal of n; every literal of n has at most 8 integer digits.
      if (number != "3000000000") {
        EXPECT_LE(integer_digits, 8U) << sql;
      }
    }
  }
  EXPECT_GT(betweens, 10);
}

// The data range of a DECIMAL column keeps its SUM and arithmetic inside their types (an overflow
// fails both engines, a SUM beyond 38 digits only antb1, D18): h (DECIMAL(38,0) at its largest
// value) gets neither SUM, AVG nor a constant, only unary -, / and // and % by an integer, and
// % 2.5 (39 digits: DOUBLE, -0 for a negative multiple) only as a select item; x (DECIMAL(18,4),
// capped to 18 digits by * k) gets + k and SUM but not * 2 or * 1.5; y (DECIMAL(38,10), 10^38 -
// 10^12 at most, one row) gets + 1 and + 7 but not + 100, which needs one more digit than
// DECIMAL(38,10) holds.
TEST(QueryGenerator, DecimalDataRangesKeepSumsAndArithmeticInTheirTypes) {
  Int128 digits38 = 1;
  for (int i = 0; i < 38; ++i) {
    digits38 *= 10;
  }
  digits38 -= 1;  // 10^38 - 1
  GenTable full{.name = "full", .path = {}, .rows = 10, .columns = {}};
  full.columns = {{.name = "h",
                   .kind = ValueKind::kDecimal,
                   .samples = {"5"},
                   .precision = 38,
                   .scale = 0,
                   .abs_max = digits38},
                  {.name = "x",
                   .kind = ValueKind::kDecimal,
                   .samples = {"1.5000"},
                   .precision = 18,
                   .scale = 4,
                   .abs_max = Int128{600'000'000'000'000'000}}};
  GenTable one{.name = "one", .path = {}, .rows = 1, .columns = {}};
  one.columns = {{.name = "y",
                  .kind = ValueKind::kDecimal,
                  .samples = {"2.0000000000"},
                  .precision = 38,
                  .scale = 10,
                  .abs_max = digits38 + 1 - Int128{1'000'000'000'000}}};
  auto gen = QueryGenerator::Make({full, one}, 11,
                                  {.supported = kSupportedFeatures, .target_percent = 100});
  ASSERT_TRUE(gen.has_value()) << gen.error();
  static const std::regex h_sum(R"re((sum|avg)\((distinct )?"?h\b)re");
  static const std::regex h_constant(R"re("?\bh\b"? ?[-+*] ?[0-9])re");
  static const std::regex h_negated(R"re(- ?"?h\b)re");
  static const std::regex h_division(R"re("?\bh\b"? ?(/|//|%) ?[0-9])re");
  static const std::regex x_times(R"re("?\bx\b"? ?\* ?[0-9])re");
  static const std::regex h_wide_modulo(R"re("?\bh\b"? ?% ?2\.5)re");
  static const std::regex h_wide_item(R"re((select|,) "?h"? ?% ?2\.5)re");
  static const std::regex x_plus(R"re("?\bx\b"? ?[-+] ?[0-9])re");
  static const std::regex x_sum(R"re((sum|avg)\( ?"?x\b)re");
  static const std::regex y_hundred(R"re("?\by\b"? ?[-+] ?100\b)re");
  static const std::regex y_small(R"re("?\by\b"? ?[-+] ?[17]\b)re");
  int h_negations = 0;
  int h_divisions = 0;
  int h_wide_moduli = 0;
  int x_additions = 0;
  int x_sums = 0;
  int y_additions = 0;
  for (uint64_t i = 0; i < 5000; ++i) {
    const auto q = gen->Generate(i);
    if (q.features.Has(Feature::kLayout)) {
      continue;  // layout comments could split a match
    }
    const std::string sql = Lower(q.sql);
    EXPECT_FALSE(std::regex_search(sql, h_sum)) << q.sql;
    EXPECT_FALSE(std::regex_search(sql, h_constant)) << q.sql;
    EXPECT_FALSE(std::regex_search(sql, x_times)) << q.sql;
    const bool wide = std::regex_search(sql, h_wide_modulo);
    EXPECT_EQ(wide, std::regex_search(sql, h_wide_item)) << q.sql;
    h_wide_moduli += wide ? 1 : 0;
    EXPECT_FALSE(std::regex_search(sql, y_hundred)) << q.sql;
    h_negations += std::regex_search(sql, h_negated) ? 1 : 0;
    h_divisions += std::regex_search(sql, h_division) ? 1 : 0;
    x_additions += std::regex_search(sql, x_plus) ? 1 : 0;
    x_sums += std::regex_search(sql, x_sum) ? 1 : 0;
    y_additions += std::regex_search(sql, y_small) ? 1 : 0;
  }
  EXPECT_GT(h_negations, 10);
  EXPECT_GT(h_divisions, 10);
  EXPECT_GT(h_wide_moduli, 2);
  EXPECT_GT(x_additions, 10);
  EXPECT_GT(x_sums, 10);
  EXPECT_GT(y_additions, 10);
}

TEST(QueryGenerator, MakeRejectsWhatCannotBeGenerated) {
  EXPECT_FALSE(QueryGenerator::Make({}, 1, {}).has_value());
  EXPECT_FALSE(QueryGenerator::Make(Tables(), 1, {.supported = {Feature::kCountStar}}).has_value())
      << "no table reference";
  EXPECT_FALSE(QueryGenerator::Make(Tables(), 1, {.supported = {Feature::kTableName}}).has_value())
      << "no select list";
  EXPECT_FALSE(
      QueryGenerator::Make(Tables(), 1, {.supported = kSupportedFeatures, .target_percent = 101})
          .has_value());
  // A path-only table set still works when only names are supported for others.
  auto only_paths = Tables();
  for (auto& t : only_paths) {
    t.path.clear();
  }
  EXPECT_FALSE(
      QueryGenerator::Make(only_paths, 1, {.supported = {Feature::kCountStar, Feature::kTablePath}})
          .has_value());
}

// antb1 returns a FLOAT column's values as DOUBLE, DuckDB as FLOAT (divergence D11): a FLOAT
// column is never referenced, and its table gets no SELECT *.
TEST(LoadGenTables, SkipsFloatColumns) {
  const std::filesystem::path dir =
      std::filesystem::path(::testing::TempDir()) / "antb1_query_gen_float";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const std::string path = (dir / "t.parquet").string();
  arrow::Int32Builder i;
  arrow::FloatBuilder f;
  arrow::DoubleBuilder d;
  for (int v = 0; v < 3; ++v) {
    ASSERT_TRUE(i.Append(v).ok());
    ASSERT_TRUE(f.Append(0.1F * static_cast<float>(v)).ok());
    ASSERT_TRUE(d.Append(0.5 * v).ok());
  }
  const auto table = arrow::Table::Make(
      arrow::schema({arrow::field("i", arrow::int32()), arrow::field("f", arrow::float32()),
                     arrow::field("d", arrow::float64())}),
      {i.Finish().ValueOrDie(), f.Finish().ValueOrDie(), d.Finish().ValueOrDie()});
  auto out = arrow::io::FileOutputStream::Open(path).ValueOrDie();
  ASSERT_TRUE(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), out, 3).ok());
  ASSERT_TRUE(out->Close().ok());

  const auto tables = LoadGenTables({TableDef{.name = "t", .files = {path}, .patterns = {path}}});
  ASSERT_TRUE(tables.has_value()) << tables.error();
  ASSERT_EQ(tables->size(), 1U);
  std::vector<std::string> names;
  for (const auto& c : tables->front().columns) {
    names.push_back(c.name);
  }
  EXPECT_EQ(names, (std::vector<std::string>{"i", "d"}));
  EXPECT_TRUE(tables->front().other_columns);
  std::filesystem::remove_all(dir);
}

// A DECIMAL column keeps its precision and scale, and its samples are literals with a digit before
// the point, and its data range the largest magnitude (none when every value is NULL; the largest
// Int128 for the 128-bit minimum, which a file may hold); a DECIMAL beyond 38 digits is skipped
// like a FLOAT.
TEST(LoadGenTables, ReadsDecimalColumns) {
  // 2^127: arrow::Decimal128 keeps its low 128 bits, the 128-bit minimum.
  const std::string int128_min_text = "170141183460469231731687303715884105728";
  const std::filesystem::path dir =
      std::filesystem::path(::testing::TempDir()) / "antb1_query_gen_decimal";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const std::string path = (dir / "t.parquet").string();
  arrow::Decimal128Builder small(arrow::decimal128(9, 2));
  arrow::Decimal256Builder wide(arrow::decimal256(40, 0));
  arrow::Decimal128Builder nulls(arrow::decimal128(9, 2));
  arrow::Decimal128Builder minimum(arrow::decimal128(38, 0));
  for (const int64_t v : {5, -1234, 0, 100}) {
    ASSERT_TRUE(small.Append(arrow::Decimal128(v)).ok());
    ASSERT_TRUE(wide.Append(arrow::Decimal256(v)).ok());
    ASSERT_TRUE(nulls.AppendNull().ok());
    ASSERT_TRUE(
        minimum.Append(v == 0 ? arrow::Decimal128(int128_min_text) : arrow::Decimal128(v)).ok());
  }
  const auto table =
      arrow::Table::Make(arrow::schema({arrow::field("p", arrow::decimal128(9, 2)),
                                        arrow::field("w", arrow::decimal256(40, 0)),
                                        arrow::field("n", arrow::decimal128(9, 2)),
                                        arrow::field("m", arrow::decimal128(38, 0))}),
                         {small.Finish().ValueOrDie(), wide.Finish().ValueOrDie(),
                          nulls.Finish().ValueOrDie(), minimum.Finish().ValueOrDie()});
  auto out = arrow::io::FileOutputStream::Open(path).ValueOrDie();
  ASSERT_TRUE(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), out, 4).ok());
  ASSERT_TRUE(out->Close().ok());

  const auto tables = LoadGenTables({TableDef{.name = "t", .files = {path}, .patterns = {path}}});
  ASSERT_TRUE(tables.has_value()) << tables.error();
  ASSERT_EQ(tables->front().columns.size(), 3U);
  const GenColumn& p = tables->front().columns.front();
  EXPECT_EQ(p.kind, ValueKind::kDecimal);
  EXPECT_EQ(p.precision, 9);
  EXPECT_EQ(p.scale, 2);
  EXPECT_EQ(p.samples, (std::vector<std::string>{"0.05", "-12.34", "0.00", "1.00"}));
  EXPECT_EQ(p.abs_max, std::optional<Int128>(1234));
  EXPECT_EQ(tables->front().columns[1].name, "n");
  EXPECT_FALSE(tables->front().columns[1].abs_max.has_value());
  EXPECT_EQ(tables->front().columns[2].name, "m");
  EXPECT_EQ(tables->front().columns[2].abs_max, std::optional<Int128>(kInt128Max));
  EXPECT_TRUE(tables->front().other_columns);
  std::filesystem::remove_all(dir);
}

// JoinRowBound is a true upper bound: against the rows of inner joins of seeded small multisets of
// keys (NULL keys never match), symmetric, often exact, and saturating instead of wrapping.
TEST(JoinRowBound, BoundsTheJoinsOfSmallKeyMultisets) {
  uint64_t state = 0x5EED;
  const auto next = [&state] {
    state = (state * 6364136223846793005ULL) + 1442695040888963407ULL;
    return state >> 33U;
  };
  int exact = 0;
  for (int trial = 0; trial < 2000; ++trial) {
    const uint64_t domain = 1 + (next() % 8);
    std::array<std::vector<std::optional<uint64_t>>, 2> sides;
    for (auto& side : sides) {
      for (uint64_t n = next() % 25; n > 0; --n) {
        side.push_back(next() % 5 == 0 ? std::nullopt : std::optional(next() % domain));
      }
    }
    std::array<GenKeyStats, 2> stats;
    std::array<std::vector<int64_t>, 2> counts;
    for (std::size_t s = 0; s < 2; ++s) {
      counts[s].assign(domain, 0);
      for (const auto& key : sides[s]) {
        if (key.has_value()) {
          ++counts[s][*key];
          ++stats[s].non_null;
        }
      }
      for (const int64_t n : counts[s]) {
        stats[s].distinct += n > 0 ? 1 : 0;
        stats[s].max_multiplicity = std::max(stats[s].max_multiplicity, n);
      }
    }
    int64_t rows = 0;
    for (uint64_t key = 0; key < domain; ++key) {
      rows += counts[0][key] * counts[1][key];
    }
    const int64_t bound = JoinRowBound(stats[0], stats[1]);
    EXPECT_GE(bound, rows) << trial;
    EXPECT_EQ(bound, JoinRowBound(stats[1], stats[0])) << trial;
    exact += bound == rows ? 1 : 0;
  }
  EXPECT_GT(exact, 200);
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  EXPECT_EQ(JoinRowBound(Stats(kMax, kMax, kMax), Stats(kMax, kMax, kMax)), kMax);
  // The three-way product (2^100) overflows alone: the other two decide, never a wrapped value.
  constexpr int64_t kTwoTo30 = 1'073'741'824;
  const GenKeyStats wide = Stats(10, kTwoTo30 * 1024, kTwoTo30);
  EXPECT_EQ(JoinRowBound(wide, wide), 10 * kTwoTo30);
  EXPECT_EQ(JoinRowBound(Stats(3000, 40, 75), Stats(0, 0, 0)), 0) << "an empty side";
}

// Arrays with NULLs for the Parquet files of the key statistics test.
std::shared_ptr<arrow::Array> Int64s(const std::vector<std::optional<int64_t>>& values) {
  arrow::Int64Builder b;
  for (const auto& v : values) {
    EXPECT_TRUE((v.has_value() ? b.Append(*v) : b.AppendNull()).ok());
  }
  return b.Finish().ValueOrDie();
}

std::shared_ptr<arrow::Array> Int32s(const std::vector<std::optional<int32_t>>& values) {
  arrow::Int32Builder b;
  for (const auto& v : values) {
    EXPECT_TRUE((v.has_value() ? b.Append(*v) : b.AppendNull()).ok());
  }
  return b.Finish().ValueOrDie();
}

std::shared_ptr<arrow::Array> UInt16s(const std::vector<uint16_t>& values) {
  arrow::UInt16Builder b;
  for (const uint16_t v : values) {
    EXPECT_TRUE(b.Append(v).ok());
  }
  return b.Finish().ValueOrDie();
}

std::shared_ptr<arrow::Array> Strings(const std::vector<std::optional<std::string>>& values) {
  arrow::StringBuilder b;
  for (const auto& v : values) {
    EXPECT_TRUE((v.has_value() ? b.Append(*v) : b.AppendNull()).ok());
  }
  return b.Finish().ValueOrDie();
}

std::shared_ptr<arrow::Array> Decimals(const std::vector<std::optional<int64_t>>& unscaled) {
  arrow::Decimal128Builder b(arrow::decimal128(9, 2));
  for (const auto& v : unscaled) {
    EXPECT_TRUE((v.has_value() ? b.Append(arrow::Decimal128(*v)) : b.AppendNull()).ok());
  }
  return b.Finish().ValueOrDie();
}

std::shared_ptr<arrow::Array> Dates(const std::vector<std::optional<int32_t>>& days) {
  arrow::Date32Builder b;
  for (const auto& v : days) {
    EXPECT_TRUE((v.has_value() ? b.Append(*v) : b.AppendNull()).ok());
  }
  return b.Finish().ValueOrDie();
}

std::shared_ptr<arrow::Array> Floats(std::size_t n) {
  arrow::FloatBuilder b;
  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_TRUE(b.Append(0.5F).ok());
  }
  return b.Finish().ValueOrDie();
}

std::shared_ptr<arrow::Array> Doubles(std::size_t n) {
  arrow::DoubleBuilder b;
  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_TRUE(b.Append(0.25).ok());
  }
  return b.Finish().ValueOrDie();
}

void WriteParquet(const std::shared_ptr<arrow::Table>& table, const std::string& path,
                  int64_t group_rows) {
  auto out = arrow::io::FileOutputStream::Open(path).ValueOrDie();
  ASSERT_TRUE(
      parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), out, group_rows).ok());
  ASSERT_TRUE(out->Close().ok());
}

// A child table in two files references a parent (two row groups) and an empty table: the key
// statistics count over every file, without NULL keys (a key of two columns with one NULL part
// neither), with the case and trailing spaces of strings and DECIMAL keys by value. Ref columns
// and tables resolve ASCII case-insensitively; refs on a FLOAT, a DOUBLE or a column typed through
// clickbench, and to a table the call does not load, are dropped; an unknown column is an error.
TEST(LoadGenTables, ComputesKeyStatisticsOfRefs) {
  const std::filesystem::path dir =
      std::filesystem::path(::testing::TempDir()) / "antb1_query_gen_refs";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto file = [&dir](std::string_view name) { return (dir / name).string(); };
  const auto parent_schema = arrow::schema(
      {arrow::field("id", arrow::int64()), arrow::field("Code", arrow::utf8()),
       arrow::field("dec", arrow::decimal128(9, 2)), arrow::field("day", arrow::date32()),
       arrow::field("f", arrow::float32()), arrow::field("d", arrow::float64())});
  WriteParquet(
      arrow::Table::Make(parent_schema, {Int64s({1, 2, 2, 3, std::nullopt, 5}),
                                         Strings({"a", "A", "a ", "a", "b", std::nullopt}),
                                         Decimals({150, 150, 200, std::nullopt, 325, 325}),
                                         Dates({10, 10, 11, 10, 11, 11}), Floats(6), Doubles(6)}),
      file("parent.parquet"), 3);
  const auto child_schema = arrow::schema(
      {arrow::field("k", arrow::int32()), arrow::field("s", arrow::utf8()),
       arrow::field("m", arrow::decimal128(9, 2)), arrow::field("day", arrow::date32()),
       arrow::field("x", arrow::float32()), arrow::field("y", arrow::float64()),
       arrow::field("EventDate", arrow::uint16())});
  WriteParquet(
      arrow::Table::Make(child_schema,
                         {Int32s({1, 1, 2, std::nullopt}), Strings({"a", "A", "a ", std::nullopt}),
                          Decimals({150, 150, std::nullopt, 325}), Dates({10, 11, 10, 10}),
                          Floats(4), Doubles(4), UInt16s({1, 2, 3, 4})}),
      file("child-0.parquet"), 4);
  WriteParquet(
      arrow::Table::Make(child_schema, {Int32s({2, 2, 7}), Strings({"a", "zz", std::nullopt}),
                                        Decimals({200, 325, 999}), Dates({10, std::nullopt, 11}),
                                        Floats(3), Doubles(3), UInt16s({5, 6, 7})}),
      file("child-1.parquet"), 3);
  WriteParquet(arrow::Table::Make(arrow::schema({arrow::field("e", arrow::int64())}), {Int64s({})}),
               file("empty.parquet"), 1);

  const auto ref = [](std::vector<std::string> columns, std::string table,
                      std::vector<std::string> ref_columns) {
    return ForeignKey{.columns = std::move(columns),
                      .table = std::move(table),
                      .ref_columns = std::move(ref_columns)};
  };
  const TableDef parent{
      .name = "parent", .files = {file("parent.parquet")}, .patterns = {file("parent.parquet")}};
  TableDef child{.name = "child",
                 .files = {file("child-0.parquet"), file("child-1.parquet")},
                 .patterns = {file("child-*.parquet")},
                 .clickbench = true};
  child.refs = {
      ref({"k"}, "parent", {"id"}),
      ref({"K", "DAY"}, "PARENT", {"ID", "Day"}),
      ref({"s"}, "parent", {"code"}),
      ref({"m"}, "parent", {"dec"}),
      ref({"x"}, "parent", {"id"}),           // FLOAT: never referenced (D11)
      ref({"y"}, "parent", {"d"}),            // DOUBLE: no DOUBLE keys
      ref({"EventDate"}, "parent", {"day"}),  // DATE only through clickbench (D2)
      ref({"k"}, "ghost", {"id"}),            // not loaded
      ref({"k"}, "empty", {"e"}),
  };
  const TableDef empty{
      .name = "empty", .files = {file("empty.parquet")}, .patterns = {file("empty.parquet")}};
  const auto tables = LoadGenTables({parent, child, empty});
  ASSERT_TRUE(tables.has_value()) << tables.error();
  ASSERT_EQ(tables->size(), 3U);
  EXPECT_TRUE((*tables)[0].refs.empty());
  EXPECT_TRUE((*tables)[2].refs.empty());
  const std::vector<GenRef>& refs = (*tables)[1].refs;
  ASSERT_EQ(refs.size(), 5U);
  const auto expect = [&](std::size_t i, const std::vector<std::string>& columns,
                          std::string_view table, GenKeyStats stats, GenKeyStats ref_stats) {
    SCOPED_TRACE(i);
    EXPECT_EQ(refs[i].columns, columns);
    EXPECT_EQ(refs[i].table, table);
    EXPECT_EQ(refs[i].stats, stats) << refs[i].stats.non_null << " " << refs[i].stats.distinct
                                    << " " << refs[i].stats.max_multiplicity;
    EXPECT_EQ(refs[i].ref_stats, ref_stats)
        << refs[i].ref_stats.non_null << " " << refs[i].ref_stats.distinct << " "
        << refs[i].ref_stats.max_multiplicity;
  };
  expect(0, {"k"}, "parent", Stats(6, 3, 3), Stats(5, 4, 2));
  expect(1, {"K", "DAY"}, "parent", Stats(5, 4, 2), Stats(5, 5, 1));
  expect(2, {"s"}, "parent", Stats(5, 4, 2), Stats(5, 4, 2));
  expect(3, {"m"}, "parent", Stats(6, 4, 2), Stats(5, 3, 2));
  expect(4, {"k"}, "empty", Stats(6, 3, 3), Stats(0, 0, 0));
  EXPECT_EQ(refs[2].ref_columns, std::vector<std::string>{"code"});

  for (const auto& [columns, ref_columns] :
       {std::pair<std::string, std::string>{"nope", "id"}, {"k", "nope"}}) {
    child.refs = {ref({columns}, "parent", {ref_columns})};
    const auto broken = LoadGenTables({parent, child});
    ASSERT_FALSE(broken.has_value());
    EXPECT_NE(broken.error().find("ref column 'nope'"), std::string::npos) << broken.error();
  }
  std::filesystem::remove_all(dir);
}

}  // namespace
}  // namespace antb1::slt
