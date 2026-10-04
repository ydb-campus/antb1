#include "query_gen.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <limits>
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
  // A Feature without generator support fails here (runner/query_gen.cc must learn it).
  const auto gen = Make(11, {.supported = kSupportedFeatures, .target_percent = 100});
  FeatureSet seen;
  for (uint64_t i = 0; i < 4000; ++i) {
    const auto q = gen.Generate(i);
    EXPECT_TRUE(q.target_sample);
    seen.Add(q.features);
  }
  const FeatureSet expected = FeatureSet::All().Minus(kNeverGenerated);
  EXPECT_EQ(seen, expected) << "never generated: " << expected.Minus(seen).Names();
}

TEST(QueryGenerator, QueriesRespectTheSemanticsBothEnginesShare) {
  const auto gen = Make(3, {.supported = kSupportedFeatures, .target_percent = 100});
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
    // DECIMAL columns: no CASE values yet (D4b); arithmetic with integer and decimal literals,
    // / // % included; no literal with more than 10 fraction digits (w's scale; m has spare
    // digits), which DuckDB would compare in a DECIMAL capped at 38 digits (divergence D13).
    // Layout comments could hide a match.
    if (!q.features.Has(Feature::kLayout)) {
      static const std::regex decimal_misuse(R"re(then "?[mw]\b|else "?[mw]\b)re");
      EXPECT_FALSE(std::regex_search(sql, decimal_misuse)) << q.sql;
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
      if (sql.find(" in") != std::string::npos) {
      }
    }
    static const std::regex long_fraction(R"re(\.[0-9]{11})re");
    EXPECT_FALSE(std::regex_search(sql, long_fraction)) << q.sql;
    // Doubles only get literals that both engines convert to the same value.
    EXPECT_FALSE(q.sql.contains("0.1000000000000000055511151231257827")) << q.sql;
    EXPECT_FALSE(q.sql.contains("it's")) << "quotes in string literals are doubled: " << q.sql;
  }
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
// value) gets neither SUM, AVG nor a constant, only unary -, / and // and % by an integer (% 2.5
// needs 39 digits, which DuckDB computes in DOUBLE); x (DECIMAL(18,4), capped to 18 digits by * k)
// gets + k and SUM but not * 2 or * 1.5; y (DECIMAL(38,10), 10^38 - 10^12 at most, one
// row) gets + 1 and + 7 but not + 100, which needs one more digit than DECIMAL(38,10) holds.
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
  static const std::regex x_plus(R"re("?\bx\b"? ?[-+] ?[0-9])re");
  static const std::regex x_sum(R"re((sum|avg)\( ?"?x\b)re");
  static const std::regex y_hundred(R"re("?\by\b"? ?[-+] ?100\b)re");
  static const std::regex y_small(R"re("?\by\b"? ?[-+] ?[17]\b)re");
  int h_negations = 0;
  int h_divisions = 0;
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
    EXPECT_FALSE(std::regex_search(sql, h_wide_modulo)) << q.sql;
    EXPECT_FALSE(std::regex_search(sql, y_hundred)) << q.sql;
    h_negations += std::regex_search(sql, h_negated) ? 1 : 0;
    h_divisions += std::regex_search(sql, h_division) ? 1 : 0;
    x_additions += std::regex_search(sql, x_plus) ? 1 : 0;
    x_sums += std::regex_search(sql, x_sum) ? 1 : 0;
    y_additions += std::regex_search(sql, y_small) ? 1 : 0;
  }
  EXPECT_GT(h_negations, 10);
  EXPECT_GT(h_divisions, 10);
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

}  // namespace
}  // namespace antb1::slt
