#include "antb1/io/parquet_table.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <gtest/gtest.h>
#include <parquet/arrow/writer.h>

namespace antb1::io {
namespace {

namespace fs = std::filesystem;

class ParquetTableTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = fs::path(::testing::TempDir()) / "antb1_io" / info->name();
    fs::remove_all(dir_);
    fs::create_directories(dir_);
  }
  void TearDown() override { fs::remove_all(dir_); }

  std::string Write(const std::string& name, int16_t rows, bool with_extra = false) {
    arrow::Int16Builder a;
    arrow::UInt16Builder d;
    for (int16_t i = 0; i < rows; ++i) {
      EXPECT_TRUE(a.Append(i).ok());
      EXPECT_TRUE(d.Append(static_cast<uint16_t>(19000 + i)).ok());
    }
    arrow::FieldVector fields{arrow::field("AdvEngineID", arrow::int16()),
                              arrow::field("EventDate", arrow::uint16())};
    std::vector<std::shared_ptr<arrow::Array>> columns{a.Finish().ValueOrDie(),
                                                       d.Finish().ValueOrDie()};
    if (with_extra) {
      arrow::StringBuilder s;
      for (int16_t i = 0; i < rows; ++i) {
        EXPECT_TRUE(s.Append("x").ok());
      }
      fields.push_back(arrow::field("Title", arrow::utf8()));
      columns.push_back(s.Finish().ValueOrDie());
    }
    auto table = arrow::Table::Make(arrow::schema(fields), columns);
    const std::string path = (dir_ / name).string();
    auto out = arrow::io::FileOutputStream::Open(path).ValueOrDie();
    EXPECT_TRUE(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), out, 3).ok());
    EXPECT_TRUE(out->Close().ok());
    return path;
  }

  fs::path dir_;
};

TEST_F(ParquetTableTest, OpensSingleFileWithRowCount) {
  auto table = ParquetTable::Open({Write("a.parquet", 7)});
  ASSERT_TRUE(table.ok()) << table.status().ToString();
  EXPECT_EQ((*table)->exact_row_count(), 7);
  EXPECT_EQ((*table)->schema()->num_fields(), 2);
  EXPECT_EQ((*table)->schema()->field(1)->type()->id(), arrow::Type::UINT16);
}

TEST_F(ParquetTableTest, GlobSumsRowsInSortedOrder) {
  Write("b.parquet", 2);
  Write("a.parquet", 5);
  auto table = ParquetTable::Open({(dir_ / "*.parquet").string()});
  ASSERT_TRUE(table.ok()) << table.status().ToString();
  EXPECT_EQ((*table)->exact_row_count(), 7);
  ASSERT_EQ((*table)->files().size(), 2U);
  EXPECT_LT((*table)->files()[0], (*table)->files()[1]);
  // data_size is the sum of the file sizes (ClickBench's data_size in `antb1 bench`).
  const auto bytes = fs::file_size((*table)->files()[0]) + fs::file_size((*table)->files()[1]);
  EXPECT_EQ((*table)->total_bytes(), static_cast<int64_t>(bytes));
  EXPECT_EQ((*table)->data_size(), (*table)->total_bytes());
}

TEST_F(ParquetTableTest, DateOverrideChangesEngineType) {
  auto table = ParquetTable::Open({Write("a.parquet", 3)},
                                  ParquetTableOptions{.overrides = {{.column = "eventdate"}}});
  ASSERT_TRUE(table.ok()) << table.status().ToString();
  EXPECT_EQ((*table)->schema()->field(1)->type()->id(), arrow::Type::DATE32);
}

TEST_F(ParquetTableTest, InvalidOverrideIsRejected) {
  const auto path = Write("a.parquet", 3, /*with_extra=*/true);
  EXPECT_TRUE(ParquetTable::Open({path}, ParquetTableOptions{.overrides = {{.column = "Title"}}})
                  .status()
                  .IsInvalid());
  EXPECT_TRUE(ParquetTable::Open({path}, ParquetTableOptions{.overrides = {{.column = "nope"}}})
                  .status()
                  .IsInvalid());
}

// An override applies to every column its name matches case-insensitively, so each of them must be
// readable as the new type (not only the first).
TEST_F(ParquetTableTest, OverrideIsCheckedAgainstEveryMatchingColumn) {
  arrow::UInt16Builder days;
  arrow::StringBuilder text;
  ASSERT_TRUE(days.Append(15'901).ok());
  ASSERT_TRUE(text.Append("x").ok());
  const auto table = arrow::Table::Make(
      arrow::schema({arrow::field("d", arrow::uint16()), arrow::field("D", arrow::utf8())}),
      {days.Finish().ValueOrDie(), text.Finish().ValueOrDie()});
  const std::string path = (dir_ / "cases.parquet").string();
  auto out = arrow::io::FileOutputStream::Open(path).ValueOrDie();
  ASSERT_TRUE(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), out, 1).ok());
  ASSERT_TRUE(out->Close().ok());

  const auto opened =
      ParquetTable::Open({path}, ParquetTableOptions{.overrides = {{.column = "d"}}});
  ASSERT_TRUE(opened.status().IsInvalid()) << opened.status().ToString();
  EXPECT_NE(opened.status().message().find("'D'"), std::string::npos) << opened.status().message();
}

TEST_F(ParquetTableTest, SchemaMismatchIsIOError) {
  const auto a = Write("a.parquet", 3);
  const auto b = Write("b.parquet", 3, /*with_extra=*/true);
  EXPECT_TRUE(ParquetTable::Open({a, b}).status().IsIOError());
}

TEST_F(ParquetTableTest, MissingAndCorruptFilesAreIOErrors) {
  EXPECT_TRUE(ParquetTable::Open({(dir_ / "missing.parquet").string()}).status().IsIOError());
  EXPECT_TRUE(ParquetTable::Open({(dir_ / "*.nomatch").string()}).status().IsIOError());
  const auto bad = (dir_ / "bad.parquet").string();
  std::ofstream(bad) << "this is not a parquet file";
  EXPECT_TRUE(ParquetTable::Open({bad}).status().IsIOError());
  EXPECT_TRUE(ParquetTable::Open({}).status().IsInvalid());
}

}  // namespace
}  // namespace antb1::io
