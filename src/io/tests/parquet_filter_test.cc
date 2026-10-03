// ParquetTable::ScanPart with a plan::ScanFilter (docs/adr/0020-filter-pushdown.md): the rows of
// the part that pass the filter, as ScanPart without one returns them, in the same order.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <arrow/util/bit_util.h>
#include <gtest/gtest.h>
#include <parquet/arrow/writer.h>
#include <parquet/file_reader.h>
#include <parquet/properties.h>

#include "antb1/io/parquet_table.h"
#include "antb1/plan/table.h"

#include "../filtered_scan.h"

namespace antb1::io {
namespace {

namespace fs = std::filesystem;

// A value of a row, as a filter in these tests sees it.
struct Value {
  bool null = true;
  int64_t integer = 0;    // integer and DATE columns
  double real = 0;        // DOUBLE columns
  std::string_view text;  // VARCHAR columns
};

using Predicate = std::function<bool(const Value&)>;

// One predicate per filtered column, applied as a table must apply a ScanFilter.
class TestFilter final : public plan::ScanFilter {
 public:
  TestFilter(std::vector<int> columns, std::vector<Predicate> predicates)
      : columns_(std::move(columns)), predicates_(std::move(predicates)) {}

  [[nodiscard]] const std::vector<int>& columns() const override { return columns_; }

  arrow::Status Apply(int column, const plan::ScanValues& values, int64_t offset,
                      std::uint8_t* selected) const override {
    if (column < 0 || static_cast<std::size_t>(column) >= predicates_.size()) {
      return arrow::Status::Invalid("no such filter column");
    }
    if (fail_) {
      return arrow::Status::Invalid("the filter failed");
    }
    for (int64_t i = 0; i < values.rows; ++i) {
      const auto bit = static_cast<std::uint64_t>(offset + i);
      if (!arrow::bit_util::GetBit(selected, bit)) {
        continue;
      }
      const Value v = values.array != nullptr ? ValueOf(*values.array, i) : StringOf(values, i);
      if (!predicates_[static_cast<std::size_t>(column)](v)) {
        arrow::bit_util::ClearBit(selected, offset + i);
      }
    }
    return arrow::Status::OK();
  }

  void Fail() { fail_ = true; }

  static Value ValueOf(const arrow::Array& array, int64_t i) {
    Value v;
    if (array.IsNull(i)) {
      return v;
    }
    v.null = false;
    switch (array.type_id()) {
      case arrow::Type::INT16:
        v.integer = static_cast<const arrow::Int16Array&>(array).Value(i);
        break;
      case arrow::Type::UINT16:
        v.integer = static_cast<const arrow::UInt16Array&>(array).Value(i);
        break;
      case arrow::Type::INT32:
        v.integer = static_cast<const arrow::Int32Array&>(array).Value(i);
        break;
      case arrow::Type::DATE32:
        v.integer = static_cast<const arrow::Date32Array&>(array).Value(i);
        break;
      case arrow::Type::INT64:
        v.integer = static_cast<const arrow::Int64Array&>(array).Value(i);
        break;
      case arrow::Type::DOUBLE:
        v.real = static_cast<const arrow::DoubleArray&>(array).Value(i);
        break;
      case arrow::Type::BINARY:
        v.text = static_cast<const arrow::BinaryArray&>(array).GetView(i);
        break;
      default:
        break;
    }
    return v;
  }

 private:
  static Value StringOf(const plan::ScanValues& values, int64_t i) {
    Value v;
    if (values.validity != nullptr &&
        !arrow::bit_util::GetBit(values.validity, static_cast<std::uint64_t>(i))) {
      return v;
    }
    v.null = false;
    v.text = values.strings[static_cast<std::size_t>(i)];
    return v;
  }

  std::vector<int> columns_;
  std::vector<Predicate> predicates_;
  bool fail_ = false;
};

constexpr int64_t kRows = 3000;

// Columns of every type the filtered path reads, NULL every 7th row (shifted per column), with
// repeated, empty and long strings.
std::shared_ptr<arrow::Table> MakeTable() {
  arrow::Int16Builder i16;
  arrow::Int32Builder i32;
  arrow::UInt16Builder u16;
  arrow::Int64Builder i64;
  arrow::FloatBuilder f32;
  arrow::DoubleBuilder f64;
  arrow::StringBuilder utf8;
  arrow::BinaryBuilder bin;
  arrow::Date32Builder date;
  arrow::Int32Builder days;  // read as DATE by an override
  arrow::Int64Builder required_i64;
  arrow::StringBuilder required_s;
  arrow::UInt16Builder event_date;  // USMALLINT read as DATE (--clickbench's EventDate)
  for (int64_t r = 0; r < kRows; ++r) {
    const auto null = [&](int shift) { return (r + shift) % 7 == 0; };
    EXPECT_TRUE(
        (null(0) ? i16.AppendNull() : i16.Append(static_cast<int16_t>(((r * 37) % 30000) - 15000)))
            .ok());
    EXPECT_TRUE(
        (null(1) ? i32.AppendNull() : i32.Append(static_cast<int32_t>((r * 101) - 70000))).ok());
    EXPECT_TRUE(
        (null(2) ? u16.AppendNull() : u16.Append(static_cast<uint16_t>((r * 13) % 65000))).ok());
    EXPECT_TRUE((null(3) ? i64.AppendNull() : i64.Append((r * 1000003) - 9000000000LL)).ok());
    EXPECT_TRUE((null(4) ? f32.AppendNull() : f32.Append(static_cast<float>(r) / 3.0F)).ok());
    EXPECT_TRUE((null(5) ? f64.AppendNull() : f64.Append(static_cast<double>(r) * -0.5)).ok());
    std::string text = r % 5 == 0 ? std::string() : "row-" + std::to_string(r % 97);
    if (r % 11 == 0) {
      text += std::string(300, 'x');
    }
    EXPECT_TRUE((null(6) ? utf8.AppendNull() : utf8.Append(text)).ok());
    EXPECT_TRUE((null(0) ? bin.AppendNull() : bin.Append("b" + std::to_string(r % 13))).ok());
    EXPECT_TRUE(
        (null(1) ? date.AppendNull() : date.Append(static_cast<int32_t>(19000 + (r % 400)))).ok());
    EXPECT_TRUE((null(2) ? days.AppendNull() : days.Append(static_cast<int32_t>(18000 + r))).ok());
    EXPECT_TRUE(required_i64.Append((r * 7919) % 100003).ok());
    EXPECT_TRUE(
        required_s.Append(r % 9 == 0 ? std::string() : "req-" + std::to_string(r % 61)).ok());
    EXPECT_TRUE((null(3) ? event_date.AppendNull()
                         : event_date.Append(static_cast<uint16_t>(15000 + (r % 300))))
                    .ok());
  }
  const auto finish = [](auto& builder) { return builder.Finish().ValueOrDie(); };
  return arrow::Table::Make(
      arrow::schema({arrow::field("i16", arrow::int16()), arrow::field("i32", arrow::int32()),
                     arrow::field("u16", arrow::uint16()), arrow::field("i64", arrow::int64()),
                     arrow::field("f32", arrow::float32()), arrow::field("f64", arrow::float64()),
                     arrow::field("s", arrow::utf8()), arrow::field("b", arrow::binary()),
                     arrow::field("d", arrow::date32()), arrow::field("days", arrow::int32()),
                     arrow::field("ri64", arrow::int64(), /*nullable=*/false),
                     arrow::field("rs", arrow::utf8(), /*nullable=*/false),
                     arrow::field("ed", arrow::uint16())}),
      {finish(i16), finish(i32), finish(u16), finish(i64), finish(f32), finish(f64), finish(utf8),
       finish(bin), finish(date), finish(days), finish(required_i64), finish(required_s),
       finish(event_date)});
}

enum class Encoding : std::uint8_t { kDictionary, kPlain, kDictionaryFallback };

class ParquetFilterTest : public ::testing::TestWithParam<Encoding> {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string name = info->name();
    for (char& c : name) {
      if (c == '/') {
        c = '_';
      }
    }
    dir_ = fs::path(::testing::TempDir()) / "antb1_io_filter" / name;
    fs::remove_all(dir_);
    fs::create_directories(dir_);
  }
  void TearDown() override { fs::remove_all(dir_); }

  // The table in row groups of 1100 rows and data pages of about 1 KiB.
  std::string Write(Encoding encoding) {
    const std::string path = (dir_ / "t.parquet").string();
    auto out = arrow::io::FileOutputStream::Open(path).ValueOrDie();
    parquet::WriterProperties::Builder props;
    props.data_pagesize(1024)->write_batch_size(64);
    if (encoding == Encoding::kPlain) {
      props.disable_dictionary();
    } else if (encoding == Encoding::kDictionaryFallback) {
      props.dictionary_pagesize_limit(256);
    }
    EXPECT_TRUE(parquet::arrow::WriteTable(*MakeTable(), arrow::default_memory_pool(), out, 1100,
                                           props.build())
                    .ok());
    EXPECT_TRUE(out->Close().ok());
    return path;
  }

  static std::shared_ptr<ParquetTable> Open(const std::string& path) {
    ParquetTableOptions options;
    options.overrides.push_back({.column = "days", .type = plan::LogicalType::kDate});
    options.overrides.push_back({.column = "ed", .type = plan::LogicalType::kDate});
    auto table = ParquetTable::Open({path}, options);
    EXPECT_TRUE(table.ok()) << table.status().ToString();
    return *table;
  }

  // Every batch of a reader, combined; the batches' sizes checked against `batch_size`.
  static std::shared_ptr<arrow::Table> Drain(arrow::RecordBatchReader& reader, int64_t batch_size) {
    arrow::RecordBatchVector batches;
    while (true) {
      std::shared_ptr<arrow::RecordBatch> batch;
      const arrow::Status status = reader.ReadNext(&batch);
      EXPECT_TRUE(status.ok()) << status.ToString();
      if (!status.ok() || batch == nullptr) {
        break;
      }
      EXPECT_GT(batch->num_rows(), 0);
      EXPECT_LE(batch->num_rows(), batch_size);
      batches.push_back(std::move(batch));
    }
    return arrow::Table::FromRecordBatches(reader.schema(), batches).ValueOrDie();
  }

  fs::path dir_;
};

// The rows of `all` (an unfiltered scan) that pass `predicates` on `columns`.
std::shared_ptr<arrow::Table> Expected(const arrow::Table& all, const std::vector<int>& columns,
                                       const std::vector<Predicate>& predicates) {
  const auto combined = all.CombineChunksToBatch().ValueOrDie();
  arrow::BooleanBuilder mask;
  for (int64_t r = 0; r < combined->num_rows(); ++r) {
    bool pass = true;
    for (std::size_t k = 0; k < columns.size(); ++k) {
      pass = pass && predicates[k](TestFilter::ValueOf(*combined->column(columns[k]), r));
    }
    EXPECT_TRUE(mask.Append(pass).ok());
  }
  const auto filter = mask.Finish().ValueOrDie();
  arrow::ArrayVector arrays;
  int64_t rows = 0;
  for (const auto& column : combined->columns()) {
    // A filter by hand: the rows whose mask is true.
    std::vector<int64_t> keep;
    const auto& m = static_cast<const arrow::BooleanArray&>(*filter);
    for (int64_t r = 0; r < m.length(); ++r) {
      if (m.Value(r)) {
        keep.push_back(r);
      }
    }
    rows = static_cast<int64_t>(keep.size());
    std::unique_ptr<arrow::ArrayBuilder> builder;
    EXPECT_TRUE(arrow::MakeBuilder(arrow::default_memory_pool(), column->type(), &builder).ok());
    for (const int64_t r : keep) {
      EXPECT_TRUE(builder->AppendArraySlice(*column->data(), r, 1).ok());
    }
    arrays.push_back(builder->Finish().ValueOrDie());
  }
  return arrow::Table::Make(all.schema(), arrays, rows);
}

// `all` with one more column, `position`: 0, 1, ... (the rows' positions in the part).
std::shared_ptr<arrow::Table> WithPositions(const arrow::Table& all) {
  arrow::Int64Builder positions;
  for (int64_t r = 0; r < all.num_rows(); ++r) {
    EXPECT_TRUE(positions.Append(r).ok());
  }
  return all
      .AddColumn(all.num_columns(), arrow::field("position", arrow::int64(), /*nullable=*/false),
                 std::make_shared<arrow::ChunkedArray>(positions.Finish().ValueOrDie()))
      .ValueOrDie();
}

struct Case {
  std::string name;
  std::vector<int> columns;  // positions in the scanned fields
  std::vector<Predicate> predicates;
};

std::vector<Case> Cases() {
  const auto not_null = [](const Value& v) { return !v.null; };
  const auto contains = [](std::string_view needle) {
    return [needle](const Value& v) { return !v.null && v.text.contains(needle); };
  };
  return {
      {.name = "keep_all", .columns = {0}, .predicates = {[](const Value&) { return true; }}},
      {.name = "keep_none", .columns = {6}, .predicates = {[](const Value&) { return false; }}},
      {.name = "string_contains", .columns = {6}, .predicates = {contains("-1")}},
      {.name = "string_null", .columns = {6}, .predicates = {[](const Value& v) {
                                                return v.null;
                                              }}},
      {.name = "long_strings", .columns = {6}, .predicates = {[](const Value& v) {
                                                 return !v.null && v.text.size() > 100;
                                               }}},
      {.name = "empty_strings", .columns = {6}, .predicates = {[](const Value& v) {
                                                  return !v.null && v.text.empty();
                                                }}},
      {.name = "integer_mod", .columns = {1}, .predicates = {[](const Value& v) {
                                                return !v.null && v.integer % 3 == 0;
                                              }}},
      {.name = "int16_and_binary",
       .columns = {0, 7},
       .predicates = {[](const Value& v) { return !v.null && v.integer > 0; },
                      [](const Value& v) { return !v.null && v.text == "b5"; }}},
      {.name = "two_strings",
       .columns = {6, 7},
       .predicates = {[](const Value& v) { return !v.null && v.text.starts_with("row-1"); },
                      not_null}},
      {.name = "date_and_double",
       .columns = {8, 5},
       .predicates = {[](const Value& v) { return !v.null && v.integer % 2 == 0; },
                      [](const Value& v) { return !v.null && v.real < -100; }}},
      {.name = "float_widened", .columns = {4}, .predicates = {[](const Value& v) {
                                                  return !v.null && v.real > 500.25;
                                                }}},
      {.name = "uint16_and_override",
       .columns = {2, 9},
       .predicates = {[](const Value& v) { return !v.null && v.integer > 30000; }, not_null}},
      {.name = "int64", .columns = {3}, .predicates = {[](const Value& v) {
                                          return !v.null && v.integer % 7 == 1;
                                        }}},
      {.name = "required_int64", .columns = {10}, .predicates = {[](const Value& v) {
                                                    return !v.null && v.integer % 5 == 2;
                                                  }}},
      {.name = "required_string", .columns = {11}, .predicates = {[](const Value& v) {
                                                     return !v.null && v.text.ends_with('1');
                                                   }}},
      {.name = "required_and_nullable",
       .columns = {11, 6},
       .predicates = {[](const Value& v) { return !v.null && !v.text.empty(); }, not_null}},
      {.name = "usmallint_as_date", .columns = {12}, .predicates = {[](const Value& v) {
                                                       return !v.null && v.integer % 3 == 0;
                                                     }}},
  };
}

TEST_P(ParquetFilterTest, ReturnsTheRowsThatPassAsAScanWithoutAFilter) {
  const auto table = Open(Write(GetParam()));
  const std::vector<int> fields = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  ASSERT_TRUE(table->supports_scan_filter(fields));
  ASSERT_GE(table->num_parts(), 3);
  for (const Case& c : Cases()) {
    for (const int64_t batch_size : {int64_t{1}, int64_t{7}, int64_t{1000}, int64_t{65536}}) {
      if (batch_size == 1 && c.name != "string_contains" && c.name != "date_and_double") {
        continue;  // one row a batch is slow: a few cases are enough
      }
      for (int64_t part = 0; part < table->num_parts(); ++part) {
        auto plain = table->ScanPart(part, fields, batch_size);
        ASSERT_TRUE(plain.ok()) << plain.status().ToString();
        const auto all = Drain(**plain, batch_size);
        auto filter = std::make_shared<TestFilter>(c.columns, c.predicates);
        auto filtered =
            table->ScanPart(part, fields, batch_size, arrow::default_memory_pool(), filter);
        ASSERT_TRUE(filtered.ok()) << filtered.status().ToString();
        const auto got = Drain(**filtered, batch_size);
        const auto expected = Expected(*all, c.columns, c.predicates);
        ASSERT_TRUE(got->schema()->Equals(*all->schema()));
        EXPECT_TRUE(got->Equals(*expected))
            << c.name << " batch " << batch_size << " part " << part << ": " << got->num_rows()
            << " rows, expected " << expected->num_rows();
        // With positions: the same rows, and each one's position in the part.
        auto positioned = table->ScanPart(part, fields, batch_size, arrow::default_memory_pool(),
                                          filter, /*positions=*/true);
        ASSERT_TRUE(positioned.ok()) << positioned.status().ToString();
        const auto got_positions = Drain(**positioned, batch_size);
        const auto numbered = WithPositions(*all);
        const auto expected_positions = Expected(*numbered, c.columns, c.predicates);
        ASSERT_TRUE(got_positions->schema()->Equals(*numbered->schema()))
            << got_positions->schema()->ToString();
        EXPECT_TRUE(got_positions->Equals(*expected_positions))
            << c.name << " with positions, batch " << batch_size << " part " << part;
      }
    }
  }
}

// A projection in another order, a filter on a column not output first, and no column at all
// but the filter's (rows counted only through a filter column).
TEST_P(ParquetFilterTest, AnyProjection) {
  const auto table = Open(Write(GetParam()));
  const std::vector<int> fields = {7, 3, 6};
  ASSERT_TRUE(table->supports_scan_filter(fields));
  const std::vector<Predicate> predicates = {
      [](const Value& v) { return !v.null && v.text.contains('1'); }};
  for (int64_t part = 0; part < table->num_parts(); ++part) {
    const auto all = Drain(**table->ScanPart(part, fields, 500), 500);
    auto filter = std::make_shared<TestFilter>(std::vector<int>{2}, predicates);
    const auto got =
        Drain(**table->ScanPart(part, fields, 500, arrow::default_memory_pool(), filter), 500);
    EXPECT_TRUE(got->Equals(*Expected(*all, {2}, predicates)));
  }
}

INSTANTIATE_TEST_SUITE_P(Encodings, ParquetFilterTest,
                         ::testing::Values(Encoding::kDictionary, Encoding::kPlain,
                                           Encoding::kDictionaryFallback));

class ParquetFilterErrorsTest : public ParquetFilterTest {};

TEST_F(ParquetFilterErrorsTest, UnsupportedColumnsAndBadRequests) {
  // A BOOLEAN and a TIMESTAMP column: not read by the filtered path.
  const std::string path = (dir_ / "other.parquet").string();
  arrow::BooleanBuilder flag;
  arrow::TimestampBuilder ts(arrow::timestamp(arrow::TimeUnit::MICRO),
                             arrow::default_memory_pool());
  arrow::Int64Builder n;
  for (int i = 0; i < 10; ++i) {
    ASSERT_TRUE(flag.Append(i % 2 == 0).ok());
    ASSERT_TRUE(ts.Append(i).ok());
    ASSERT_TRUE(n.Append(i).ok());
  }
  const auto t = arrow::Table::Make(
      arrow::schema({arrow::field("flag", arrow::boolean()),
                     arrow::field("ts", arrow::timestamp(arrow::TimeUnit::MICRO)),
                     arrow::field("n", arrow::int64())}),
      {flag.Finish().ValueOrDie(), ts.Finish().ValueOrDie(), n.Finish().ValueOrDie()});
  auto out = arrow::io::FileOutputStream::Open(path).ValueOrDie();
  ASSERT_TRUE(parquet::arrow::WriteTable(*t, arrow::default_memory_pool(), out, 10).ok());
  ASSERT_TRUE(out->Close().ok());
  auto table = ParquetTable::Open({path});
  ASSERT_TRUE(table.ok());
  EXPECT_FALSE((*table)->supports_scan_filter({0}));
  EXPECT_FALSE((*table)->supports_scan_filter({1}));
  EXPECT_TRUE((*table)->supports_scan_filter({2}));
  EXPECT_FALSE((*table)->supports_scan_filter({2, 7}));
  auto filter = std::make_shared<TestFilter>(
      std::vector<int>{0}, std::vector<Predicate>{[](const Value&) { return true; }});
  EXPECT_TRUE((*table)
                  ->ScanPart(0, {0}, 10, arrow::default_memory_pool(), filter)
                  .status()
                  .IsNotImplemented());
  EXPECT_TRUE(
      (*table)->ScanPart(5, {2}, 10, arrow::default_memory_pool(), filter).status().IsInvalid());
  EXPECT_TRUE(
      (*table)->ScanPart(0, {2}, 0, arrow::default_memory_pool(), filter).status().IsInvalid());
  EXPECT_TRUE((*table)
                  ->ScanPart(0, {2}, 10, arrow::default_memory_pool(), nullptr, /*positions=*/true)
                  .status()
                  .IsInvalid())
      << "positions without a filter";
  auto beyond = std::make_shared<TestFilter>(
      std::vector<int>{3}, std::vector<Predicate>{[](const Value&) { return true; }});
  EXPECT_TRUE(
      (*table)->ScanPart(0, {2}, 10, arrow::default_memory_pool(), beyond).status().IsInvalid());
}

TEST_F(ParquetFilterErrorsTest, FilterErrorsAndChangedFiles) {
  const std::string path = Write(Encoding::kDictionary);
  const auto table = Open(path);
  auto filter = std::make_shared<TestFilter>(
      std::vector<int>{0}, std::vector<Predicate>{[](const Value&) { return true; }});
  filter->Fail();
  auto reader = table->ScanPart(0, {6}, 100, arrow::default_memory_pool(), filter);
  ASSERT_TRUE(reader.ok());
  std::shared_ptr<arrow::RecordBatch> batch;
  const arrow::Status failed = (*reader)->ReadNext(&batch);
  EXPECT_TRUE(failed.IsInvalid()) << failed.ToString();
  EXPECT_EQ(failed.message(), "the filter failed");
  // The file rewritten after Open: an I/O error naming it.
  {
    std::ofstream append(path, std::ios::app | std::ios::binary);
    append << "junk";
  }
  auto ok_filter = std::make_shared<TestFilter>(
      std::vector<int>{0}, std::vector<Predicate>{[](const Value&) { return true; }});
  auto changed = table->ScanPart(0, {6}, 100, arrow::default_memory_pool(), ok_filter);
  ASSERT_TRUE(changed.ok());
  const arrow::Status io = (*changed)->ReadNext(&batch);
  EXPECT_TRUE(io.IsIOError()) << io.ToString();
  EXPECT_NE(io.message().find("t.parquet"), std::string::npos) << io.ToString();
}

// A corrupt data page (the file's size and footer unchanged): an I/O error naming the file, from a
// filter's column and from another column.
TEST_F(ParquetFilterErrorsTest, CorruptPagesAreIOErrors) {
  const std::string path = Write(Encoding::kPlain);
  const auto table = Open(path);
  {
    // The first column chunk starts right after the 4-byte magic "PAR1".
    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    file.seekp(4);
    const std::string garbage(64, '\xFF');
    file.write(garbage.data(), static_cast<std::streamsize>(garbage.size()));
  }
  const auto keep = std::vector<Predicate>{[](const Value&) { return true; }};
  // Field 0 (i16, the first chunk) as the filter's column, then as another column.
  for (const auto& [fields, column] :
       {std::pair{std::vector<int>{0, 6}, 0}, std::pair{std::vector<int>{6, 0}, 0}}) {
    auto filter = std::make_shared<TestFilter>(std::vector<int>{column}, keep);
    auto reader = table->ScanPart(0, fields, 100, arrow::default_memory_pool(), filter);
    ASSERT_TRUE(reader.ok()) << reader.status().ToString();
    arrow::Status st;
    std::shared_ptr<arrow::RecordBatch> batch;
    do {
      st = (*reader)->ReadNext(&batch);
    } while (st.ok() && batch != nullptr);
    EXPECT_TRUE(st.IsIOError()) << st.ToString();
    EXPECT_NE(st.message().find("t.parquet"), std::string::npos) << st.ToString();
  }
}

// Parts of a second file: each read with its own file's footer.
TEST_F(ParquetFilterErrorsTest, PartsOfEveryFile) {
  const std::string first = Write(Encoding::kDictionary);
  const std::string second = (dir_ / "u.parquet").string();
  fs::copy_file(first, second);
  ParquetTableOptions options;
  options.overrides.push_back({.column = "days", .type = plan::LogicalType::kDate});
  options.overrides.push_back({.column = "ed", .type = plan::LogicalType::kDate});
  auto table = ParquetTable::Open({first, second}, options);
  ASSERT_TRUE(table.ok()) << table.status().ToString();
  const std::vector<int> fields = {6, 3, 12};
  const std::vector<Predicate> predicates = {
      [](const Value& v) { return !v.null && v.text.contains('3'); }};
  int64_t rows = 0;
  for (int64_t part = 0; part < (*table)->num_parts(); ++part) {
    const auto all = Drain(**(*table)->ScanPart(part, fields, 256), 256);
    auto filter = std::make_shared<TestFilter>(std::vector<int>{0}, predicates);
    const auto got =
        Drain(**(*table)->ScanPart(part, fields, 256, arrow::default_memory_pool(), filter), 256);
    EXPECT_TRUE(got->Equals(*Expected(*all, {0}, predicates))) << part;
    rows += got->num_rows();
  }
  EXPECT_GT(rows, 0);
}

// A pool that fails every allocation that would take it past `cap` bytes in use.
class CappedPool final : public arrow::MemoryPool {
 public:
  explicit CappedPool(int64_t cap) : cap_(cap) {}
  arrow::Status Allocate(int64_t size, int64_t alignment, uint8_t** out) override {
    if (used_ + size > cap_) {
      return arrow::Status::OutOfMemory("capped at ", cap_);
    }
    ARROW_RETURN_NOT_OK(backend_->Allocate(size, alignment, out));
    used_ += size;
    return arrow::Status::OK();
  }
  arrow::Status Reallocate(int64_t old_size, int64_t new_size, int64_t alignment,
                           uint8_t** ptr) override {
    if (used_ - old_size + new_size > cap_) {
      return arrow::Status::OutOfMemory("capped at ", cap_);
    }
    ARROW_RETURN_NOT_OK(backend_->Reallocate(old_size, new_size, alignment, ptr));
    used_ += new_size - old_size;
    return arrow::Status::OK();
  }
  void Free(uint8_t* buffer, int64_t size, int64_t alignment) override {
    backend_->Free(buffer, size, alignment);
    used_ -= size;
  }
  [[nodiscard]] int64_t bytes_allocated() const override { return used_; }
  [[nodiscard]] int64_t total_bytes_allocated() const override { return used_; }
  [[nodiscard]] int64_t num_allocations() const override { return 0; }
  [[nodiscard]] std::string backend_name() const override { return "capped"; }

 private:
  arrow::MemoryPool* backend_ = arrow::default_memory_pool();
  int64_t cap_;
  int64_t used_ = 0;
};

// Out of memory anywhere in a filtered scan (pages, decoded values, the filter's kept strings,
// the output, the rows' positions): an OutOfMemory status, never a crash, and every byte given
// back; with enough memory the rows are those of the unlimited scan.
TEST_F(ParquetFilterErrorsTest, OutOfMemoryAnywhere) {
  const auto table = Open(Write(Encoding::kDictionaryFallback));
  const std::vector<int> fields = {6, 3, 11, 12, 5};
  const std::vector<Predicate> predicates = {
      [](const Value& v) { return !v.null && v.text.contains('1'); },
      [](const Value& v) { return !v.null && v.integer % 2 == 0; }};
  const auto full =
      Drain(**table->ScanPart(0, fields, 200, arrow::default_memory_pool(),
                              std::make_shared<TestFilter>(std::vector<int>{0, 1}, predicates),
                              /*positions=*/true),
            200);
  bool succeeded = false;
  int failures = 0;
  for (int64_t cap = 0; cap < int64_t{8} * 1024 * 1024 && !succeeded; cap = (cap * 3 / 2) + 1024) {
    CappedPool pool(cap);
    {
      auto reader = table->ScanPart(
          0, fields, 200, &pool, std::make_shared<TestFilter>(std::vector<int>{0, 1}, predicates),
          /*positions=*/true);
      ASSERT_TRUE(reader.ok()) << reader.status().ToString();
      arrow::RecordBatchVector batches;
      arrow::Status st;
      while (true) {
        std::shared_ptr<arrow::RecordBatch> batch;
        st = (*reader)->ReadNext(&batch);
        if (!st.ok() || batch == nullptr) {
          break;
        }
        batches.push_back(std::move(batch));
      }
      if (st.ok()) {
        succeeded = true;
        const auto got = arrow::Table::FromRecordBatches((*reader)->schema(), batches).ValueOrDie();
        EXPECT_TRUE(got->Equals(*full)) << cap;
      } else {
        ++failures;
        EXPECT_TRUE(st.IsOutOfMemory() || st.IsIOError()) << cap << ": " << st.ToString();
      }
    }
    EXPECT_EQ(pool.bytes_allocated(), 0) << cap;
  }
  EXPECT_TRUE(succeeded);
  EXPECT_GT(failures, 3);
}

// A filter's failure on a fixed-width column; Close, then the stream has ended.
TEST_F(ParquetFilterErrorsTest, FixedWidthFilterErrorsAndClose) {
  const auto table = Open(Write(Encoding::kPlain));
  auto failing = std::make_shared<TestFilter>(
      std::vector<int>{1}, std::vector<Predicate>{[](const Value&) { return true; }});
  failing->Fail();
  auto reader = table->ScanPart(0, {6, 3}, 100, arrow::default_memory_pool(), failing);
  ASSERT_TRUE(reader.ok());
  std::shared_ptr<arrow::RecordBatch> batch;
  EXPECT_TRUE((*reader)->ReadNext(&batch).IsInvalid());
  auto keep = std::make_shared<TestFilter>(
      std::vector<int>{1}, std::vector<Predicate>{[](const Value&) { return true; }});
  auto closed = table->ScanPart(0, {6, 3}, 100, arrow::default_memory_pool(), keep);
  ASSERT_TRUE(closed.ok());
  ASSERT_TRUE((*closed)->ReadNext(&batch).ok());
  EXPECT_NE(batch, nullptr);
  ASSERT_TRUE((*closed)->Close().ok());
  ASSERT_TRUE((*closed)->ReadNext(&batch).ok());
  EXPECT_EQ(batch, nullptr);
}

// Which leaves the filtered path reads (FilteredColumnOf), and MakeFilteredScan's checks.
TEST_F(ParquetFilterErrorsTest, ReadableLeavesAndRequests) {
  // Leaves: 0 i32 (INT32), 1 l64 (INT64), 2 f (FLOAT), 3 d (DOUBLE), 4 s (BYTE_ARRAY),
  // 5 the list's element (repeated), 6 the struct's field (definition level 2).
  arrow::Int32Builder i32;
  arrow::Int64Builder l64;
  arrow::FloatBuilder f;
  arrow::DoubleBuilder d;
  arrow::StringBuilder text;
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(i32.Append(i).ok());
    ASSERT_TRUE(l64.Append(i).ok());
    ASSERT_TRUE(f.Append(static_cast<float>(i)).ok());
    ASSERT_TRUE(d.Append(i).ok());
    ASSERT_TRUE(text.Append("x").ok());
  }
  const auto int32s = [](const std::vector<int32_t>& v) {
    arrow::Int32Builder b;
    EXPECT_TRUE(b.AppendValues(v).ok());
    return std::static_pointer_cast<arrow::Int32Array>(b.Finish().ValueOrDie());
  };
  auto values = int32s({1, 2, 3, 4});
  auto list = arrow::ListArray::FromArrays(*int32s({0, 1, 2, 3, 4}), *values).ValueOrDie();
  auto inner = int32s({5, 6, 7, 8});
  auto structs =
      arrow::StructArray::Make({inner}, {arrow::field("v", arrow::int32())}).ValueOrDie();
  const auto t = arrow::Table::Make(
      arrow::schema({arrow::field("i32", arrow::int32()), arrow::field("l64", arrow::int64()),
                     arrow::field("f", arrow::float32()), arrow::field("d", arrow::float64()),
                     arrow::field("s", arrow::utf8()), arrow::field("l", list->type()),
                     arrow::field("st", structs->type())}),
      {i32.Finish().ValueOrDie(), l64.Finish().ValueOrDie(), f.Finish().ValueOrDie(),
       d.Finish().ValueOrDie(), text.Finish().ValueOrDie(), list, structs});
  const std::string path = (dir_ / "leaves.parquet").string();
  {
    auto out = arrow::io::FileOutputStream::Open(path).ValueOrDie();
    ASSERT_TRUE(parquet::arrow::WriteTable(*t, arrow::default_memory_pool(), out, 4).ok());
    ASSERT_TRUE(out->Close().ok());
  }
  const auto metadata = parquet::ParquetFileReader::OpenFile(path)->metadata();
  const auto ok = [&](int leaf, const auto& storage, const auto& engine) {
    return FilteredColumnOf(*metadata, leaf, *storage, *engine).has_value();
  };
  EXPECT_TRUE(ok(0, arrow::int32(), arrow::int32()));
  EXPECT_TRUE(ok(0, arrow::int32(), arrow::date32()));
  EXPECT_FALSE(ok(0, arrow::int16(), arrow::int32()));   // storage and engine differ
  EXPECT_FALSE(ok(1, arrow::int32(), arrow::int32()));   // physical INT64
  EXPECT_FALSE(ok(1, arrow::int64(), arrow::date32()));  // DATE from INT64
  EXPECT_TRUE(ok(1, arrow::int64(), arrow::int64()));
  EXPECT_FALSE(ok(0, arrow::int64(), arrow::int64()));  // physical INT32
  // A DECIMAL is never filtered in the scan (ADR 0021), whatever its storage.
  EXPECT_FALSE(ok(0, arrow::decimal128(9, 2), arrow::decimal128(9, 2)));
  EXPECT_FALSE(ok(1, arrow::decimal128(15, 2), arrow::decimal128(15, 2)));
  EXPECT_TRUE(ok(2, arrow::float32(), arrow::float64()));
  EXPECT_FALSE(ok(3, arrow::float32(), arrow::float64()));  // physical DOUBLE
  EXPECT_TRUE(ok(3, arrow::float64(), arrow::float64()));
  EXPECT_FALSE(ok(2, arrow::float64(), arrow::float64()));  // physical FLOAT
  EXPECT_TRUE(ok(4, arrow::utf8(), arrow::binary()));
  EXPECT_FALSE(ok(0, arrow::binary(), arrow::binary()));  // physical INT32
  EXPECT_FALSE(ok(4, arrow::large_utf8(), arrow::binary()));
  EXPECT_FALSE(ok(4, arrow::utf8(), arrow::boolean()));
  EXPECT_FALSE(ok(5, arrow::int32(), arrow::int32()));  // repeated
  EXPECT_FALSE(ok(6, arrow::int32(), arrow::int32()));  // nested: definition level 2
  EXPECT_FALSE(ok(-1, arrow::int32(), arrow::int32()));
  EXPECT_FALSE(ok(metadata->num_columns(), arrow::int32(), arrow::int32()));

  // MakeFilteredScan: one row group, filter columns among the scanned fields, each once.
  const auto keep = std::vector<Predicate>{[](const Value&) { return true; }};
  const std::optional<FilteredColumn> readable =
      FilteredColumnOf(*metadata, 0, *arrow::int32(), *arrow::int32());
  ASSERT_TRUE(readable.has_value());
  if (!readable.has_value()) {
    return;
  }
  const FilteredColumn& column = *readable;
  const auto schema = arrow::schema({arrow::field("i32", arrow::int32())});
  const auto make = [&](std::vector<int> row_groups, std::vector<int> filtered, int64_t batch) {
    return MakeFilteredScan(
               Segment{.path = path,
                       .metadata = metadata,
                       .bytes = 0,
                       .footer = nullptr,
                       .row_groups = std::move(row_groups)},
               {column}, schema, batch, arrow::default_memory_pool(),
               std::make_shared<TestFilter>(std::move(filtered),
                                            std::vector<Predicate>(filtered.size(), keep[0])))
        .status();
  };
  EXPECT_TRUE(make({0, 0}, {0}, 10).IsInvalid());
  EXPECT_TRUE(make({0}, {1}, 10).IsInvalid());
  EXPECT_TRUE(make({0}, {-1}, 10).IsInvalid());
  EXPECT_TRUE(make({0}, {0, 0}, 10).IsInvalid());
  EXPECT_TRUE(make({0}, {0}, 0).IsInvalid());
  EXPECT_TRUE(make({0}, {0}, 10).ok());
}

}  // namespace
}  // namespace antb1::io
