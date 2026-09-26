#include "query_gen.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <string>
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

// A small table (path form allowed) and a large one, with one column of every kind; EventDate is
// DATE only through the clickbench option.
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
  for (uint64_t i = 0; i < 3000; ++i) {
    const auto q = gen.Generate(i);
    const std::string sql = Lower(q.sql);
    const bool projection = q.features.Has(Feature::kColumns) || q.features.Has(Feature::kStar);
    EXPECT_EQ(q.sort, projection ? SortMode::kRowSort : SortMode::kNoSort) << q.sql;
    EXPECT_EQ(q.row_count_only, projection && q.features.Has(Feature::kLimit)) << q.sql;
    if (q.features.Has(Feature::kTablePath)) {
      // DuckDB reads the raw file there: a column typed through the clickbench option differs.
      EXPECT_FALSE(sql.contains("eventdate") || q.features.Has(Feature::kStar)) << q.sql;
    }
    if (q.features.Has(Feature::kStar) && q.table == "big") {
      EXPECT_TRUE(q.features.Has(Feature::kLimit)) << q.sql;
    }
    // Doubles only get literals that both engines convert to the same value.
    EXPECT_FALSE(q.sql.contains("0.1000000000000000055511151231257827")) << q.sql;
    EXPECT_FALSE(q.sql.contains("it's")) << "quotes in string literals are doubled: " << q.sql;
  }
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

}  // namespace
}  // namespace antb1::slt
