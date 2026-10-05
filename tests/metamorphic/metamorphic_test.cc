// Metamorphic tests (label metamorphic): the relations of relations.h over the Parquet fixtures,
// plus checks that relate antb1 and the tables file to an independent Parquet reader.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <gtest/gtest.h>
#include <parquet/arrow/reader.h>

#include "antb1/engine/session.h"
#include "antb1/plan/types.h"

#include "antb1_engine.h"
#include "engine.h"
#include "fixtures.h"
#include "relations.h"
#include "supported_features.h"
#include "tables.h"

namespace antb1::metamorphic {
namespace {

std::string Substitute(std::string sql) {
  constexpr std::string_view kVar = "${FIXTURES}";
  const std::string dir = antb1::testing::FixturesDir().string();
  for (std::size_t pos = sql.find(kVar); pos != std::string::npos; pos = sql.find(kVar, pos)) {
    sql.replace(pos, kVar.size(), dir);
    pos += dir.size();
  }
  return sql;
}

std::vector<slt::TableDef> LoadFixtureTables() {
  auto tables = slt::LoadTables(antb1::testing::SltTablesFile(), antb1::testing::FixturesDir());
  EXPECT_TRUE(tables.has_value()) << tables.error();
  return tables.value_or(std::vector<slt::TableDef>{});
}

// antb1 sessions over the tables of tests/slt/tables.txt, one per batch size.
class Engines {
 public:
  slt::Engine& For(int64_t batch_size) {
    auto& engine = engines_[batch_size];
    if (!engine) {
      if (tables_.empty()) {
        tables_ = LoadFixtureTables();
      }
      engine::SessionOptions options;
      options.batch_size = batch_size;
      auto made = slt::Antb1Engine::Make(tables_, options);
      EXPECT_TRUE(made.has_value()) << made.error();
      if (made.has_value()) {
        engine = std::move(*made);
      }
    }
    return *engine;
  }

 private:
  std::vector<slt::TableDef> tables_;
  std::map<int64_t, std::unique_ptr<slt::Antb1Engine>> engines_;
};

std::string DescribeProbes(const Relation& r) {
  std::string out;
  for (std::size_t i = 0; i < r.probes.size(); ++i) {
    out += std::format("\n  [{}] batch_size={}: {}", i, r.probes[i].batch_size, r.probes[i].sql);
  }
  return out;
}

class Relations : public ::testing::TestWithParam<Relation> {
 protected:
  static Engines& engines() {
    static Engines instance;
    return instance;
  }
};

TEST_P(Relations, Hold) {
  const Relation& r = GetParam();
  std::vector<slt::ExecResult> answers;
  answers.reserve(r.probes.size());
  for (const auto& probe : r.probes) {
    answers.push_back(engines().For(probe.batch_size).Execute(Substitute(probe.sql)));
  }
  const Verdict v = Evaluate(r, answers, slt::kSupportedFeatures);
  switch (v.kind) {
    case Verdict::Kind::kHolds:
      break;
    case Verdict::Kind::kPending:
      // Counted, never silently skipped: ctest and JUnit list it as skipped, with the reason.
      GTEST_SKIP() << v.message;
    case Verdict::Kind::kViolated:
      FAIL() << "relation " << r.name << " violated: " << v.message << DescribeProbes(r);
    case Verdict::Kind::kBroken:
      FAIL() << "relation " << r.name << ": " << v.message << DescribeProbes(r);
  }
}

INSTANTIATE_TEST_SUITE_P(, Relations, ::testing::ValuesIn(AllRelations()),
                         [](const ::testing::TestParamInfo<Relation>& param_info) {
                           return param_info.param.name;
                         });

slt::ResultSet Single(std::string value) {
  return slt::ResultSet{.classes = {slt::ColumnClass::kInteger},
                        .type_names = {"BIGINT"},
                        .rows = {{std::move(value)}}};
}

// A single DECIMAL value in its canonical text (plan::FormatDecimal).
slt::ResultSet SingleDecimal(std::string value) {
  return slt::ResultSet{.classes = {slt::ColumnClass::kDecimal},
                        .type_names = {"DECIMAL(4,2)"},
                        .rows = {{std::move(value)}}};
}

slt::ExecResult UnsupportedAnswer() {
  return std::unexpected(slt::EngineError{
      .kind = "unsupported", .message = "unsupported: WHERE", .unsupported = true});
}

TEST(Evaluate, ActiveRelationsHoldOrAreViolated) {
  const Relation r{.name = "r",
                   .features = {slt::Feature::kCountStar},
                   .probes = {Probe{.sql = "a"}, Probe{.sql = "b"}, Probe{.sql = "c"}},
                   .check = FirstEqualsSumOfRest()};
  const slt::FeatureSet supported = {slt::Feature::kCountStar};
  EXPECT_EQ(Evaluate(r, {Single("5"), Single("2"), Single("3")}, supported).kind,
            Verdict::Kind::kHolds);
  const Verdict wrong = Evaluate(r, {Single("5"), Single("2"), Single("4")}, supported);
  EXPECT_EQ(wrong.kind, Verdict::Kind::kViolated);
  EXPECT_EQ(wrong.message, "answer 0 is 5, but 2 + 4 = 6");
  EXPECT_EQ(Evaluate(r, {Single("5"), Single("2"), UnsupportedAnswer()}, supported).kind,
            Verdict::Kind::kBroken)
      << "an Unsupported answer to an active relation is a failure";
}

TEST(Evaluate, PendingRelationsNeedAnUnsupportedAnswer) {
  const Relation r{.name = "r",
                   .features = {slt::Feature::kCountStar, slt::Feature::kWhere},
                   .probes = {Probe{.sql = "a"}, Probe{.sql = "b"}},
                   .check = AllEqual()};
  const slt::FeatureSet supported = {slt::Feature::kCountStar};
  const Verdict pending = Evaluate(r, {Single("1"), UnsupportedAnswer()}, supported);
  EXPECT_EQ(pending.kind, Verdict::Kind::kPending);
  EXPECT_NE(pending.message.find("needs where"), std::string::npos) << pending.message;
  const Verdict answered = Evaluate(r, {Single("1"), Single("1")}, supported);
  EXPECT_EQ(answered.kind, Verdict::Kind::kBroken);
  EXPECT_NE(answered.message.find("Add them to kSupportedFeatures"), std::string::npos)
      << answered.message;
  const slt::ExecResult bind =
      std::unexpected(slt::EngineError{.kind = "bind", .message = "bind: no such column"});
  EXPECT_EQ(Evaluate(r, {bind, UnsupportedAnswer()}, supported).kind, Verdict::Kind::kBroken);
  // Once the features are declared, the same answers make the relation active.
  EXPECT_EQ(Evaluate(r, {Single("1"), Single("1")}, r.features).kind, Verdict::Kind::kHolds);
}

TEST(Evaluate, RedactedMessagesHoldNoValuesOrErrorTexts) {
  const Relation r{.name = "r",
                   .features = {slt::Feature::kCountStar},
                   .probes = {Probe{.sql = "a"}, Probe{.sql = "b"}},
                   .check = FirstEqualsSumOfRest()};
  const slt::FeatureSet supported = {slt::Feature::kCountStar};
  const Verdict wrong = Evaluate(r, {Single("5"), Single("4")}, supported);
  ASSERT_EQ(wrong.kind, Verdict::Kind::kViolated);
  EXPECT_NE(wrong.message.find('5'), std::string::npos) << wrong.message;
  EXPECT_EQ(wrong.redacted.find_first_of("0123456789"), std::string::npos) << wrong.redacted;
  const slt::ExecResult bind =
      std::unexpected(slt::EngineError{.kind = "bind", .message = "bind: SECRET_COLUMN"});
  const Verdict broken = Evaluate(r, {Single("5"), bind}, supported);
  ASSERT_EQ(broken.kind, Verdict::Kind::kBroken);
  EXPECT_EQ(broken.redacted, "query 1 fails (bind error)");
  const Verdict pending =
      Evaluate(Relation{.name = "p",
                        .features = {slt::Feature::kCountStar, slt::Feature::kWhere},
                        .probes = {Probe{.sql = "a"}, Probe{.sql = "b"}},
                        .check = AllEqual()},
               {Single("5"), UnsupportedAnswer()}, supported);
  EXPECT_EQ(pending.redacted, pending.message) << "pending messages name only features";
}

TEST(Checks, MinMaxRowCountsAndEquality) {
  EXPECT_FALSE(
      FirstEqualsMinOfRest()(std::vector{Single("2"), Single("5"), Single("2")}).has_value());
  EXPECT_TRUE(
      FirstEqualsMinOfRest()(std::vector{Single("5"), Single("5"), Single("2")}).has_value());
  EXPECT_FALSE(
      FirstEqualsMaxOfRest()(std::vector{Single("-1"), Single("-1"), Single("-10")}).has_value());
  const slt::ResultSet null_value{
      .classes = {slt::ColumnClass::kInteger}, .type_names = {"BIGINT"}, .rows = {{std::nullopt}}};
  EXPECT_FALSE(FirstEqualsMaxOfRest()(std::vector{null_value, null_value}).has_value());
  EXPECT_FALSE(FirstEqualsSumOfRest()(std::vector{Single("18446744073709551614"),
                                                  Single("9223372036854775807"),
                                                  Single("9223372036854775807")})
                   .has_value())
      << "sums are exact beyond 64 bits";
  slt::ResultSet three_rows{.classes = {slt::ColumnClass::kInteger}, .type_names = {"INTEGER"}};
  three_rows.rows = {{"1"}, {"2"}, {"3"}};
  EXPECT_FALSE(RowCountsEqualFirst()(std::vector{Single("3"), three_rows}).has_value());
  EXPECT_FALSE(
      RowCountsAreMinOf({1, 10})(std::vector{Single("3"), Single("7"), three_rows}).has_value());
  EXPECT_TRUE(RowCountsAreMinOf({2})(std::vector{Single("3"), three_rows}).has_value());
  EXPECT_FALSE(AllEqual()(std::vector{three_rows, three_rows}).has_value());
  slt::ResultSet two_rows = three_rows;
  two_rows.rows = {{"3"}, {"1"}};
  EXPECT_FALSE(FirstIsUnionOfRest()(std::vector{three_rows, two_rows, Single("2")}).has_value());
  EXPECT_TRUE(FirstIsUnionOfRest()(std::vector{three_rows, two_rows}).has_value());
  EXPECT_TRUE(FirstIsUnionOfRest()(std::vector{three_rows, three_rows, Single("2")}).has_value());
  EXPECT_TRUE(AllEqual()(std::vector{three_rows, Single("3")}).has_value());
}

TEST(Checks, DecimalMinMaxCompareByValue) {
  // By value, not as text: 10.00 > 9.99, -0.25 < 0.00 and -.500 < .500.
  EXPECT_FALSE(FirstEqualsMaxOfRest()(std::vector{SingleDecimal("10.00"), SingleDecimal("9.99"),
                                                  SingleDecimal("10.00")})
                   .has_value());
  EXPECT_FALSE(FirstEqualsMinOfRest()(std::vector{SingleDecimal("-0.25"), SingleDecimal("0.00"),
                                                  SingleDecimal("-0.25")})
                   .has_value());
  EXPECT_FALSE(FirstEqualsMinOfRest()(std::vector{SingleDecimal("-.500"), SingleDecimal(".500"),
                                                  SingleDecimal("-.500")})
                   .has_value());
  EXPECT_TRUE(FirstEqualsMaxOfRest()(
                  std::vector{SingleDecimal("9.99"), SingleDecimal("9.99"), SingleDecimal("10.00")})
                  .has_value());
  // Values of one DECIMAL type have one scale; texts at two scales do not compare.
  EXPECT_EQ(FirstEqualsMaxOfRest()(
                std::vector{SingleDecimal("1.50"), SingleDecimal("1.50"), SingleDecimal("1.5")})
                .value_or(""),
            "answer 2 (1.5) does not compare with 1.50");
}

TEST(RelationList, IsWellFormed) {
  const auto relations = AllRelations();
  std::set<std::string> names;
  int active = 0;
  for (const auto& r : relations) {
    EXPECT_TRUE(names.insert(r.name).second) << "duplicate relation " << r.name;
    for (const char c : r.name) {
      EXPECT_TRUE(std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_') << r.name;
    }
    EXPECT_GE(r.probes.size(), 2U) << r.name;
    EXPECT_FALSE(r.features.empty()) << r.name;
    EXPECT_TRUE(static_cast<bool>(r.check)) << r.name;
    active += slt::kSupportedFeatures.Contains(r.features) ? 1 : 0;
  }
  EXPECT_GT(active, 0) << "no relation is active with the declared features";
}

TEST(RelationList, TablePathsMatchTablesTxt) {
  const auto tables = LoadFixtureTables();
  ASSERT_EQ(tables.size(), TablePaths().size());
  for (std::size_t i = 0; i < tables.size(); ++i) {
    EXPECT_EQ(tables[i].name, TablePaths()[i].table);
    ASSERT_EQ(tables[i].patterns.size(), 1U) << tables[i].name;
    EXPECT_EQ(tables[i].patterns[0],
              (antb1::testing::FixturesDir() / TablePaths()[i].path).lexically_normal().string());
  }
}

// RowCount (COUNT(*) from the Parquet footers) vs the rows an independent reader (the Parquet
// library, not antb1) decodes from every file of the table.
TEST(RowCount, MatchesAnIndependentParquetScan) {
  Engines engines;
  for (const auto& t : LoadFixtureTables()) {
    int64_t scanned = 0;
    for (const auto& file : t.files) {
      auto input = arrow::io::ReadableFile::Open(file);
      ASSERT_TRUE(input.ok()) << input.status().ToString();
      auto reader = parquet::arrow::OpenFile(*input, arrow::default_memory_pool());
      ASSERT_TRUE(reader.ok()) << reader.status().ToString();
      auto table = (*reader)->ReadTable();
      ASSERT_TRUE(table.ok()) << table.status().ToString();
      scanned += (*table)->num_rows();
    }
    const auto answer = engines.For(kDefaultBatchSize).Execute("SELECT COUNT(*) FROM " + t.name);
    ASSERT_TRUE(answer.has_value()) << answer.error().message;
    ASSERT_EQ(answer->rows.size(), 1U);
    EXPECT_EQ(answer->rows[0][0], std::to_string(scanned)) << t.name;
  }
}

// The Arrow schema of the first file of a table, as the Parquet library (not antb1) reads it.
std::shared_ptr<arrow::Schema> FirstFileSchema(const slt::TableDef& t) {
  if (t.files.empty()) {
    ADD_FAILURE() << t.name << " has no files";
    return nullptr;
  }
  auto input = arrow::io::ReadableFile::Open(t.files.front());
  if (!input.ok()) {
    ADD_FAILURE() << input.status().ToString();
    return nullptr;
  }
  auto reader = parquet::arrow::OpenFile(*input, arrow::default_memory_pool());
  if (!reader.ok()) {
    ADD_FAILURE() << reader.status().ToString();
    return nullptr;
  }
  std::shared_ptr<arrow::Schema> schema;
  const arrow::Status status = (*reader)->GetSchema(&schema);
  EXPECT_TRUE(status.ok()) << status.ToString();
  return schema;
}

// The kind of join key a column type gives: integers of any width join each other, as DECIMALs of
// any precision and scale do; std::nullopt for a type that never keys a join (DOUBLE).
enum class KeyKind : std::uint8_t { kInteger, kDecimal, kDate, kVarchar };

std::optional<KeyKind> KeyKindOf(const arrow::DataType& type) {
  const auto logical = plan::FromArrow(type);
  if (!logical.ok()) {
    return std::nullopt;
  }
  if (plan::IsInteger(*logical)) {
    return KeyKind::kInteger;
  }
  switch (logical->id()) {
    case plan::LogicalType::kDecimal:
      return KeyKind::kDecimal;
    case plan::LogicalType::kDate:
      return KeyKind::kDate;
    case plan::LogicalType::kVarchar:
      return KeyKind::kVarchar;
    default:
      return std::nullopt;
  }
}

// The ref= options of tests/slt/tables.txt (runner/tables.h), which LoadTables does not check
// against the files: every column exists, and each pair of columns joins one kind of key.
TEST(TablesTxt, RefsJoinColumnsOfOneKind) {
  const auto tables = LoadFixtureTables();
  int refs = 0;
  for (const auto& t : tables) {
    for (const auto& ref : t.refs) {
      ++refs;
      const auto target = std::ranges::find(tables, ref.table, &slt::TableDef::name);
      ASSERT_NE(target, tables.end()) << t.name << ": " << ref.table;
      const auto from = FirstFileSchema(t);
      const auto to = FirstFileSchema(*target);
      ASSERT_NE(from, nullptr);
      ASSERT_NE(to, nullptr);
      ASSERT_EQ(ref.columns.size(), ref.ref_columns.size());
      for (std::size_t c = 0; c < ref.columns.size(); ++c) {
        const std::string what =
            std::format("{}.{} -> {}.{}", t.name, ref.columns[c], ref.table, ref.ref_columns[c]);
        const auto column = from->GetFieldByName(ref.columns[c]);
        const auto ref_column = to->GetFieldByName(ref.ref_columns[c]);
        ASSERT_NE(column, nullptr) << what << ": no such column";
        ASSERT_NE(ref_column, nullptr) << what << ": no such referenced column";
        const auto kind = KeyKindOf(*column->type());
        EXPECT_TRUE(kind.has_value()) << what << ": " << column->type()->ToString();
        EXPECT_EQ(kind, KeyKindOf(*ref_column->type()))
            << what << ": " << column->type()->ToString() << " against "
            << ref_column->type()->ToString();
      }
    }
  }
  EXPECT_EQ(refs, 13) << "the references of the star schema (tools/fixturegen/star.h)";
}

}  // namespace
}  // namespace antb1::metamorphic
