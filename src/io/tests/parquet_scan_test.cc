// ParquetTable::Scan: batches of the engine view (plan::Table::schema()) over every file, row
// group and batch in order.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <gtest/gtest.h>
#include <parquet/arrow/writer.h>
#include <parquet/properties.h>

#include "antb1/io/parquet_table.h"
#include "antb1/plan/types.h"

namespace antb1::io {
namespace {

namespace fs = std::filesystem;

template <class Builder, class T>
std::shared_ptr<arrow::Array> Build(const std::vector<std::optional<T>>& values) {
  Builder builder;
  for (const auto& v : values) {
    const arrow::Status st = v.has_value() ? builder.Append(*v) : builder.AppendNull();
    EXPECT_TRUE(st.ok()) << st.ToString();
  }
  std::shared_ptr<arrow::Array> out;
  EXPECT_TRUE(builder.Finish(&out).ok());
  return out;
}

template <class Builder, class T>
std::shared_ptr<arrow::Array> Build(const std::vector<T>& values) {
  std::vector<std::optional<T>> optional(values.begin(), values.end());
  return Build<Builder, T>(optional);
}

std::vector<int64_t> Sequence(int64_t first, int64_t count) {
  std::vector<int64_t> out(static_cast<std::size_t>(count));
  std::ranges::iota(out, first);
  return out;
}

// Everything a scan returned, and how.
struct Scanned {
  std::shared_ptr<arrow::Schema> schema;
  std::vector<int64_t> batch_rows;
  std::shared_ptr<arrow::Table> table;  // the batches, chunks combined
};

class ParquetScanTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = fs::path(::testing::TempDir()) / "antb1_io_scan" / info->name();
    fs::remove_all(dir_);
    fs::create_directories(dir_);
  }
  void TearDown() override { fs::remove_all(dir_); }

  std::string Write(const std::string& name, const std::shared_ptr<arrow::Table>& table,
                    int64_t row_group_rows, bool store_schema = false) {
    const std::string path = (dir_ / name).string();
    auto out = arrow::io::FileOutputStream::Open(path);
    EXPECT_TRUE(out.ok()) << out.status().ToString();
    parquet::ArrowWriterProperties::Builder arrow_props;
    if (store_schema) {
      arrow_props.store_schema();
    }
    const arrow::Status st =
        parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), *out, row_group_rows,
                                   parquet::default_writer_properties(), arrow_props.build());
    EXPECT_TRUE(st.ok()) << st.ToString();
    EXPECT_TRUE((*out)->Close().ok());
    return path;
  }

  // Rows [first, first + count) of a two-column table: a INT64 = row, b INT32 = -row (NULL every
  // fifth row).
  std::string WriteNumbers(const std::string& name, int64_t first, int64_t count,
                           int64_t row_group_rows) {
    std::vector<std::optional<int32_t>> b;
    for (const int64_t v : Sequence(first, count)) {
      b.push_back(v % 5 == 0 ? std::nullopt : std::optional(static_cast<int32_t>(-v)));
    }
    auto table = arrow::Table::Make(
        arrow::schema({arrow::field("a", arrow::int64()), arrow::field("b", arrow::int32())}),
        {Build<arrow::Int64Builder, int64_t>(Sequence(first, count)),
         Build<arrow::Int32Builder, int32_t>(b)});
    return Write(name, table, row_group_rows);
  }

  static Scanned Scan(const plan::Table& table, const std::vector<int>& fields,
                      int64_t batch_size) {
    Scanned out;
    auto reader = table.Scan(fields, batch_size);
    EXPECT_TRUE(reader.ok()) << reader.status().ToString();
    if (!reader.ok()) {
      return out;
    }
    out.schema = (*reader)->schema();
    std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
    while (true) {
      std::shared_ptr<arrow::RecordBatch> batch;
      const arrow::Status st = (*reader)->ReadNext(&batch);
      EXPECT_TRUE(st.ok()) << st.ToString();
      if (!st.ok() || batch == nullptr) {
        break;
      }
      EXPECT_TRUE(batch->schema()->Equals(*out.schema));
      EXPECT_TRUE(batch->ValidateFull().ok());
      out.batch_rows.push_back(batch->num_rows());
      batches.push_back(std::move(batch));
    }
    auto combined = arrow::Table::FromRecordBatches(out.schema, batches);
    EXPECT_TRUE(combined.ok()) << combined.status().ToString();
    if (combined.ok()) {
      auto chunks = (*combined)->CombineChunks();
      EXPECT_TRUE(chunks.ok());
      out.table = chunks.ok() ? *chunks : nullptr;
    }
    return out;
  }

  fs::path dir_;
};

std::vector<std::optional<int64_t>> Int64s(const arrow::Table& table, int column) {
  std::vector<std::optional<int64_t>> out;
  if (table.num_rows() == 0) {
    return out;
  }
  const auto& chunk = *table.column(column)->chunk(0);
  for (int64_t i = 0; i < chunk.length(); ++i) {
    if (chunk.IsNull(i)) {
      out.emplace_back(std::nullopt);
      continue;
    }
    switch (chunk.type_id()) {
      case arrow::Type::INT64:
        out.emplace_back(static_cast<const arrow::Int64Array&>(chunk).Value(i));
        break;
      case arrow::Type::INT32:
        out.emplace_back(static_cast<const arrow::Int32Array&>(chunk).Value(i));
        break;
      case arrow::Type::DATE32:
        out.emplace_back(static_cast<const arrow::Date32Array&>(chunk).Value(i));
        break;
      case arrow::Type::INT16:
        out.emplace_back(static_cast<const arrow::Int16Array&>(chunk).Value(i));
        break;
      case arrow::Type::UINT16:
        out.emplace_back(static_cast<const arrow::UInt16Array&>(chunk).Value(i));
        break;
      default:
        ADD_FAILURE() << "not an integer column: " << chunk.type()->ToString();
        return out;
    }
  }
  return out;
}

std::vector<std::optional<std::string>> Bytes(const arrow::Table& table, int column) {
  std::vector<std::optional<std::string>> out;
  const auto& chunk = static_cast<const arrow::BinaryArray&>(*table.column(column)->chunk(0));
  out.reserve(static_cast<std::size_t>(chunk.length()));
  for (int64_t i = 0; i < chunk.length(); ++i) {
    out.push_back(chunk.IsNull(i) ? std::nullopt : std::optional(std::string(chunk.GetView(i))));
  }
  return out;
}

std::vector<std::optional<int64_t>> NumbersA(int64_t first, int64_t count) {
  std::vector<std::optional<int64_t>> out;
  for (const int64_t v : Sequence(first, count)) {
    out.emplace_back(v);
  }
  return out;
}

std::vector<std::optional<int64_t>> NumbersB(int64_t first, int64_t count) {
  std::vector<std::optional<int64_t>> out;
  for (const int64_t v : Sequence(first, count)) {
    out.push_back(v % 5 == 0 ? std::nullopt : std::optional(-v));
  }
  return out;
}

TEST_F(ParquetScanTest, ReadsEveryRowGroupInOrder) {
  auto table = ParquetTable::Open({WriteNumbers("a.parquet", 0, 10, 3)});
  ASSERT_TRUE(table.ok()) << table.status().ToString();
  const Scanned s = Scan(**table, {0, 1}, 64);
  ASSERT_NE(s.table, nullptr);
  EXPECT_TRUE(s.schema->Equals(*(*table)->schema()));
  EXPECT_EQ(s.table->num_rows(), 10);
  EXPECT_EQ(Int64s(*s.table, 0), NumbersA(0, 10));
  EXPECT_EQ(Int64s(*s.table, 1), NumbersB(0, 10));
}

TEST_F(ParquetScanTest, BatchesHoldAtMostBatchSizeRows) {
  auto table = ParquetTable::Open({WriteNumbers("a.parquet", 0, 1000, 300)});
  ASSERT_TRUE(table.ok()) << table.status().ToString();
  for (const int64_t batch_size : {int64_t{1}, int64_t{7}, int64_t{300}, int64_t{65536}}) {
    const Scanned s = Scan(**table, {0}, batch_size);
    ASSERT_NE(s.table, nullptr);
    int64_t total = 0;
    for (const int64_t rows : s.batch_rows) {
      EXPECT_GT(rows, 0) << batch_size;
      EXPECT_LE(rows, batch_size);
      total += rows;
    }
    EXPECT_EQ(total, 1000) << batch_size;
    EXPECT_EQ(Int64s(*s.table, 0), NumbersA(0, 1000)) << batch_size;
  }
}

TEST_F(ParquetScanTest, FilesInSortedOrder) {
  WriteNumbers("part-2.parquet", 30, 5, 2);
  WriteNumbers("part-0.parquet", 0, 10, 4);
  WriteNumbers("part-1.parquet", 10, 20, 7);
  auto table = ParquetTable::Open({(dir_ / "part-*.parquet").string()});
  ASSERT_TRUE(table.ok()) << table.status().ToString();
  const Scanned s = Scan(**table, {1, 0}, 3);
  ASSERT_NE(s.table, nullptr);
  EXPECT_EQ(s.table->num_rows(), 35);
  EXPECT_EQ(Int64s(*s.table, 1), NumbersA(0, 35));
  EXPECT_EQ(Int64s(*s.table, 0), NumbersB(0, 35));
}

TEST_F(ParquetScanTest, ProjectionOrderIsTheRequestedOrder) {
  auto table = ParquetTable::Open({WriteNumbers("a.parquet", 3, 4, 10)});
  ASSERT_TRUE(table.ok()) << table.status().ToString();
  const Scanned b_then_a = Scan(**table, {1, 0}, 64);
  ASSERT_NE(b_then_a.table, nullptr);
  ASSERT_EQ(b_then_a.schema->num_fields(), 2);
  EXPECT_EQ(b_then_a.schema->field(0)->name(), "b");
  EXPECT_EQ(b_then_a.schema->field(1)->name(), "a");
  EXPECT_EQ(Int64s(*b_then_a.table, 0), NumbersB(3, 4));
  EXPECT_EQ(Int64s(*b_then_a.table, 1), NumbersA(3, 4));
  const Scanned only_b = Scan(**table, {1}, 64);
  ASSERT_NE(only_b.table, nullptr);
  ASSERT_EQ(only_b.schema->num_fields(), 1);
  EXPECT_EQ(Int64s(*only_b.table, 0), NumbersB(3, 4));
}

TEST_F(ParquetScanTest, NoFieldsGivesRowCountsOnly) {
  WriteNumbers("a.parquet", 0, 7, 3);
  WriteNumbers("b.parquet", 7, 2, 3);
  auto table = ParquetTable::Open({(dir_ / "*.parquet").string()});
  ASSERT_TRUE(table.ok()) << table.status().ToString();
  const Scanned s = Scan(**table, {}, 4);
  ASSERT_NE(s.table, nullptr);
  EXPECT_EQ(s.schema->num_fields(), 0);
  EXPECT_EQ(s.table->num_rows(), 9);
  for (const int64_t rows : s.batch_rows) {
    EXPECT_LE(rows, 4);
  }
}

// Top-level field indices are not Parquet leaf column indices: a struct before a column shifts
// the column's leaf index.
TEST_F(ParquetScanTest, NestedFieldsShiftLeafIndices) {
  const auto x = Build<arrow::Int32Builder, int32_t>(std::vector<int32_t>{1, 2, 3});
  const auto y = Build<arrow::Int32Builder, int32_t>(std::vector<int32_t>{4, 5, 6});
  auto st = arrow::StructArray::Make({x, y}, std::vector<std::string>{"x", "y"});
  ASSERT_TRUE(st.ok());
  const auto z = Build<arrow::Int64Builder, int64_t>(std::vector<int64_t>{7, 8, 9});
  auto data = arrow::Table::Make(
      arrow::schema({arrow::field("st", (*st)->type()), arrow::field("z", arrow::int64())}),
      {*st, z});
  auto table = ParquetTable::Open({Write("nested.parquet", data, 2)});
  ASSERT_TRUE(table.ok()) << table.status().ToString();
  EXPECT_TRUE(plan::FromArrow(*(*table)->schema()->field(0)->type()).status().IsNotImplemented());
  const Scanned z_only = Scan(**table, {1}, 64);
  ASSERT_NE(z_only.table, nullptr);
  EXPECT_EQ(Int64s(*z_only.table, 0), (std::vector<std::optional<int64_t>>{7, 8, 9}));
  // An unsupported column still scans in its storage type (every leaf of it).
  const Scanned both = Scan(**table, {1, 0}, 64);
  ASSERT_NE(both.table, nullptr);
  EXPECT_EQ(both.schema->field(1)->type()->id(), arrow::Type::STRUCT);
  EXPECT_EQ(Int64s(*both.table, 0), (std::vector<std::optional<int64_t>>{7, 8, 9}));
}

// Storage types become the engine view: strings -> binary (never validated as UTF-8), FLOAT ->
// DOUBLE, USMALLINT/INTEGER -> DATE with an override; NULLs survive every conversion.
TEST_F(ParquetScanTest, ConvertsToTheEngineView) {
  const std::vector<std::optional<std::string>> strings = {"a", std::nullopt, "", "\xff\xfe",
                                                           "Привет"};
  const std::vector<std::optional<uint16_t>> days16 = {15887, 0, std::nullopt, 65535, 1};
  const std::vector<std::optional<int32_t>> days32 = {-1, std::nullopt, 0, 2932896, -719528};
  const std::vector<std::optional<float>> floats = {1.5F, std::nullopt, -0.25F, 0.1F, 3e38F};
  const std::vector<std::optional<int16_t>> smalls = {-32768, 32767, std::nullopt, 0, -1};
  auto data = arrow::Table::Make(
      arrow::schema({
          arrow::field("utf8", arrow::utf8()),
          arrow::field("large_utf8", arrow::large_utf8()),
          arrow::field("large_binary", arrow::large_binary()),
          arrow::field("u16", arrow::uint16()),
          arrow::field("i32", arrow::int32()),
          arrow::field("f", arrow::float32()),
          arrow::field("i16", arrow::int16()),
          arrow::field("dt", arrow::date32()),
      }),
      {Build<arrow::StringBuilder, std::string>(strings),
       Build<arrow::LargeStringBuilder, std::string>(strings),
       Build<arrow::LargeBinaryBuilder, std::string>(strings),
       Build<arrow::UInt16Builder, uint16_t>(days16), Build<arrow::Int32Builder, int32_t>(days32),
       Build<arrow::FloatBuilder, float>(floats), Build<arrow::Int16Builder, int16_t>(smalls),
       Build<arrow::Date32Builder, int32_t>(days32)});
  // ARROW:schema keeps the large types (a Parquet reader gives binary/utf8 by default).
  const std::string path = Write("types.parquet", data, 2, /*store_schema=*/true);
  auto table = ParquetTable::Open(
      {path}, ParquetTableOptions{.overrides = {{.column = "U16"}, {.column = "i32"}}});
  ASSERT_TRUE(table.ok()) << table.status().ToString();
  const auto& schema = *(*table)->schema();
  const std::vector<std::shared_ptr<arrow::DataType>> engine_types = {
      arrow::binary(), arrow::binary(),  arrow::binary(), arrow::date32(),
      arrow::date32(), arrow::float64(), arrow::int16(),  arrow::date32()};
  ASSERT_EQ(schema.num_fields(), 8);
  for (int i = 0; i < schema.num_fields(); ++i) {
    EXPECT_TRUE(schema.field(i)->type()->Equals(*engine_types[static_cast<std::size_t>(i)]))
        << i << ": " << schema.field(i)->type()->ToString();
  }
  for (const int64_t batch_size : {int64_t{1}, int64_t{3}, int64_t{64}}) {
    const Scanned s = Scan(**table, {0, 1, 2, 3, 4, 5, 6, 7}, batch_size);
    ASSERT_NE(s.table, nullptr);
    EXPECT_TRUE(s.schema->Equals(schema));
    EXPECT_EQ(Bytes(*s.table, 0), strings) << batch_size;
    EXPECT_EQ(Bytes(*s.table, 1), strings) << batch_size;
    EXPECT_EQ(Bytes(*s.table, 2), strings) << batch_size;
    EXPECT_EQ(Int64s(*s.table, 3),
              (std::vector<std::optional<int64_t>>{15887, 0, std::nullopt, 65535, 1}));
    EXPECT_EQ(Int64s(*s.table, 4),
              (std::vector<std::optional<int64_t>>{-1, std::nullopt, 0, 2932896, -719528}));
    const auto& d = static_cast<const arrow::DoubleArray&>(*s.table->column(5)->chunk(0));
    EXPECT_EQ(d.Value(0), 1.5);
    EXPECT_TRUE(d.IsNull(1));
    EXPECT_EQ(d.Value(2), -0.25);
    EXPECT_EQ(d.Value(3), static_cast<double>(0.1F));
    EXPECT_EQ(d.Value(4), static_cast<double>(3e38F));
    EXPECT_EQ(Int64s(*s.table, 6),
              (std::vector<std::optional<int64_t>>{-32768, 32767, std::nullopt, 0, -1}));
    EXPECT_EQ(Int64s(*s.table, 7),
              (std::vector<std::optional<int64_t>>{-1, std::nullopt, 0, 2932896, -719528}));
  }
}

TEST_F(ParquetScanTest, EmptyFiles) {
  const std::string empty = WriteNumbers("a.parquet", 0, 0, 10);
  auto table = ParquetTable::Open({empty});
  ASSERT_TRUE(table.ok()) << table.status().ToString();
  EXPECT_EQ((*table)->exact_row_count(), 0);
  for (const std::vector<int>& fields : {std::vector<int>{0, 1}, std::vector<int>{}}) {
    const Scanned s = Scan(**table, fields, 8);
    ASSERT_NE(s.table, nullptr);
    EXPECT_TRUE(s.batch_rows.empty());
    EXPECT_EQ(s.table->num_rows(), 0);
  }
  // Between non-empty files.
  WriteNumbers("b.parquet", 0, 3, 10);
  WriteNumbers("c.parquet", 3, 2, 10);
  auto mixed = ParquetTable::Open({(dir_ / "*.parquet").string()});
  ASSERT_TRUE(mixed.ok()) << mixed.status().ToString();
  const Scanned s = Scan(**mixed, {0}, 8);
  ASSERT_NE(s.table, nullptr);
  EXPECT_EQ(Int64s(*s.table, 0), NumbersA(0, 5));
}

TEST_F(ParquetScanTest, InvalidRequests) {
  auto table = ParquetTable::Open({WriteNumbers("a.parquet", 0, 3, 10)});
  ASSERT_TRUE(table.ok()) << table.status().ToString();
  EXPECT_TRUE((*table)->Scan({2}, 8).status().IsInvalid());
  EXPECT_TRUE((*table)->Scan({-1}, 8).status().IsInvalid());
  EXPECT_TRUE((*table)->Scan({0, 1, 0}, 8).status().IsInvalid());
  EXPECT_TRUE((*table)->Scan({0}, 0).status().IsInvalid());
  EXPECT_TRUE((*table)->Scan({0}, -5).status().IsInvalid());
}

TEST_F(ParquetScanTest, CloseEndsTheScan) {
  auto table = ParquetTable::Open({WriteNumbers("a.parquet", 0, 10, 2)});
  ASSERT_TRUE(table.ok()) << table.status().ToString();
  auto reader = (*table)->Scan({0}, 2);
  ASSERT_TRUE(reader.ok()) << reader.status().ToString();
  std::shared_ptr<arrow::RecordBatch> batch;
  ASSERT_TRUE((*reader)->ReadNext(&batch).ok());
  ASSERT_NE(batch, nullptr);
  ASSERT_TRUE((*reader)->Close().ok());
  ASSERT_TRUE((*reader)->ReadNext(&batch).ok());
  EXPECT_EQ(batch, nullptr);
}

// Files that change or break after Open are I/O errors naming the file, never exceptions.
TEST_F(ParquetScanTest, BrokenFilesAreIOErrors) {
  const std::string a = WriteNumbers("a.parquet", 0, 4, 2);
  const std::string b = WriteNumbers("b.parquet", 4, 4, 2);
  auto table = ParquetTable::Open({a, b});
  ASSERT_TRUE(table.ok()) << table.status().ToString();
  const auto scan_error = [&](const std::vector<int>& fields) {
    auto reader = (*table)->Scan(fields, 3);
    EXPECT_TRUE(reader.ok()) << reader.status().ToString();
    arrow::Status st;
    while (reader.ok() && st.ok()) {
      std::shared_ptr<arrow::RecordBatch> batch;
      st = (*reader)->ReadNext(&batch);
      if (st.ok() && batch == nullptr) {
        break;
      }
    }
    return st;
  };
  // b.parquet is no longer a Parquet file.
  std::ofstream(b, std::ios::trunc) << "this is not a parquet file";
  arrow::Status st = scan_error({0, 1});
  EXPECT_TRUE(st.IsIOError()) << st.ToString();
  EXPECT_NE(st.message().find("b.parquet"), std::string::npos) << st.ToString();
  // b.parquet holds other types now.
  auto other = arrow::Table::Make(
      arrow::schema({arrow::field("a", arrow::utf8()), arrow::field("b", arrow::int32())}),
      {Build<arrow::StringBuilder, std::string>(std::vector<std::string>{"x"}),
       Build<arrow::Int32Builder, int32_t>(std::vector<int32_t>{1})});
  Write("b.parquet", other, 10);
  st = scan_error({0});
  EXPECT_TRUE(st.IsIOError()) << st.ToString();
  EXPECT_NE(st.message().find("changed"), std::string::npos) << st.ToString();
  // b.parquet has fewer fields now.
  auto narrower = arrow::Table::Make(arrow::schema({arrow::field("a", arrow::int64())}),
                                     {Build<arrow::Int64Builder, int64_t>(Sequence(0, 2))});
  Write("b.parquet", narrower, 10);
  st = scan_error({1});
  EXPECT_TRUE(st.IsIOError()) << st.ToString();
  // b.parquet is gone.
  fs::remove(b);
  st = scan_error({0});
  EXPECT_TRUE(st.IsIOError()) << st.ToString();
}

// Corrupt data pages behind an intact footer: Open succeeds, the scan fails with an IOError that
// names the file.
TEST_F(ParquetScanTest, CorruptDataPagesAreIOErrors) {
  const std::string path = WriteNumbers("a.parquet", 0, 1000, 1000);
  auto table = ParquetTable::Open({path});
  ASSERT_TRUE(table.ok()) << table.status().ToString();
  {
    // The first column chunk starts right after the 4-byte magic "PAR1".
    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    file.seekp(4);
    const std::string garbage(64, '\xFF');
    file.write(garbage.data(), static_cast<std::streamsize>(garbage.size()));
  }
  auto reader = (*table)->Scan({0, 1}, 100);
  ASSERT_TRUE(reader.ok()) << reader.status().ToString();
  arrow::Status st;
  std::shared_ptr<arrow::RecordBatch> batch;
  do {
    st = (*reader)->ReadNext(&batch);
  } while (st.ok() && batch != nullptr);
  EXPECT_TRUE(st.IsIOError()) << st.ToString();
  EXPECT_NE(st.message().find("a.parquet"), std::string::npos) << st.ToString();
}

// Column type overrides: DATE only, on USMALLINT, INTEGER or DATE columns.
TEST_F(ParquetScanTest, ColumnOverridesAreChecked) {
  const auto x = Build<arrow::Int32Builder, int32_t>(std::vector<int32_t>{1, 2});
  auto st = arrow::StructArray::Make({x}, std::vector<std::string>{"x"});
  ASSERT_TRUE(st.ok());
  auto data = arrow::Table::Make(
      arrow::schema({arrow::field("day", arrow::date32()), arrow::field("n", arrow::uint16()),
                     arrow::field("st", (*st)->type())}),
      {Build<arrow::Date32Builder, int32_t>(std::vector<int32_t>{15887, 15888}),
       Build<arrow::UInt16Builder, uint16_t>(std::vector<uint16_t>{1, 2}), *st});
  const std::string path = Write("types.parquet", data, 10);
  // A DATE column read as DATE: nothing to convert.
  auto dated = ParquetTable::Open({path}, ParquetTableOptions{.overrides = {{.column = "DAY"}}});
  ASSERT_TRUE(dated.ok()) << dated.status().ToString();
  EXPECT_EQ((*dated)->schema()->field(0)->type()->id(), arrow::Type::DATE32);
  const Scanned s = Scan(**dated, {0}, 8);
  ASSERT_NE(s.table, nullptr);
  EXPECT_EQ(Int64s(*s.table, 0), (std::vector<std::optional<int64_t>>{15887, 15888}));
  // Only DATE is an override target, and only integer day numbers can be read as dates.
  EXPECT_TRUE(ParquetTable::Open(
                  {path}, ParquetTableOptions{.overrides = {{.column = "n",
                                                             .type = plan::LogicalType::kBigInt}}})
                  .status()
                  .IsInvalid());
  EXPECT_TRUE(ParquetTable::Open({path}, ParquetTableOptions{.overrides = {{.column = "st"}}})
                  .status()
                  .IsInvalid());
}

}  // namespace
}  // namespace antb1::io
