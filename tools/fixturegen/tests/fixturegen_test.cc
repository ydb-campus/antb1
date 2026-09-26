#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <arrow/api.h>
#include <gtest/gtest.h>
#include <parquet/file_reader.h>
#include <parquet/metadata.h>

#include "fixtures.h"
#include "hits_schema.h"

namespace antb1::fixturegen {
namespace {

namespace fs = std::filesystem;

std::shared_ptr<arrow::Table> Hits(HitsVariant variant, Nulls nulls, int64_t first, int64_t n) {
  auto table = MakeHitsTable(variant, nulls, first, n);
  EXPECT_TRUE(table.ok()) << table.status().ToString();
  return table.ValueOrDie();
}

std::string ReadBytes(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}

TEST(SplitMix64, MatchesTheReferenceSequence) {
  SplitMix64 rng(0);
  EXPECT_EQ(rng.Next(), 0xE220A8397B1DCDAFULL);
  EXPECT_EQ(rng.Next(), 0x6E789E6AA1B965F4ULL);
  EXPECT_EQ(rng.Next(), 0x06C45D188009454FULL);
}

TEST(HitsSchema, Has105UniqueColumns) {
  const auto columns = HitsColumns();
  ASSERT_EQ(columns.size(), kHitsColumnCount);
  std::set<std::string_view> names;
  for (const auto& c : columns) {
    names.insert(c.name);
  }
  EXPECT_EQ(names.size(), kHitsColumnCount);
  EXPECT_EQ(columns[0].name, "WatchID");
  EXPECT_EQ(columns[5].name, "EventDate");
  EXPECT_EQ(columns[5].type, HitsType::kUInt16);
  EXPECT_EQ(columns[104].name, "CLID");
}

TEST(HitsSchema, VariantsDifferInNullabilityAndStringType) {
  const auto partitioned = HitsArrowSchema(HitsVariant::kPartitioned);
  const auto single = HitsArrowSchema(HitsVariant::kSingleFile);
  EXPECT_TRUE(partitioned->GetFieldByName("Title")->type()->Equals(arrow::binary()));
  EXPECT_TRUE(single->GetFieldByName("Title")->type()->Equals(arrow::utf8()));
  for (const auto& f : partitioned->fields()) {
    EXPECT_TRUE(f->nullable()) << f->name();
  }
  for (const auto& f : single->fields()) {
    EXPECT_FALSE(f->nullable()) << f->name();
  }
}

TEST(HitsTable, IsDeterministicAndIndependentOfSlicing) {
  const auto full = Hits(HitsVariant::kPartitioned, Nulls::kNone, 0, kHitsRows);
  const auto again = Hits(HitsVariant::kPartitioned, Nulls::kNone, 0, kHitsRows);
  EXPECT_TRUE(full->Equals(*again));
  const auto middle = Hits(HitsVariant::kPartitioned, Nulls::kNone, 1000, 3000);
  EXPECT_TRUE(middle->Equals(*full->Slice(1000, 3000)));
}

TEST(HitsTable, RequiredVariantHasTheSameValues) {
  const auto partitioned = Hits(HitsVariant::kPartitioned, Nulls::kNone, 0, 500);
  const auto single = Hits(HitsVariant::kSingleFile, Nulls::kNone, 0, 500);
  for (int c = 0; c < partitioned->num_columns(); ++c) {
    const auto a = partitioned->column(c)->chunk(0);
    auto b = single->column(c)->chunk(0);
    if (b->type_id() == arrow::Type::STRING) {
      b = b->View(arrow::binary()).ValueOrDie();
    }
    EXPECT_TRUE(a->Equals(*b)) << partitioned->field(c)->name();
    EXPECT_EQ(a->null_count(), 0);
  }
}

TEST(HitsTable, NullsVariantMasksTheSameValues) {
  const auto base = Hits(HitsVariant::kPartitioned, Nulls::kNone, 0, kHitsRows);
  const auto nulls = Hits(HitsVariant::kPartitioned, Nulls::kSprinkled, 0, kHitsRows);
  for (int c = 0; c < base->num_columns(); ++c) {
    const auto a = base->column(c)->chunk(0);
    const auto b = nulls->column(c)->chunk(0);
    const std::string& name = base->field(c)->name();
    EXPECT_TRUE(b->IsNull(0)) << name;
    if (name == "SocialAction" || name == "HistoryLength") {
      EXPECT_EQ(b->null_count(), kHitsRows) << name;
    } else if (name == "CounterID") {
      EXPECT_EQ(b->null_count(), 1);
    } else {
      EXPECT_GT(b->null_count(), kHitsRows / 20) << name;
      EXPECT_LT(b->null_count(), kHitsRows / 8) << name;
    }
    for (int64_t r = 0; r < kHitsRows; ++r) {
      if (b->IsValid(r)) {
        ASSERT_TRUE(a->RangeEquals(*b, r, r + 1, r)) << name << " row " << r;
      }
    }
  }
}

TEST(HitsTable, UserIdSumOverflowsInt64AndEventDateIsJuly2013) {
  const auto hits = Hits(HitsVariant::kPartitioned, Nulls::kNone, 0, kHitsRows);
  const auto& user_id =
      static_cast<const arrow::Int64Array&>(*hits->GetColumnByName("UserID")->chunk(0));
  int64_t sum = 0;
  bool overflow = false;
  int maxima = 0;
  int minima = 0;
  for (int64_t r = 0; r < user_id.length(); ++r) {
    overflow = __builtin_add_overflow(sum, user_id.Value(r), &sum) || overflow;
    maxima += user_id.Value(r) == std::numeric_limits<int64_t>::max() ? 1 : 0;
    minima += user_id.Value(r) == std::numeric_limits<int64_t>::min() ? 1 : 0;
  }
  EXPECT_TRUE(overflow);
  EXPECT_EQ(maxima, 2);
  EXPECT_EQ(minima, 1);

  const auto& date =
      static_cast<const arrow::UInt16Array&>(*hits->GetColumnByName("EventDate")->chunk(0));
  const auto* begin = date.raw_values();
  const auto [lo, hi] = std::minmax_element(begin, begin + date.length());
  EXPECT_EQ(*lo, 15'887);  // 2013-07-01
  EXPECT_EQ(*hi, 15'917);  // 2013-07-31
}

TEST(EdgeTable, HasExtremesNullsAndNoNaN) {
  auto edge = MakeEdgeTable().ValueOrDie();
  ASSERT_EQ(edge->num_rows(), 12);
  ASSERT_EQ(edge->num_columns(), 8);
  const auto& i64 = static_cast<const arrow::Int64Array&>(*edge->GetColumnByName("i64")->chunk(0));
  EXPECT_EQ(i64.Value(0), std::numeric_limits<int64_t>::min());
  EXPECT_EQ(i64.Value(1), std::numeric_limits<int64_t>::max());
  const auto& d = static_cast<const arrow::DoubleArray&>(*edge->GetColumnByName("d")->chunk(0));
  for (int64_t r = 0; r < d.length(); ++r) {
    if (d.IsValid(r)) {
      EXPECT_FALSE(std::isnan(d.Value(r))) << "row " << r;
    }
  }
  EXPECT_DOUBLE_EQ(d.Value(1), 1.0 / 3.0);
  for (int c = 1; c < edge->num_columns(); ++c) {
    EXPECT_TRUE(edge->column(c)->chunk(0)->IsNull(5)) << edge->field(c)->name();
  }
}

class FixtureFilesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // ctest runs every test case in its own process, concurrently, with the build tree of the
    // preset as working directory: one directory per test case and preset (not a shared /tmp path).
    dir_ = fs::current_path() / "fixturegen_test_tmp" /
           ::testing::UnitTest::GetInstance()->current_test_info()->name();
    fs::remove_all(dir_);
    auto written = WriteAllFixtures(dir_ / "a");
    ASSERT_TRUE(written.ok()) << written.status().ToString();
    files_ = *written;
  }
  void TearDown() override { fs::remove_all(dir_); }

  fs::path dir_;
  std::vector<FixtureFile> files_;
};

TEST_F(FixtureFilesTest, AreByteIdenticalAcrossRuns) {
  ASSERT_TRUE(WriteAllFixtures(dir_ / "b").ok());
  ASSERT_EQ(files_.size(), 10U);
  for (const auto& f : files_) {
    const std::string a = ReadBytes(dir_ / "a" / f.path);
    ASSERT_FALSE(a.empty()) << f.path;
    EXPECT_EQ(a, ReadBytes(dir_ / "b" / f.path)) << f.path;
  }
}

TEST_F(FixtureFilesTest, HaveTheDeclaredLayout) {
  int64_t split_rows = 0;
  for (const auto& f : files_) {
    auto metadata =
        parquet::ParquetFileReader::OpenFile((dir_ / "a" / f.path).string())->metadata();
    EXPECT_EQ(metadata->num_rows(), f.rows) << f.path;
    EXPECT_EQ(metadata->num_row_groups(), f.row_groups) << f.path;
    const auto kv = metadata->key_value_metadata();
    EXPECT_TRUE(kv == nullptr || kv->FindKey("ARROW:schema") < 0) << f.path;
    for (int g = 0; g < metadata->num_row_groups(); ++g) {
      EXPECT_EQ(metadata->RowGroup(g)->ColumnChunk(0)->compression(), parquet::Compression::SNAPPY)
          << f.path;
    }
    if (f.path.starts_with("hits_like_split/")) {
      split_rows += f.rows;
    }
  }
  EXPECT_EQ(split_rows, kHitsRows);
  const auto hits = std::ranges::find(files_, std::string("hits_like.parquet"), &FixtureFile::path);
  ASSERT_NE(hits, files_.end());
  EXPECT_EQ(hits->rows, kHitsRows);
  EXPECT_EQ(hits->row_groups, 4);
}

TEST_F(FixtureFilesTest, CheckSchemaAcceptsPartitionedLayoutOnly) {
  for (const auto* name : {"hits_like.parquet", "hits_like_nulls.parquet", "empty.parquet",
                           "hits_like_split/part-3.parquet"}) {
    auto diffs = CheckHitsSchema((dir_ / "a" / name).string());
    ASSERT_TRUE(diffs.ok()) << diffs.status().ToString();
    EXPECT_TRUE(diffs->empty()) << name << ": " << diffs->front();
  }
  auto required = CheckHitsSchema((dir_ / "a" / "hits_like_required.parquet").string());
  ASSERT_TRUE(required.ok());
  // 105 repetition differences plus logical and converted type differences for 28 strings.
  EXPECT_EQ(required->size(), 105U + (2U * 28U));
  auto edge = CheckHitsSchema((dir_ / "a" / "edge.parquet").string());
  ASSERT_TRUE(edge.ok());
  EXPECT_EQ(edge->front(), "column count: expected 105, got 8");
  EXPECT_TRUE(CheckHitsSchema((dir_ / "missing.parquet").string()).status().IsIOError());
}

}  // namespace
}  // namespace antb1::fixturegen
