// ParquetTable::Scan: batches of the engine view (plan::Table::schema()) over every file, row
// group and batch in order; ParquetTable::ScanPart: the same for one row group.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <gtest/gtest.h>
#include <parquet/arrow/writer.h>
#include <parquet/properties.h>

#include "antb1/common/int128.h"
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
                    int64_t row_group_rows, bool store_schema = false,
                    bool decimals_as_integers = false) {
    const std::string path = (dir_ / name).string();
    auto out = arrow::io::FileOutputStream::Open(path);
    EXPECT_TRUE(out.ok()) << out.status().ToString();
    parquet::ArrowWriterProperties::Builder arrow_props;
    if (store_schema) {
      arrow_props.store_schema();
    }
    parquet::WriterProperties::Builder props;
    if (decimals_as_integers) {
      props.enable_store_decimal_as_integer();  // INT32 up to 9 digits, INT64 up to 18
    }
    const arrow::Status st =
        parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), *out, row_group_rows,
                                   props.build(), arrow_props.build());
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

  // Every part scanned in order, as one Scanned (batch_rows of all parts).
  static Scanned ScanParts(const plan::Table& table, const std::vector<int>& fields,
                           int64_t batch_size) {
    Scanned out;
    std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
    for (int64_t part = 0; part < table.num_parts(); ++part) {
      auto reader = table.ScanPart(part, fields, batch_size);
      EXPECT_TRUE(reader.ok()) << part << ": " << reader.status().ToString();
      if (!reader.ok()) {
        return out;
      }
      out.schema = (*reader)->schema();
      int64_t rows = 0;
      while (true) {
        std::shared_ptr<arrow::RecordBatch> batch;
        const arrow::Status st = (*reader)->ReadNext(&batch);
        EXPECT_TRUE(st.ok()) << st.ToString();
        if (!st.ok() || batch == nullptr) {
          break;
        }
        EXPECT_TRUE(batch->ValidateFull().ok());
        EXPECT_LE(batch->num_rows(), batch_size);
        rows += batch->num_rows();
        out.batch_rows.push_back(batch->num_rows());
        batches.push_back(std::move(batch));
      }
      EXPECT_EQ(rows, table.part_rows(part)) << part;
    }
    if (out.schema == nullptr) {
      return out;
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

// A scan holds the data of about one row group at a time, whatever the size of the file. Parquet
// pre-buffering would read coalesced ranges of up to 32 MiB (here: the whole file) and keep every
// column chunk it has read until the file is closed. Everything is allocated from the scan's pool
// (the query's memory budget), which therefore sees it all.
TEST_F(ParquetScanTest, HoldsAboutOneRowGroupAtATime) {
  constexpr int64_t kRowGroups = 32;
  constexpr int64_t kRowsPerGroup = 64;
  constexpr std::size_t kValueBytes = 1024;  // distinct values: about 64 KiB per column chunk
  std::string path;
  {
    arrow::BinaryBuilder builder;
    for (int64_t row = 0; row < kRowGroups * kRowsPerGroup; ++row) {
      std::string value(kValueBytes, static_cast<char>('a' + (row % 26)));
      value.replace(0, 20, std::to_string(row));
      ASSERT_TRUE(builder.Append(value).ok());
    }
    const auto data = arrow::Table::Make(arrow::schema({arrow::field("v", arrow::binary())}),
                                         {builder.Finish().ValueOrDie()});
    path = Write("wide.parquet", data, kRowsPerGroup);
  }
  const auto file_bytes = static_cast<int64_t>(fs::file_size(path));
  auto table = ParquetTable::Open({path});
  ASSERT_TRUE(table.ok()) << table.status().ToString();

  arrow::ProxyMemoryPool pool(arrow::default_memory_pool());
  const int64_t before = pool.bytes_allocated();
  int64_t most = before;
  int64_t batches = 0;
  {
    auto reader = (*table)->Scan({0}, kRowsPerGroup, &pool);
    ASSERT_TRUE(reader.ok()) << reader.status().ToString();
    while (true) {
      std::shared_ptr<arrow::RecordBatch> batch;
      ASSERT_TRUE((*reader)->ReadNext(&batch).ok());
      most = std::max(most, pool.bytes_allocated());
      if (batch == nullptr) {
        break;
      }
      ++batches;
    }
  }
  EXPECT_EQ(batches, kRowGroups);
  EXPECT_GT(pool.max_memory(), kRowsPerGroup * int64_t{kValueBytes});  // a row group's values
  EXPECT_EQ(pool.bytes_allocated(), 0);
  EXPECT_LT(most - before, file_bytes / 4)
      << "the scan held " << (most - before) << " bytes of a " << file_bytes << "-byte file";
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
  // Statistics come from z's own leaf (leaf 2), not from leaf 1 (st.y); the struct has none.
  for (const auto& [part, min, max] :
       std::vector<std::tuple<int64_t, int64_t, int64_t>>{{0, 7, 8}, {1, 9, 9}}) {
    const auto stats = (*table)->part_stats(part, 1);
    ASSERT_TRUE(stats.has_value()) << part;
    const plan::PartStats got = stats.value_or(plan::PartStats{});
    EXPECT_EQ(got.min, std::optional<Int128>(min)) << part;
    EXPECT_EQ(got.max, std::optional<Int128>(max)) << part;
    EXPECT_FALSE((*table)->part_stats(part, 0).has_value()) << part;
  }
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

// A DECIMAL column of at most 38 digits reads as decimal128(p, s) whatever its Parquet storage
// (INT32, INT64 or FIXED_LEN_BYTE_ARRAY) and whatever an ARROW:schema restores (decimal32,
// decimal64, decimal128 or decimal256): the engine's DECIMAL(p, s) (ADR 0021). Wider decimals stay
// unsupported.
TEST_F(ParquetScanTest, ReadsEveryDecimalStorage) {
  arrow::Decimal32Builder d9(arrow::decimal32(9, 2));
  arrow::Decimal64Builder d18(arrow::decimal64(18, 4));
  arrow::Decimal128Builder d38(arrow::decimal128(38, 10));
  arrow::Decimal256Builder d256(arrow::decimal256(38, 0));
  arrow::Decimal128Builder d15(arrow::decimal128(15, 2));
  arrow::Decimal256Builder wide(arrow::decimal256(40, 0));
  const auto ok = [](const arrow::Status& st) { ASSERT_TRUE(st.ok()) << st.ToString(); };
  const std::string nines38(38, '9');
  ok(d9.Append(arrow::Decimal32(999999999)));
  ok(d9.AppendNull());
  ok(d9.Append(arrow::Decimal32(-999999999)));
  ok(d9.Append(arrow::Decimal32(-5)));
  ok(d18.Append(arrow::Decimal64(999999999999999999)));
  ok(d18.Append(arrow::Decimal64(-1)));
  ok(d18.AppendNull());
  ok(d18.Append(arrow::Decimal64(-999999999999999999)));
  ok(d38.Append(arrow::Decimal128(nines38)));
  ok(d38.Append(arrow::Decimal128("-" + nines38)));
  ok(d38.Append(arrow::Decimal128(0)));
  ok(d38.AppendNull());
  ok(d256.AppendNull());
  ok(d256.Append(arrow::Decimal256("-" + nines38)));
  ok(d256.Append(arrow::Decimal256(nines38)));
  ok(d256.Append(arrow::Decimal256(7)));
  ok(d15.Append(arrow::Decimal128(-12345)));
  ok(d15.Append(arrow::Decimal128(999999999999999)));
  ok(d15.Append(arrow::Decimal128(0)));
  ok(d15.AppendNull());
  for (int i = 0; i < 4; ++i) {
    ok(wide.Append(arrow::Decimal256(i)));
  }
  const auto data = arrow::Table::Make(
      arrow::schema({arrow::field("d9", arrow::decimal32(9, 2)),
                     arrow::field("d18", arrow::decimal64(18, 4)),
                     arrow::field("d38", arrow::decimal128(38, 10)),
                     arrow::field("d256", arrow::decimal256(38, 0)),
                     arrow::field("d15", arrow::decimal128(15, 2)),
                     arrow::field("wide", arrow::decimal256(40, 0))}),
      {d9.Finish().ValueOrDie(), d18.Finish().ValueOrDie(), d38.Finish().ValueOrDie(),
       d256.Finish().ValueOrDie(), d15.Finish().ValueOrDie(), wide.Finish().ValueOrDie()});
  const std::vector<std::shared_ptr<arrow::DataType>> engine_types = {
      arrow::decimal128(9, 2), arrow::decimal128(18, 4), arrow::decimal128(38, 10),
      arrow::decimal128(38, 0), arrow::decimal128(15, 2)};
  const std::vector<std::vector<std::string>> expected = {
      {"9999999.99", "null", "-9999999.99", "-0.05"},
      {"99999999999999.9999", "-0.0001", "null", "-99999999999999.9999"},
      {"9999999999999999999999999999.9999999999", "-9999999999999999999999999999.9999999999",
       "0E-10", "null"},
      {"null", "-" + nines38, nines38, "7"},
      {"-123.45", "9999999999999.99", "0.00", "null"}};
  for (const bool store_schema : {false, true}) {
    for (const bool as_integers : {false, true}) {
      SCOPED_TRACE(std::format("ARROW:schema {}, integer storage {}", store_schema, as_integers));
      const std::string path =
          Write(std::format("decimals_{}_{}.parquet", store_schema, as_integers), data, 2,
                store_schema, as_integers);
      auto table = ParquetTable::Open({path}, ParquetTableOptions{});
      ASSERT_TRUE(table.ok()) << table.status().ToString();
      const auto& schema = *(*table)->schema();
      for (std::size_t i = 0; i < engine_types.size(); ++i) {
        EXPECT_TRUE(schema.field(static_cast<int>(i))->type()->Equals(*engine_types[i]))
            << i << ": " << schema.field(static_cast<int>(i))->type()->ToString();
      }
      EXPECT_FALSE(plan::FromArrow(*schema.field(5)->type()).ok())
          << "a 40-digit decimal stays unsupported";
      for (const int64_t batch_size : {int64_t{1}, int64_t{3}}) {
        const Scanned s = Scan(**table, {0, 1, 2, 3, 4}, batch_size);
        ASSERT_NE(s.table, nullptr);
        for (int c = 0; c < 5; ++c) {
          std::vector<std::string> got;
          for (int64_t r = 0; r < s.table->num_rows(); ++r) {
            got.push_back(s.table->column(c)->GetScalar(r).ValueOrDie()->ToString());
          }
          EXPECT_EQ(got, expected[static_cast<std::size_t>(c)]) << c << " " << batch_size;
        }
      }
    }
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

// ---- parts: one per row group with rows, scanned one at a time ----

std::vector<std::optional<int64_t>> PartRows(const plan::Table& table) {
  std::vector<std::optional<int64_t>> out;
  out.reserve(static_cast<std::size_t>(table.num_parts()));
  for (int64_t part = 0; part < table.num_parts(); ++part) {
    out.push_back(table.part_rows(part));
  }
  return out;
}

TEST_F(ParquetScanTest, PartsAreRowGroupsInFileOrder) {
  WriteNumbers("part-0.parquet", 0, 10, 3);
  WriteNumbers("part-1.parquet", 10, 0, 3);  // no rows: no part
  WriteNumbers("part-2.parquet", 10, 7, 7);
  WriteNumbers("part-3.parquet", 17, 5, 2);
  auto table = ParquetTable::Open({(dir_ / "part-*.parquet").string()});
  ASSERT_TRUE(table.ok()) << table.status().ToString();
  EXPECT_EQ((*table)->num_parts(), 8);
  EXPECT_EQ(PartRows(**table), (std::vector<std::optional<int64_t>>{3, 3, 3, 1, 7, 2, 2, 1}));
  EXPECT_EQ((*table)->exact_row_count(), 22);
  for (const std::vector<int>& fields :
       {std::vector<int>{0, 1}, std::vector<int>{1}, std::vector<int>{}}) {
    for (const int64_t batch_size : {int64_t{1}, int64_t{2}, int64_t{64}}) {
      const Scanned whole = Scan(**table, fields, batch_size);
      const Scanned parts = ScanParts(**table, fields, batch_size);
      ASSERT_NE(whole.table, nullptr);
      ASSERT_NE(parts.table, nullptr);
      EXPECT_TRUE(parts.schema->Equals(*whole.schema));
      EXPECT_EQ(parts.table->num_rows(), 22);
      EXPECT_TRUE(parts.table->Equals(*whole.table)) << batch_size;
    }
  }
}

// A part converts its columns to the engine view exactly as the scan of the whole table does.
TEST_F(ParquetScanTest, PartsConvertToTheEngineView) {
  const std::vector<std::optional<std::string>> strings = {"a", std::nullopt, "", "\xff", "bc"};
  const std::vector<std::optional<uint16_t>> days = {15887, 0, std::nullopt, 65535, 1};
  const std::vector<std::optional<float>> floats = {1.5F, std::nullopt, -0.25F, 0.1F, 3e38F};
  auto data = arrow::Table::Make(
      arrow::schema({arrow::field("s", arrow::utf8()), arrow::field("l", arrow::large_binary()),
                     arrow::field("day", arrow::uint16()), arrow::field("f", arrow::float32())}),
      {Build<arrow::StringBuilder, std::string>(strings),
       Build<arrow::LargeBinaryBuilder, std::string>(strings),
       Build<arrow::UInt16Builder, uint16_t>(days), Build<arrow::FloatBuilder, float>(floats)});
  const std::string path = Write("types.parquet", data, 2, /*store_schema=*/true);
  auto table = ParquetTable::Open({path}, ParquetTableOptions{.overrides = {{.column = "day"}}});
  ASSERT_TRUE(table.ok()) << table.status().ToString();
  EXPECT_EQ(PartRows(**table), (std::vector<std::optional<int64_t>>{2, 2, 1}));
  for (const int64_t batch_size : {int64_t{1}, int64_t{64}}) {
    const Scanned whole = Scan(**table, {3, 2, 1, 0}, batch_size);
    const Scanned parts = ScanParts(**table, {3, 2, 1, 0}, batch_size);
    ASSERT_NE(parts.table, nullptr);
    EXPECT_TRUE(parts.schema->Equals(*whole.schema));
    EXPECT_EQ(parts.schema->field(0)->type()->id(), arrow::Type::DOUBLE);
    EXPECT_EQ(parts.schema->field(1)->type()->id(), arrow::Type::DATE32);
    EXPECT_TRUE(parts.table->Equals(*whole.table)) << batch_size;
    EXPECT_EQ(Bytes(*parts.table, 3), strings);
  }
}

TEST_F(ParquetScanTest, TableWithoutRowsHasNoParts) {
  WriteNumbers("a.parquet", 0, 0, 4);
  WriteNumbers("b.parquet", 0, 0, 4);
  auto table = ParquetTable::Open({(dir_ / "*.parquet").string()});
  ASSERT_TRUE(table.ok()) << table.status().ToString();
  EXPECT_EQ((*table)->num_parts(), 0);
  EXPECT_EQ((*table)->part_rows(0), std::nullopt);
  EXPECT_TRUE((*table)->ScanPart(0, {0}, 8).status().IsInvalid());
}

TEST_F(ParquetScanTest, InvalidPartRequests) {
  auto table = ParquetTable::Open({WriteNumbers("a.parquet", 0, 5, 2)});
  ASSERT_TRUE(table.ok()) << table.status().ToString();
  ASSERT_EQ((*table)->num_parts(), 3);
  EXPECT_EQ((*table)->part_rows(-1), std::nullopt);
  EXPECT_EQ((*table)->part_rows(3), std::nullopt);
  EXPECT_TRUE((*table)->ScanPart(-1, {0}, 8).status().IsInvalid());
  EXPECT_TRUE((*table)->ScanPart(3, {0}, 8).status().IsInvalid());
  EXPECT_TRUE((*table)->ScanPart(1, {2}, 8).status().IsInvalid());
  EXPECT_TRUE((*table)->ScanPart(1, {0, 0}, 8).status().IsInvalid());
  EXPECT_TRUE((*table)->ScanPart(1, {0}, 0).status().IsInvalid());
}

// A scan reads a file with the footer read at Open: a file rewritten since then is an I/O error
// naming the file, for a part as for the whole table.
TEST_F(ParquetScanTest, FileChangedAfterOpenIsIOErrorForParts) {
  const std::string a = WriteNumbers("a.parquet", 0, 4, 2);
  const std::string b = WriteNumbers("b.parquet", 4, 4, 2);
  auto table = ParquetTable::Open({a, b});
  ASSERT_TRUE(table.ok()) << table.status().ToString();
  ASSERT_EQ((*table)->num_parts(), 4);
  WriteNumbers("b.parquet", 100, 50, 10);
  // Parts of a.parquet still read.
  auto reader = (*table)->ScanPart(1, {0}, 8);
  ASSERT_TRUE(reader.ok()) << reader.status().ToString();
  std::shared_ptr<arrow::RecordBatch> batch;
  ASSERT_TRUE((*reader)->ReadNext(&batch).ok());
  ASSERT_NE(batch, nullptr);
  EXPECT_EQ(batch->num_rows(), 2);
  // Parts of b.parquet do not.
  reader = (*table)->ScanPart(2, {0}, 8);
  ASSERT_TRUE(reader.ok()) << reader.status().ToString();
  const arrow::Status st = (*reader)->ReadNext(&batch);
  EXPECT_TRUE(st.IsIOError()) << st.ToString();
  EXPECT_NE(st.message().find("b.parquet"), std::string::npos) << st.ToString();
  EXPECT_NE(st.message().find("changed"), std::string::npos) << st.ToString();
}

// A file rewritten with the same size but other values (and so other statistics in its footer)
// is not decoded with the footer read at Open.
TEST_F(ParquetScanTest, SameSizeRewriteIsIOError) {
  const auto write_plain = [&](const std::string& name, int64_t first) {
    const std::string path = (dir_ / name).string();
    auto out = arrow::io::FileOutputStream::Open(path);
    EXPECT_TRUE(out.ok()) << out.status().ToString();
    const auto properties = parquet::WriterProperties::Builder()
                                .disable_dictionary()
                                ->compression(parquet::Compression::UNCOMPRESSED)
                                ->build();
    auto data = arrow::Table::Make(arrow::schema({arrow::field("a", arrow::int64())}),
                                   {Build<arrow::Int64Builder, int64_t>(Sequence(first, 100))});
    EXPECT_TRUE(
        parquet::arrow::WriteTable(*data, arrow::default_memory_pool(), *out, 50, properties).ok());
    EXPECT_TRUE((*out)->Close().ok());
    return path;
  };
  const std::string path = write_plain("a.parquet", 0);
  const std::string other = write_plain("other.parquet", 1000);
  ASSERT_EQ(fs::file_size(path), fs::file_size(other));
  auto table = ParquetTable::Open({path});
  ASSERT_TRUE(table.ok()) << table.status().ToString();
  fs::copy_file(other, path, fs::copy_options::overwrite_existing);
  for (int64_t part = -1; part < (*table)->num_parts(); ++part) {
    auto reader = part < 0 ? (*table)->Scan({0}, 8) : (*table)->ScanPart(part, {0}, 8);
    ASSERT_TRUE(reader.ok()) << reader.status().ToString();
    std::shared_ptr<arrow::RecordBatch> batch;
    const arrow::Status st = (*reader)->ReadNext(&batch);
    EXPECT_TRUE(st.IsIOError()) << part << ": " << st.ToString();
    EXPECT_NE(st.message().find("footer differs"), std::string::npos) << st.ToString();
    EXPECT_NE(st.message().find("a.parquet"), std::string::npos) << st.ToString();
  }
}

// Statistics of a part (a row group) for skipping it: exact min, max and NULL count for
// integer-valued columns, in the engine view (a USMALLINT day number read as DATE keeps its value);
// nothing for other types, for a file written without statistics, or out of range.
TEST_F(ParquetScanTest, PartStatisticsOfIntegerColumns) {
  std::vector<std::optional<int64_t>> id;
  std::vector<std::optional<uint16_t>> day;
  std::vector<std::optional<int64_t>> sparse;  // NULL in the whole second row group
  std::vector<std::optional<double>> real;
  std::vector<std::optional<std::string>> text;
  std::vector<std::optional<int32_t>> days;  // INTEGER day numbers, read as DATE
  arrow::Decimal128Builder huge(arrow::decimal128(38, 0));
  for (int64_t i = 0; i < 9; ++i) {
    days.emplace_back(static_cast<int32_t>(-3 + i));
    ASSERT_TRUE(huge.Append(arrow::Decimal128(-(i + 1))).ok());
    id.emplace_back(100 + i);
    day.emplace_back(static_cast<uint16_t>(15000 + (i * 2)));
    sparse.push_back(i >= 3 && i < 6 ? std::nullopt : std::optional<int64_t>(-i));
    real.emplace_back(static_cast<double>(i) / 2);
    text.emplace_back(std::string(1, static_cast<char>('a' + i)));
  }
  const auto data = arrow::Table::Make(
      arrow::schema({arrow::field("id", arrow::int64()), arrow::field("day", arrow::uint16()),
                     arrow::field("sparse", arrow::int64()), arrow::field("real", arrow::float64()),
                     arrow::field("text", arrow::utf8()), arrow::field("days", arrow::int32()),
                     arrow::field("huge", arrow::decimal128(38, 0))}),
      {Build<arrow::Int64Builder, int64_t>(id), Build<arrow::UInt16Builder, uint16_t>(day),
       Build<arrow::Int64Builder, int64_t>(sparse), Build<arrow::DoubleBuilder, double>(real),
       Build<arrow::StringBuilder, std::string>(text), Build<arrow::Int32Builder, int32_t>(days),
       huge.Finish().ValueOrDie()});
  const std::string path = Write("stats.parquet", data, 3);
  auto table = ParquetTable::Open(
      {path}, ParquetTableOptions{.overrides = {{.column = "day"}, {.column = "days"}}});
  ASSERT_TRUE(table.ok()) << table.status().ToString();
  ASSERT_EQ((*table)->num_parts(), 3);
  const auto stats = [&](int64_t part, int field) { return (*table)->part_stats(part, field); };
  const auto expect = [&](int64_t part, int field, std::optional<int64_t> min,
                          std::optional<int64_t> max, int64_t nulls) {
    const auto got = stats(part, field);
    ASSERT_TRUE(got.has_value()) << part << " " << field;
    const plan::PartStats& s = got.value();
    EXPECT_EQ(s.min, min.has_value() ? std::optional<Int128>(*min) : std::nullopt);
    EXPECT_EQ(s.max, max.has_value() ? std::optional<Int128>(*max) : std::nullopt);
    EXPECT_EQ(s.null_count, nulls);
    EXPECT_EQ(s.rows, 3);
  };
  expect(1, 0, 103, 105, 0);
  expect(2, 1, 15012, 15016, 0);  // DATE read from USMALLINT day numbers
  expect(0, 5, -3, -1, 0);        // DATE read from INTEGER day numbers, before 1970
  expect(0, 2, -2, 0, 0);
  expect(1, 2, std::nullopt, std::nullopt, 3);  // every value NULL
  EXPECT_FALSE(stats(0, 6).has_value()) << "DECIMAL(38,0): no part statistics (ADR 0021)";
  EXPECT_FALSE(stats(0, 3).has_value()) << "DOUBLE: NaN may be missing from min/max";
  EXPECT_FALSE(stats(0, 4).has_value()) << "VARCHAR: min/max may be truncated";
  EXPECT_FALSE(stats(3, 0).has_value());
  EXPECT_FALSE(stats(-1, 0).has_value());
  EXPECT_FALSE(stats(0, 7).has_value());

  // A file written without statistics never skips.
  const std::string bare = (dir_ / "bare.parquet").string();
  {
    auto out = arrow::io::FileOutputStream::Open(bare);
    ASSERT_TRUE(out.ok());
    const auto properties = parquet::WriterProperties::Builder().disable_statistics()->build();
    ASSERT_TRUE(
        parquet::arrow::WriteTable(*data, arrow::default_memory_pool(), *out, 3, properties).ok());
    ASSERT_TRUE((*out)->Close().ok());
  }
  auto plain = ParquetTable::Open({bare});
  ASSERT_TRUE(plain.ok()) << plain.status().ToString();
  EXPECT_FALSE((*plain)->part_stats(0, 0).has_value());
}

}  // namespace
}  // namespace antb1::io
