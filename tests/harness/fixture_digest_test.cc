// The fixture digest is logical: equal for the same data written with other compression, encodings
// or page sizes, different for any change of a value, a type, a NULL or the row groups.

#include "fixture_digest.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <gtest/gtest.h>
#include <parquet/arrow/writer.h>
#include <parquet/properties.h>

namespace antb1::harness {
namespace {

namespace fs = std::filesystem;

struct TableOptions {
  int64_t changed_row = -1;     // this row gets another integer
  bool null_row_3 = true;       // the double of row 3 is NULL
  bool binary_strings = false;  // unannotated BYTE_ARRAY instead of UTF8
};

std::shared_ptr<arrow::Table> MakeTable(const TableOptions& o = {}) {
  arrow::Int64Builder ints;
  arrow::DoubleBuilder doubles;
  arrow::StringBuilder strings;
  for (int64_t i = 0; i < 100; ++i) {
    EXPECT_TRUE(ints.Append(i == o.changed_row ? -i : i * 7).ok());
    EXPECT_TRUE(
        (i == 3 && o.null_row_3 ? doubles.AppendNull() : doubles.Append(static_cast<double>(i) / 3))
            .ok());
    EXPECT_TRUE(strings.Append(std::string(static_cast<std::size_t>(i % 5), 'x')).ok());
  }
  std::shared_ptr<arrow::Array> s = strings.Finish().ValueOrDie();
  if (o.binary_strings) {
    s = s->View(arrow::binary()).ValueOrDie();
  }
  auto schema = arrow::schema({arrow::field("i", arrow::int64()),
                               arrow::field("d", arrow::float64()), arrow::field("s", s->type())});
  return arrow::Table::Make(schema, {ints.Finish().ValueOrDie(), doubles.Finish().ValueOrDie(), s});
}

class FixtureDigest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::path(::testing::TempDir()) / "antb1_digest" /
           ::testing::UnitTest::GetInstance()->current_test_info()->name();
    std::error_code ec;
    fs::remove_all(dir_, ec);
    fs::create_directories(dir_);
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  FileDigest Write(const std::string& name, const arrow::Table& table, int64_t row_group_rows,
                   const std::shared_ptr<parquet::WriterProperties>& properties) {
    const fs::path path = dir_ / name;
    auto out = arrow::io::FileOutputStream::Open(path.string()).ValueOrDie();
    EXPECT_TRUE(parquet::arrow::WriteTable(table, arrow::default_memory_pool(), out, row_group_rows,
                                           properties)
                    .ok());
    EXPECT_TRUE(out->Close().ok());
    auto digest = DigestFile(path, name);
    EXPECT_TRUE(digest.ok()) << digest.status().ToString();
    return digest.ValueOr(FileDigest{});
  }

  static std::shared_ptr<parquet::WriterProperties> Snappy() {
    return parquet::WriterProperties::Builder().compression(parquet::Compression::SNAPPY)->build();
  }

  fs::path dir_;
};

TEST_F(FixtureDigest, IgnoresCompressionEncodingAndPages) {
  const auto table = MakeTable();
  const FileDigest a = Write("a.parquet", *table, 40, Snappy());
  const auto plain = parquet::WriterProperties::Builder()
                         .compression(parquet::Compression::UNCOMPRESSED)
                         ->disable_dictionary()
                         ->data_pagesize(64)
                         ->build();
  const FileDigest b = Write("b.parquet", *table, 40, plain);
  EXPECT_EQ(a.rows, 100);
  EXPECT_EQ(a.row_groups, (std::vector<int64_t>{40, 40, 20}));
  EXPECT_EQ(a.schema_sha256, b.schema_sha256);
  EXPECT_EQ(a.data_sha256, b.data_sha256);
  EXPECT_EQ(a.Line().substr(a.Line().find(' ')), b.Line().substr(b.Line().find(' ')));
}

TEST_F(FixtureDigest, SeesValuesNullsTypesAndRowGroups) {
  const FileDigest base = Write("base.parquet", *MakeTable(), 40, Snappy());
  const FileDigest value = Write("value.parquet", *MakeTable({.changed_row = 50}), 40, Snappy());
  const FileDigest nulls = Write("nulls.parquet", *MakeTable({.null_row_3 = false}), 40, Snappy());
  const FileDigest groups = Write("groups.parquet", *MakeTable(), 50, Snappy());
  const FileDigest types =
      Write("types.parquet", *MakeTable({.binary_strings = true}), 40, Snappy());
  EXPECT_EQ(base.data_sha256.size(), 64U);
  EXPECT_NE(base.data_sha256, value.data_sha256);
  EXPECT_NE(base.data_sha256, nulls.data_sha256);
  EXPECT_NE(base.row_groups, groups.row_groups);
  EXPECT_NE(base.schema_sha256, types.schema_sha256) << "UTF8 vs unannotated BYTE_ARRAY";
  EXPECT_EQ(base.data_sha256, types.data_sha256) << "the same bytes";
}

// A decimal's digest is its unscaled value, whatever the physical storage (INT32 or FLBA); a change
// in either 64-bit half is seen.
TEST_F(FixtureDigest, HashesDecimalsByValue) {
  const auto make = [](const arrow::Decimal128& last) {
    arrow::Decimal128Builder builder(arrow::decimal128(9, 2));
    EXPECT_TRUE(builder.Append(arrow::Decimal128(-12345)).ok());
    EXPECT_TRUE(builder.AppendNull().ok());
    EXPECT_TRUE(builder.Append(last).ok());
    auto schema = arrow::schema({arrow::field("d", arrow::decimal128(9, 2))});
    return arrow::Table::Make(schema, {builder.Finish().ValueOrDie()});
  };
  const auto as_integer = parquet::WriterProperties::Builder()
                              .enable_store_decimal_as_integer()
                              ->compression(parquet::Compression::SNAPPY)
                              ->build();
  const FileDigest flba = Write("flba.parquet", *make(arrow::Decimal128(999)), 10, Snappy());
  const FileDigest int32 = Write("int32.parquet", *make(arrow::Decimal128(999)), 10, as_integer);
  const FileDigest low = Write("low.parquet", *make(arrow::Decimal128(998)), 10, Snappy());
  const FileDigest negative = Write("neg.parquet", *make(arrow::Decimal128(-999)), 10, Snappy());
  EXPECT_NE(flba.schema_sha256, int32.schema_sha256) << "FIXED_LEN_BYTE_ARRAY vs INT32";
  EXPECT_EQ(flba.data_sha256, int32.data_sha256);
  EXPECT_NE(flba.data_sha256, low.data_sha256);
  EXPECT_NE(flba.data_sha256, negative.data_sha256);
}

TEST_F(FixtureDigest, ComparesDigestTexts) {
  const FileDigest a = Write("a.parquet", *MakeTable(), 40, Snappy());
  FileDigest changed = a;
  changed.data_sha256 = std::string(64, '0');
  const std::string text = DigestText({a});
  EXPECT_TRUE(CompareDigests(text, {a}).empty());
  EXPECT_EQ(CompareDigests(text, {changed}).size(), 1U);
  EXPECT_EQ(CompareDigests(text, {}).size(), 1U);  // missing
  FileDigest extra = a;
  extra.path = "extra.parquet";
  EXPECT_EQ(CompareDigests(text, {a, extra}).size(), 1U);  // unexpected
}

TEST_F(FixtureDigest, DirectoryIsSortedAndRecursive) {
  fs::create_directories(dir_ / "sub");
  Write("sub/z.parquet", *MakeTable(), 100, Snappy());
  Write("b.parquet", *MakeTable(), 100, Snappy());
  auto digests = DigestDirectory(dir_);
  ASSERT_TRUE(digests.ok()) << digests.status().ToString();
  ASSERT_EQ(digests->size(), 2U);
  EXPECT_EQ((*digests)[0].path, "b.parquet");
  EXPECT_EQ((*digests)[1].path, "sub/z.parquet");
  EXPECT_FALSE(DigestDirectory(dir_ / "sub" / "none").ok());
}

// The top-level tpch/ belongs to fixtures.tpch: it is skipped before it is opened (an unreadable
// one is no error), while a tpch/ deeper down is digested like any other directory.
TEST_F(FixtureDigest, SkipsTheTopLevelTpchDirectoryOnly) {
  fs::create_directories(dir_ / "tpch" / "deep");
  fs::create_directories(dir_ / "sub" / "tpch");
  Write("a.parquet", *MakeTable(), 100, Snappy());
  Write("tpch/b.parquet", *MakeTable(), 100, Snappy());
  Write("tpch/deep/c.parquet", *MakeTable(), 100, Snappy());
  Write("sub/tpch/d.parquet", *MakeTable(), 100, Snappy());
  fs::permissions(dir_ / "tpch", fs::perms::none);
  auto digests = DigestDirectory(dir_);
  fs::permissions(dir_ / "tpch", fs::perms::owner_all);
  ASSERT_TRUE(digests.ok()) << digests.status().ToString();
  ASSERT_EQ(digests->size(), 2U);
  EXPECT_EQ((*digests)[0].path, "a.parquet");
  EXPECT_EQ((*digests)[1].path, "sub/tpch/d.parquet");
}

}  // namespace
}  // namespace antb1::harness
