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
#include <numeric>
#include <optional>
#include <ranges>
#include <regex>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <gtest/gtest.h>
#include <parquet/arrow/writer.h>

#include "antb1/plan/catalog.h"
#include "antb1/sql/ast.h"
#include "antb1/sql/parser.h"

#include "canonical.h"
#include "join_tables.h"
#include "sql_parser_property.h"
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

// `tables` without their refs: no query joins.
std::vector<GenTable> WithoutRefs(std::vector<GenTable> tables) {
  for (GenTable& t : tables) {
    t.refs.clear();
  }
  return tables;
}

constexpr FeatureSet kJoinFeatures = {Feature::kCommaJoin, Feature::kJoinOn, Feature::kTableAlias,
                                      Feature::kQualifiedName};

FeatureSet Union(FeatureSet a, FeatureSet b) {
  a.Add(b);
  return a;
}

bool Joins(const GeneratedQuery& q) {
  return q.features.Has(Feature::kCommaJoin) || q.features.Has(Feature::kJoinOn);
}

QueryGenerator MakeOver(std::vector<GenTable> tables, uint64_t seed, GeneratorOptions options) {
  auto gen = QueryGenerator::Make(std::move(tables), seed, options);
  EXPECT_TRUE(gen.has_value()) << gen.error();
  return *std::move(gen);
}

QueryGenerator Make(uint64_t seed, GeneratorOptions options) {
  return MakeOver(Tables(), seed, options);
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

// Over tables without refs (Tables()) and with them (JoinTables()) together, so that the join
// features count as seen once kSupportedFeatures declares them (roadmap PR J2b).
TEST(QueryGenerator, SupportedQueriesUseOnlySupportedFeatures) {
  const FeatureSet richer = {Feature::kCountStar,      Feature::kSum,
                             Feature::kTableName,      Feature::kWhere,
                             Feature::kIntegerColumns, Feature::kIntegerLiteral,
                             Feature::kMultipleItems,  Feature::kKeywordCase};
  for (const FeatureSet& supported :
       {kSupportedFeatures, richer, Union(kSupportedFeatures, kJoinFeatures)}) {
    FeatureSet seen;
    for (const auto& tables : {Tables(), JoinTables()}) {
      const auto gen = MakeOver(tables, 7, {.supported = supported, .target_percent = 0});
      for (uint64_t i = 0; i < 500; ++i) {
        const auto q = gen.Generate(i);
        EXPECT_FALSE(q.target_sample);
        EXPECT_FALSE(q.sql.empty());
        EXPECT_TRUE(supported.Contains(q.features))
            << q.sql << "\n  uses " << q.features.Minus(supported).Names();
        seen.Add(q.features);
      }
    }
    EXPECT_EQ(seen, supported) << "seen: " << seen.Names();
  }
}

TEST(QueryGenerator, TargetSamplesCoverTheWholeGrammar) {
  // A Feature without generator support fails here (runner/query_gen.cc must learn it), unless it
  // waits in kGeneratorPending. The joins need tables with refs.
  FeatureSet seen;
  for (const auto& tables : {Tables(), JoinTables()}) {
    const auto gen = MakeOver(tables, 11, {.supported = kSupportedFeatures, .target_percent = 100});
    for (uint64_t i = 0; i < 4000; ++i) {
      const auto q = gen.Generate(i);
      EXPECT_TRUE(q.target_sample);
      seen.Add(q.features);
    }
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

// ---- generated joins (ADR 0022) ----

TEST(QueryGenerator, JoinQueriesAreAPureFunctionOfTheSeed) {
  const GeneratorOptions options{.supported = kSupportedFeatures, .target_percent = 50};
  const auto a = MakeOver(JoinTables(), 42, options);
  const auto b = MakeOver(JoinTables(), 42, options);
  const auto c = MakeOver(JoinTables(), 43, options);
  std::vector<std::string> sql;
  int joins = 0;
  int differ = 0;
  for (uint64_t i = 0; i < 200; ++i) {
    const auto q = a.Generate(i);
    sql.push_back(q.sql);
    joins += Joins(q) ? 1 : 0;
    EXPECT_EQ(q.sql, b.Generate(i).sql) << i;
    differ += q.sql != c.Generate(i).sql ? 1 : 0;
  }
  for (uint64_t i = sql.size(); i-- > 0;) {
    EXPECT_EQ(b.Generate(i).sql, sql[i]) << "in reverse order: " << i;
  }
  EXPECT_GT(joins, 20);
  EXPECT_GT(differ, 50) << "another seed must give other queries";
}

// Every generated query parses, and its canonical form reads back as the same statement: the
// round-trip property of fuzz/sql_parser_property.h (Parse(ToSql), idempotent ToSql, depth).
TEST(QueryGenerator, GeneratedQueriesParseAndRoundTrip) {
  for (const auto& tables : {Tables(), JoinTables()}) {
    for (const unsigned target : {0U, 100U}) {
      const auto gen =
          MakeOver(tables, 19, {.supported = kSupportedFeatures, .target_percent = target});
      for (uint64_t i = 0; i < 2000; ++i) {
        const auto q = gen.Generate(i);
        const auto parsed = sql::Parse(q.sql);
        ASSERT_TRUE(parsed.has_value()) << q.sql << "\n  " << parsed.error().message;
        EXPECT_EQ(fuzz::SqlParserPropertyViolation(q.sql), "") << q.sql;
      }
    }
  }
}

// Over tables without refs the join features change nothing: no query joins, and every query is
// the one the other supported features alone give.
TEST(QueryGenerator, TablesWithoutRefsGiveTheSameQueriesWithJoinFeatures) {
  const FeatureSet base = kSupportedFeatures.Minus(kJoinFeatures);
  for (const auto& tables : {Tables(), WithoutRefs(JoinTables())}) {
    const auto plain = MakeOver(tables, 23, {.supported = base, .target_percent = 0});
    const auto joins =
        MakeOver(tables, 23, {.supported = Union(base, kJoinFeatures), .target_percent = 0});
    for (uint64_t i = 0; i < 2000; ++i) {
      const auto p = plain.Generate(i);
      const auto j = joins.Generate(i);
      EXPECT_EQ(p.sql, j.sql) << i;
      EXPECT_EQ(p.features, j.features) << i;
    }
  }
}

// Every draw that only a join needs comes from a second random stream: over tables with refs, a
// query that joins nothing is the query the tables give without refs, whichever table it reads.
// (At target 100 the supported set is ignored: the refs make the only difference.)
TEST(QueryGenerator, JoinFeaturesChangeOnlyJoinQueries) {
  const GeneratorOptions options{.supported = kSupportedFeatures, .target_percent = 100};
  const auto with_refs = MakeOver(JoinTables(), 29, options);
  const auto without = MakeOver(WithoutRefs(JoinTables()), 29, options);
  int joins = 0;
  int same = 0;
  for (uint64_t i = 0; i < 2000; ++i) {
    const auto q = with_refs.Generate(i);
    const auto plain = without.Generate(i);
    EXPECT_FALSE(Joins(plain)) << plain.sql;
    if (Joins(q)) {
      ++joins;
      continue;
    }
    ++same;
    EXPECT_EQ(q.sql, plain.sql) << i;
    EXPECT_EQ(q.features, plain.features) << i;
  }
  EXPECT_GT(joins, 400) << "more than 20% of the queries join";
  EXPECT_GT(same, 400);
}

// A join that fails fails before the first draw from the first stream, so its query falls back
// to the query the tables give without refs. At target 100 no join fails; at target 0 partial
// join feature sets make some fail: without kTableAlias, a join from dims read by its glob path
// (only an alias names it, ADR 0022 rule 1), or from slots read by a path that names another
// table.
TEST(QueryGenerator, FailedJoinsFallBackToTheQueriesWithoutRefs) {
  const FeatureSet base = kSupportedFeatures.Minus(kJoinFeatures);
  for (const FeatureSet& join : {FeatureSet{Feature::kCommaJoin, Feature::kJoinOn},
                                 FeatureSet{Feature::kCommaJoin, Feature::kQualifiedName}}) {
    SCOPED_TRACE(join.Names());
    const GeneratorOptions options{.supported = Union(base, join), .target_percent = 0};
    const auto with_refs = MakeOver(JoinTables(), 47, options);
    const auto without = MakeOver(WithoutRefs(JoinTables()), 47, options);
    int joins = 0;
    int dims_by_path = 0;  // 60% of them come from a join that failed
    for (uint64_t i = 0; i < 2000; ++i) {
      const auto q = with_refs.Generate(i);
      if (Joins(q)) {
        ++joins;
        continue;
      }
      dims_by_path += q.table == "dims" && q.features.Has(Feature::kTablePath) ? 1 : 0;
      const auto plain = without.Generate(i);
      EXPECT_EQ(q.sql, plain.sql) << i;
      EXPECT_EQ(q.features, plain.features) << i;
    }
    EXPECT_GT(joins, 400);
    EXPECT_GT(dims_by_path, 30);
  }
}

bool SameName(std::string_view a, std::string_view b) {
  return plan::AsciiLower(a) == plan::AsciiLower(b);
}

bool HasColumn(const GenTable& t, std::string_view name) {
  return std::ranges::any_of(t.columns, [&](const GenColumn& c) { return SameName(c.name, name); });
}

// The name of a FROM '<path>' item without an alias (ADR 0022 rule 1): its file name up to the
// first dot, leading dots skipped, or the whole path when it has a glob character.
std::string PathBindingName(std::string_view path) {
  if (path.find_first_of("*?[") != std::string_view::npos) {
    return std::string(path);
  }
  std::string_view file = path.substr(path.find_last_of('/') + 1);
  file.remove_prefix(std::min(file.find_first_not_of('.'), file.size()));
  return std::string(file.substr(0, file.find('.')));
}

// Calls `f` on every node of an expression tree, parents first.
template <class F>
void ForEachNode(const sql::Expr& e, const F& f) {
  f(e);
  std::visit(
      [&f](const auto& node) {
        using Node = std::remove_cvref_t<decltype(node)>;
        if constexpr (std::is_same_v<Node, sql::AggregateCall>) {
          if (node.arg.has_value()) {
            ForEachNode(**node.arg, f);
          }
        } else if constexpr (std::is_same_v<Node, sql::UnaryExpr> ||
                             std::is_same_v<Node, sql::CastExpr>) {
          ForEachNode(*node.operand, f);
        } else if constexpr (std::is_same_v<Node, sql::BinaryExpr>) {
          ForEachNode(*node.left, f);
          ForEachNode(*node.right, f);
        } else if constexpr (std::is_same_v<Node, sql::LikeExpr>) {
          ForEachNode(*node.operand, f);
          ForEachNode(*node.pattern, f);
        } else if constexpr (std::is_same_v<Node, sql::InExpr>) {
          ForEachNode(*node.operand, f);
          for (const sql::Expr& value : node.list) {
            ForEachNode(value, f);
          }
        } else if constexpr (std::is_same_v<Node, sql::BetweenExpr>) {
          ForEachNode(*node.operand, f);
          ForEachNode(*node.low, f);
          ForEachNode(*node.high, f);
        } else if constexpr (std::is_same_v<Node, sql::FunctionCall>) {
          for (const sql::Expr& arg : node.args) {
            ForEachNode(arg, f);
          }
        } else if constexpr (std::is_same_v<Node, sql::CaseExpr>) {
          if (node.operand.has_value()) {
            ForEachNode(**node.operand, f);
          }
          for (const sql::CaseBranch& branch : node.branches) {
            ForEachNode(*branch.when, f);
            ForEachNode(*branch.then, f);
          }
          if (node.otherwise.has_value()) {
            ForEachNode(**node.otherwise, f);
          }
        } else if constexpr (std::is_same_v<Node, sql::ExtractExpr>) {
          ForEachNode(*node.source, f);
        }
      },
      static_cast<const sql::ExprNode&>(e));
}

const sql::ColumnRef* AsColumn(const sql::Expr& e) {
  return std::get_if<sql::ColumnRef>(&static_cast<const sql::ExprNode&>(e));
}

// A ref of JoinTables() that joins can follow.
struct JoinRef {
  std::string child;
  std::vector<std::string> columns;
  std::string parent;
  std::vector<std::string> ref_columns;
};

std::vector<JoinRef> UsableJoinRefs() {
  return {
      {.child = "facts", .columns = {"f_dim"}, .parent = "dims", .ref_columns = {"d_id"}},
      {.child = "facts", .columns = {"f_alt"}, .parent = "dims", .ref_columns = {"d_id"}},
      {.child = "facts", .columns = {"f_code"}, .parent = "codes", .ref_columns = {"c_code"}},
      {.child = "facts",
       .columns = {"f_dim", "f_day"},
       .parent = "slots",
       .ref_columns = {"s_dim", "s_day"}},
      {.child = "facts", .columns = {"f_amount"}, .parent = "dims", .ref_columns = {"d_amount"}},
      {.child = "dims", .columns = {"d_parent"}, .parent = "dims", .ref_columns = {"d_id"}},
  };
}

// What the join queries of a run covered (CheckJoin).
struct JoinCoverage {
  int queries = 0;
  int three_tables = 0;
  int comma = 0;
  int cross = 0;
  int join = 0;
  int inner = 0;
  int alias_with_as = 0;
  int alias_without_as = 0;
  int path = 0;
  int path_qualifier = 0;
  int dotted_path_name = 0;
  int repeated_table = 0;
  int facts_twice = 0;
  int two_column_key = 0;
  int decimal_key = 0;
  int empty_table = 0;
  int sibling_on = 0;
  int closing_edge = 0;
  int on_condition = 0;
  int unconnected_start = 0;
  int star_without_limit = 0;
  int decimal_comparison = 0;
};

// Checks a generated join over JoinTables() against the algorithm, on its AST: 2 or 3 inner FROM
// items of distinct names (ADR 0022 rule 1); every column reference names one item that has the
// column (no ambiguous or hidden names, every reference qualified next to slots, whose skipped
// columns are unknown); an ON reads only its own and earlier items (rule 10); the top-level
// equalities of refs' keys in WHERE and ON connect every item, and never compare columns a join
// cannot (DOUBLE, 48 DECIMAL digits, INTEGER to VARCHAR); SELECT * has a LIMIT unless the bound
// is at most 50; no column typed through clickbench next to a path item (D2); facts stands twice
// only through the empty codes (bound 0); and the features match the AST.
void CheckJoin(const GeneratedQuery& q, const std::vector<GenTable>& tables, JoinCoverage& cov) {
  SCOPED_TRACE(q.sql);
  const auto parsed = sql::Parse(q.sql);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
  const sql::SelectStatement& stmt = *parsed;
  ASSERT_GE(stmt.from.size(), 2U);
  ASSERT_LE(stmt.from.size(), 3U);
  ++cov.queries;
  cov.three_tables += stmt.from.size() == 3 ? 1 : 0;
  struct Item {
    const GenTable* table = nullptr;
    std::string name;
    bool path = false;
    bool aliased = false;
  };
  std::vector<Item> items;
  bool comma = false;
  bool join_on = false;
  for (const sql::FromItem& item : stmt.from) {
    ASSERT_NE(item.connector, sql::Connector::kLeft);
    const bool path = item.table.kind == sql::TableRef::Kind::kPath;
    const auto table = std::ranges::find_if(tables, [&](const GenTable& t) {
      return path ? t.path == item.table.name : SameName(t.name, item.table.name);
    });
    ASSERT_NE(table, tables.end()) << item.table.name;
    std::string name =
        item.alias.value_or(path ? PathBindingName(item.table.name) : item.table.name);
    for (const Item& other : items) {
      EXPECT_FALSE(SameName(other.name, name)) << "two FROM items named " << name;
      cov.repeated_table += other.table == &*table ? 1 : 0;
    }
    comma = comma || item.connector == sql::Connector::kComma ||
            item.connector == sql::Connector::kCross;
    join_on = join_on || item.connector == sql::Connector::kInner;
    cov.comma += item.connector == sql::Connector::kComma ? 1 : 0;
    cov.cross += item.connector == sql::Connector::kCross ? 1 : 0;
    if (item.connector == sql::Connector::kInner) {
      ++cov.join;
      const std::string connector = plan::AsciiLower(
          std::string_view(q.sql).substr(item.connector_span.offset, item.connector_span.length));
      cov.inner += connector.starts_with("inner") ? 1 : 0;
    }
    if (item.alias.has_value()) {
      const std::string alias = plan::AsciiLower(
          std::string_view(q.sql).substr(item.alias_span.offset, item.alias_span.length));
      (alias.starts_with("as") ? cov.alias_with_as : cov.alias_without_as) += 1;
    }
    cov.path += path ? 1 : 0;
    cov.dotted_path_name += path && !item.alias.has_value() && table->name == "codes" ? 1 : 0;
    cov.empty_table += table->rows == 0 ? 1 : 0;
    items.push_back(Item{.table = &*table,
                         .name = std::move(name),
                         .path = path,
                         .aliased = item.alias.has_value()});
  }
  const auto count_of = [&](std::string_view table) {
    return std::ranges::count_if(items, [&](const Item& i) { return i.table->name == table; });
  };
  const bool slots = count_of("slots") > 0;
  const bool any_path = std::ranges::any_of(items, [](const Item& i) { return i.path; });
  if (count_of("facts") > 1) {
    ++cov.facts_twice;
    EXPECT_EQ(q.row_bound, 0) << "facts twice, not through the empty codes";
  }
  // The item a column reference names, or npos (a select alias, or a broken reference).
  constexpr std::size_t kNone = std::string::npos;
  const auto item_of = [&](const sql::ColumnRef& c) {
    std::vector<std::size_t> found;
    for (std::size_t k = 0; k < items.size(); ++k) {
      if (c.qualifier.empty() ? HasColumn(*items[k].table, c.name)
                              : SameName(items[k].name, c.qualifier)) {
        found.push_back(k);
      }
    }
    return found.size() == 1 ? found.front() : kNone;
  };
  bool qualified = false;
  const auto check_columns = [&](const sql::Expr& e, std::size_t scope) {
    ForEachNode(e, [&](const sql::Expr& node) {
      const sql::ColumnRef* c = AsColumn(node);
      if (c == nullptr) {
        return;
      }
      static const std::regex select_alias("a[1-3]");
      if (c->qualifier.empty() && std::ranges::none_of(items, [&](const Item& i) {
            return HasColumn(*i.table, c->name);
          })) {
        EXPECT_TRUE(std::regex_match(c->name, select_alias)) << c->name << ": no such column";
        return;
      }
      const std::size_t k = item_of(*c);
      ASSERT_NE(k, kNone) << c->qualifier << "." << c->name << ": ambiguous or no such item";
      EXPECT_TRUE(HasColumn(*items[k].table, c->name))
          << c->qualifier << "." << c->name << ": a name the item does not have";
      EXPECT_LE(k, scope) << c->name << ": an ON reads a later FROM item";
      EXPECT_TRUE(!slots || !c->qualifier.empty()) << c->name << ": unqualified next to slots";
      EXPECT_FALSE(any_path && SameName(c->name, "EventDate")) << "clickbench typing (D2)";
      qualified = qualified || !c->qualifier.empty();
      cov.path_qualifier += !c->qualifier.empty() && items[k].path && !items[k].aliased ? 1 : 0;
    });
  };
  // Equalities of a usable ref's key columns between two items: (child item, parent item, ref,
  // key column).
  const std::vector<JoinRef> refs = UsableJoinRefs();
  std::vector<std::array<std::size_t, 4>> halves;
  const auto key_half = [&](const sql::Expr& e) {
    const auto* eq = std::get_if<sql::BinaryExpr>(&static_cast<const sql::ExprNode&>(e));
    if (eq == nullptr || eq->op != sql::BinaryOp::kEq) {
      return false;
    }
    const sql::ColumnRef* l = AsColumn(*eq->left);
    const sql::ColumnRef* r = AsColumn(*eq->right);
    if (l == nullptr || r == nullptr) {
      return false;
    }
    const std::size_t li = item_of(*l);
    const std::size_t ri = item_of(*r);
    if (li == kNone || ri == kNone || li == ri) {
      return false;
    }
    bool found = false;
    for (std::size_t n = 0; n < refs.size(); ++n) {
      for (std::size_t k = 0; k < refs[n].columns.size(); ++k) {
        const auto matches = [&](std::size_t ci, const sql::ColumnRef& cc, std::size_t pi,
                                 const sql::ColumnRef& pc) {
          return items[ci].table->name == refs[n].child && SameName(cc.name, refs[n].columns[k]) &&
                 items[pi].table->name == refs[n].parent &&
                 SameName(pc.name, refs[n].ref_columns[k]);
        };
        if (matches(li, *l, ri, *r)) {
          halves.push_back({li, ri, n, k});
          found = true;
        } else if (matches(ri, *r, li, *l)) {
          halves.push_back({ri, li, n, k});
          found = true;
        }
      }
    }
    return found;
  };
  for (const sql::SelectItem& item : stmt.items) {
    check_columns(item.expr, kNone);
  }
  bool condition = false;  // a WHERE or ON conjunct that is no key
  for (std::size_t k = 0; k < stmt.from.size(); ++k) {
    // Rule 10: the items up to this one; those before the last comma are earlier comma siblings.
    std::size_t group = 0;
    for (std::size_t g = 1; g <= k; ++g) {
      group = stmt.from[g].connector == sql::Connector::kComma ? g : group;
    }
    bool sibling = false;
    bool extra = false;
    for (const sql::Expr& conjunct : stmt.from[k].on) {
      check_columns(conjunct, k);
      ForEachNode(conjunct, [&](const sql::Expr& node) {
        if (const sql::ColumnRef* c = AsColumn(node)) {
          sibling = sibling || item_of(*c) < group;
        }
      });
      extra = !key_half(conjunct) || extra;
    }
    cov.sibling_on += sibling ? 1 : 0;
    cov.on_condition += extra ? 1 : 0;
    condition = condition || extra;
  }
  for (const sql::Expr& conjunct : stmt.where) {
    check_columns(conjunct, kNone);
    condition = !key_half(conjunct) || condition;
  }
  for (const auto& e : stmt.group_by) {
    check_columns(e, kNone);
  }
  for (const sql::Expr& e : stmt.having) {
    check_columns(e, kNone);
  }
  for (const sql::OrderItem& item : stmt.order_by) {
    check_columns(item.expr, kNone);
  }
  // Edges: every key column of a ref equal between the same two items. They connect every item.
  std::vector<std::size_t> component(items.size());
  std::ranges::iota(component, std::size_t{0});
  const auto root = [&](std::size_t k) {
    while (component[k] != k) {
      k = component[k];
    }
    return k;
  };
  std::size_t edges = 0;
  bool first_two = false;
  std::vector<int64_t> bounds;
  for (std::size_t n = 0; n < refs.size(); ++n) {
    for (std::size_t ci = 0; ci < items.size(); ++ci) {
      for (std::size_t pi = 0; pi < items.size(); ++pi) {
        const bool all = std::ranges::all_of(
            std::views::iota(std::size_t{0}, refs[n].columns.size()), [&](std::size_t k) {
              return std::ranges::contains(halves, std::array<std::size_t, 4>{ci, pi, n, k});
            });
        if (ci == pi || !all) {
          continue;
        }
        ++edges;
        component[root(ci)] = root(pi);
        first_two = first_two || (std::min(ci, pi) == 0 && std::max(ci, pi) == 1);
        cov.two_column_key += refs[n].columns.size() == 2 ? 1 : 0;
        cov.decimal_key += refs[n].columns.front() == "f_amount" ? 1 : 0;
        const GenTable& child = *items[ci].table;
        const auto ref = std::ranges::find_if(child.refs, [&](const GenRef& r) {
          return r.table == refs[n].parent && r.columns == refs[n].columns;
        });
        if (ref != child.refs.end()) {
          bounds.push_back(JoinRowBound(ref->stats, ref->ref_stats));
        }
      }
    }
  }
  for (std::size_t k = 0; k < items.size(); ++k) {
    EXPECT_EQ(root(k), root(0)) << "FROM item " << k << " is not connected by keys";
  }
  cov.closing_edge += edges > items.size() - 1 ? 1 : 0;
  cov.unconnected_start += first_two ? 0 : 1;
  EXPECT_LE(q.row_bound, 10'000);
  if (items.size() == 2) {
    EXPECT_TRUE(std::ranges::contains(bounds, q.row_bound)) << "bound " << q.row_bound;
  }
  // Comparisons of two columns never mix what DuckDB compares otherwise: no DOUBLE key, no
  // common DECIMAL type beyond 38 digits (D13: f_wide against d_fine, d_amount, f_amount or
  // s_hours), no INTEGER against VARCHAR.
  const auto comparisons = [&](const sql::Expr& e) {
    ForEachNode(e, [&](const sql::Expr& node) {
      const auto* b = std::get_if<sql::BinaryExpr>(&static_cast<const sql::ExprNode&>(node));
      if (b == nullptr || b->op < sql::BinaryOp::kEq || b->op > sql::BinaryOp::kGe) {
        return;
      }
      const sql::ColumnRef* l = AsColumn(*b->left);
      const sql::ColumnRef* r = AsColumn(*b->right);
      if (l == nullptr || r == nullptr) {
        return;
      }
      const auto pair = [&](std::string_view x, std::string_view y) {
        return (SameName(l->name, x) && SameName(r->name, y)) ||
               (SameName(l->name, y) && SameName(r->name, x));
      };
      EXPECT_FALSE(pair("f_ratio", "d_ratio") && b->op == sql::BinaryOp::kEq);
      EXPECT_FALSE(pair("f_wide", "d_fine") || pair("f_wide", "d_amount") ||
                   pair("f_wide", "f_amount") || pair("f_wide", "s_hours"));
      EXPECT_FALSE(pair("f_id", "c_code"));
      cov.decimal_comparison += pair("d_fine", "f_amount") ? 1 : 0;
    });
  };
  for (const sql::SelectItem& item : stmt.items) {
    comparisons(item.expr);
  }
  for (const sql::FromItem& item : stmt.from) {
    for (const sql::Expr& conjunct : item.on) {
      comparisons(conjunct);
    }
  }
  for (const sql::Expr& conjunct : stmt.where) {
    comparisons(conjunct);
  }
  if (stmt.star) {
    EXPECT_TRUE(stmt.limit.has_value() || q.row_bound <= 50) << "bound " << q.row_bound;
    cov.star_without_limit += stmt.limit.has_value() ? 0 : 1;
    EXPECT_FALSE(any_path && std::ranges::any_of(items,
                                                 [](const Item& i) {
                                                   return std::ranges::any_of(
                                                       i.table->columns, [](const GenColumn& c) {
                                                         return c.via_override;
                                                       });
                                                 }))
        << "SELECT * reads a column typed through clickbench next to a path (D2)";
  }
  EXPECT_EQ(q.features.Has(Feature::kCommaJoin), comma);
  EXPECT_EQ(q.features.Has(Feature::kJoinOn), join_on);
  EXPECT_EQ(q.features.Has(Feature::kTableAlias),
            std::ranges::any_of(items, [](const Item& i) { return i.aliased; }));
  EXPECT_EQ(q.features.Has(Feature::kQualifiedName), qualified);
  EXPECT_EQ(q.features.Has(Feature::kTablePath), any_path);
  EXPECT_EQ(q.features.Has(Feature::kTableName),
            std::ranges::any_of(items, [](const Item& i) { return !i.path; }));
  EXPECT_FALSE(q.features.Has(Feature::kLeftJoin));
  EXPECT_TRUE(!condition || q.features.Has(Feature::kWhere)) << "a condition beyond the keys";
  EXPECT_TRUE(std::ranges::any_of(items, [&](const Item& i) { return i.table->name == q.table; }));
}

TEST(QueryGenerator, JoinQueriesFollowTheRefs) {
  const std::vector<GenTable> tables = JoinTables();
  const auto gen = MakeOver(tables, 31, {.supported = kSupportedFeatures, .target_percent = 100});
  JoinCoverage cov;
  for (uint64_t i = 0; cov.queries < 4000 && i < 20'000; ++i) {
    const auto q = gen.Generate(i);
    if (Joins(q)) {
      CheckJoin(q, tables, cov);
    }
  }
  EXPECT_EQ(cov.queries, 4000);
  EXPECT_GT(cov.three_tables, 0);
  EXPECT_GT(cov.comma, 0);
  EXPECT_GT(cov.cross, 0);
  EXPECT_GT(cov.join, 0);
  EXPECT_GT(cov.inner, 0);
  EXPECT_GT(cov.alias_with_as, 0);
  EXPECT_GT(cov.alias_without_as, 0);
  EXPECT_GT(cov.path, 0);
  EXPECT_GT(cov.path_qualifier, 0);
  EXPECT_GT(cov.dotted_path_name, 0) << "codes by its path without an alias (rule 1)";
  EXPECT_GT(cov.repeated_table, 0) << "self-joins and dims in two roles";
  EXPECT_GT(cov.facts_twice, 0) << "through the empty codes";
  EXPECT_GT(cov.two_column_key, 0);
  EXPECT_GT(cov.decimal_key, 0);
  EXPECT_GT(cov.empty_table, 0);
  EXPECT_GT(cov.sibling_on, 0) << "an ON that reads an earlier comma sibling";
  EXPECT_GT(cov.closing_edge, 0);
  EXPECT_GT(cov.on_condition, 0) << "an ON condition beyond the keys";
  EXPECT_GT(cov.unconnected_start, 0) << "the first two FROM items share no key";
  EXPECT_GT(cov.star_without_limit, 0);
  EXPECT_GT(cov.decimal_comparison, 0) << "DECIMAL columns of two items compared";
}

// Join features declared one by one: a join uses only the declared ones, so the walk takes no
// step that needs an alias (a table twice, a path that names another table) or a qualifier (a key
// column's name in two items, a table with columns the generator skips), and other columns of a
// name two items have are not referenced; supported queries join.
TEST(QueryGenerator, DeclaredJoinFeaturesAppearInSupportedQueries) {
  const std::vector<GenTable> tables = JoinTables();
  for (const FeatureSet& join : {kJoinFeatures, FeatureSet{Feature::kCommaJoin, Feature::kJoinOn},
                                 FeatureSet{Feature::kJoinOn, Feature::kTableAlias},
                                 FeatureSet{Feature::kCommaJoin, Feature::kQualifiedName}}) {
    SCOPED_TRACE(join.Names());
    const FeatureSet base = kSupportedFeatures.Minus(kJoinFeatures);
    const FeatureSet supported = Union(base, join);
    const auto gen = MakeOver(tables, 37, {.supported = supported, .target_percent = 0});
    FeatureSet seen;
    JoinCoverage cov;
    for (uint64_t i = 0; i < 3000; ++i) {
      const auto q = gen.Generate(i);
      EXPECT_TRUE(supported.Contains(q.features))
          << q.sql << "\n  uses " << q.features.Minus(supported).Names();
      seen.Add(q.features);
      if (Joins(q)) {
        CheckJoin(q, tables, cov);
      }
    }
    EXPECT_EQ(seen.Minus(base), join) << "seen: " << seen.Names();
    EXPECT_GT(cov.queries, 300);
  }
}

// An ON condition beyond the keys reads the columns of the items up to its own; with none that a
// literal can meet (no string literals for VARCHAR columns), the ON holds its keys only.
TEST(QueryGenerator, OnConditionsNeedComparableColumnsInScope) {
  GenTable parent{.name = "parent", .path = {}, .rows = 10, .columns = {}};
  parent.columns = {{.name = "p_code", .kind = ValueKind::kVarchar, .samples = {"a"}}};
  GenTable child{.name = "child", .path = {}, .rows = 30, .columns = {}};
  child.columns = {{.name = "c_code", .kind = ValueKind::kVarchar, .samples = {"a"}}};
  child.refs = {GenRef{.columns = {"c_code"},
                       .table = "parent",
                       .ref_columns = {"p_code"},
                       .stats = Stats(30, 10, 3),
                       .ref_stats = Stats(10, 10, 1)}};
  const FeatureSet supported = {Feature::kCountStar, Feature::kCountColumn,
                                Feature::kTableName, Feature::kVarcharColumns,
                                Feature::kJoinOn,    Feature::kWhere};
  const auto gen = MakeOver({parent, child}, 43, {.supported = supported, .target_percent = 0});
  int joins = 0;
  for (uint64_t i = 0; i < 500; ++i) {
    const auto q = gen.Generate(i);
    EXPECT_TRUE(supported.Contains(q.features)) << q.sql;
    EXPECT_FALSE(q.features.Has(Feature::kWhere)) << q.sql;
    joins += Joins(q) ? 1 : 0;
  }
  EXPECT_GT(joins, 100);
}

// An ON condition beyond the keys is one of WHERE's forms (kWhere): joins have them when kWhere is
// supported, and no ON holds more than its keys when it is not.
TEST(QueryGenerator, OnConditionsNeedTheWhereFeature) {
  const std::vector<GenTable> tables = JoinTables();
  for (const bool where : {false, true}) {
    SCOPED_TRACE(where ? "with kWhere" : "without kWhere");
    FeatureSet supported = {Feature::kCountStar, Feature::kTableName, Feature::kIntegerColumns,
                            Feature::kIntegerLiteral, Feature::kJoinOn};
    if (where) {
      supported.Add(Feature::kWhere);
    }
    const auto gen = MakeOver(tables, 41, {.supported = supported, .target_percent = 0});
    JoinCoverage cov;
    for (uint64_t i = 0; i < 1000; ++i) {
      const auto q = gen.Generate(i);
      EXPECT_TRUE(supported.Contains(q.features))
          << q.sql << "\n  uses " << q.features.Minus(supported).Names();
      if (Joins(q)) {
        CheckJoin(q, tables, cov);
      }
    }
    EXPECT_GT(cov.queries, 100);
    EXPECT_EQ(cov.on_condition > 0, where) << cov.on_condition << " ON conditions";
  }
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
