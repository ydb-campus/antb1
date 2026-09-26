#include "antb1/engine/format.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/array/concatenate.h>
#include <gtest/gtest.h>

namespace antb1::engine {
namespace {

using plan::LogicalType;

template <class Builder, class T>
std::shared_ptr<arrow::Array> Make(const std::shared_ptr<arrow::DataType>& type,
                                   const std::vector<T>& values, bool with_null = false) {
  Builder b(type, arrow::default_memory_pool());
  for (const auto& v : values) {
    EXPECT_TRUE(b.Append(v).ok());
  }
  if (with_null) {
    EXPECT_TRUE(b.AppendNull().ok());
  }
  return b.Finish().ValueOrDie();
}

TEST(FormatValueTest, CanonicalForms) {
  auto ints = Make<arrow::Int64Builder, int64_t>(arrow::int64(), {-5, INT64_MAX}, true);
  EXPECT_EQ(FormatValue(*ints, 0, LogicalType::kBigInt), "-5");
  EXPECT_EQ(FormatValue(*ints, 1, LogicalType::kBigInt), "9223372036854775807");
  EXPECT_EQ(FormatValue(*ints, 2, LogicalType::kBigInt), "NULL");

  auto doubles =
      Make<arrow::DoubleBuilder, double>(arrow::float64(), {0.30000000000000004, 0.1, 1e300});
  EXPECT_EQ(FormatValue(*doubles, 0, LogicalType::kDouble), "0.30000000000000004");
  EXPECT_EQ(FormatValue(*doubles, 1, LogicalType::kDouble), "0.1");
  EXPECT_EQ(FormatValue(*doubles, 2, LogicalType::kDouble), "1e+300");

  // Dates outside years 1 to 9999 as DuckDB prints them.
  auto dates = Make<arrow::Date32Builder, int32_t>(arrow::date32(),
                                                   {19000, 0, -1, -719163, 17542962, INT32_MAX});
  EXPECT_EQ(FormatValue(*dates, 0, LogicalType::kDate), "2022-01-08");
  EXPECT_EQ(FormatValue(*dates, 1, LogicalType::kDate), "1970-01-01");
  EXPECT_EQ(FormatValue(*dates, 2, LogicalType::kDate), "1969-12-31");
  EXPECT_EQ(FormatValue(*dates, 3, LogicalType::kDate), "0001-12-31 (BC)");
  EXPECT_EQ(FormatValue(*dates, 4, LogicalType::kDate), "50000-12-31");
  EXPECT_EQ(FormatValue(*dates, 5, LogicalType::kDate), "infinity");

  auto bin = Make<arrow::BinaryBuilder, std::string>(arrow::binary(), {"a,b", ""});
  EXPECT_EQ(FormatValue(*bin, 0, LogicalType::kVarchar), "a,b");
  EXPECT_EQ(FormatValue(*bin, 1, LogicalType::kVarchar), "");

  arrow::Decimal128Builder dec(arrow::decimal128(38, 0));
  ASSERT_TRUE(dec.Append(arrow::Decimal128("-170141183460469231731687303715884105728")).ok());
  auto decs = dec.Finish().ValueOrDie();
  EXPECT_EQ(FormatValue(*decs, 0, LogicalType::kHugeInt),
            "-170141183460469231731687303715884105728");
}

// Every result type the executor produces (plan::ToArrow of each logical type).
TEST(FormatValueTest, EveryResultType) {
  auto i16 = Make<arrow::Int16Builder, int16_t>(arrow::int16(), {-32768});
  EXPECT_EQ(FormatValue(*i16, 0, LogicalType::kSmallInt), "-32768");
  auto i32 = Make<arrow::Int32Builder, int32_t>(arrow::int32(), {2147483647, 19000});
  EXPECT_EQ(FormatValue(*i32, 0, LogicalType::kInteger), "2147483647");
  EXPECT_EQ(FormatValue(*i32, 1, LogicalType::kDate), "2022-01-08");
  auto u16 = Make<arrow::UInt16Builder, uint16_t>(arrow::uint16(), {65535});
  EXPECT_EQ(FormatValue(*u16, 0, LogicalType::kUSmallInt), "65535");
  auto floats = Make<arrow::FloatBuilder, float>(arrow::float32(), {0.5F});
  EXPECT_EQ(FormatValue(*floats, 0, LogicalType::kDouble), "0.5");
  auto doubles = Make<arrow::DoubleBuilder, double>(
      arrow::float64(),
      {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
       -std::numeric_limits<double>::infinity()});
  EXPECT_EQ(FormatValue(*doubles, 0, LogicalType::kDouble), "nan");
  EXPECT_EQ(FormatValue(*doubles, 1, LogicalType::kDouble), "inf");
  EXPECT_EQ(FormatValue(*doubles, 2, LogicalType::kDouble), "-inf");
  auto utf8 = Make<arrow::StringBuilder, std::string>(arrow::utf8(), {"naïve"});
  EXPECT_EQ(FormatValue(*utf8, 0, LogicalType::kVarchar), "naïve");
  arrow::Decimal128Builder dec(arrow::decimal128(38, 0));
  ASSERT_TRUE(dec.Append(arrow::Decimal128("18446744073709551614")).ok());
  ASSERT_TRUE(dec.AppendNull().ok());
  auto decs = dec.Finish().ValueOrDie();
  EXPECT_EQ(FormatValue(*decs, 0, LogicalType::kHugeInt), "18446744073709551614");
  EXPECT_EQ(FormatValue(*decs, 1, LogicalType::kHugeInt), "NULL");
}

QueryResult OneColumn(const std::shared_ptr<arrow::Array>& array, std::string name,
                      LogicalType type) {
  QueryResult r;
  r.table = arrow::Table::Make(arrow::schema({arrow::field(name, array->type())}), {array});
  r.names = {std::move(name)};
  r.types = {type};
  return r;
}

TEST(FormatResultTest, CsvJsonTable) {
  auto bin = Make<arrow::BinaryBuilder, std::string>(arrow::binary(), {"x\"y", "\xff"}, true);
  auto r = OneColumn(bin, "s", LogicalType::kVarchar);
  EXPECT_EQ(*FormatResult(r, OutputFormat::kCsv), "s\n\"x\"\"y\"\n\xff\n\n");
  EXPECT_EQ(*FormatResult(r, OutputFormat::kJson),
            "[\n {\"s\": \"x\\\"y\"},\n {\"s\": \"\\\\xff\"},\n {\"s\": null}\n]\n");

  auto ints = Make<arrow::Int64Builder, int64_t>(arrow::int64(), {123456});
  auto t = OneColumn(ints, "count_star()", LogicalType::kBigInt);
  EXPECT_EQ(*FormatResult(t, OutputFormat::kTable),
            "count_star()\n------------\n123456\n(1 row)\n");
  EXPECT_EQ(*FormatResult(t, OutputFormat::kJson), "[\n {\"count_star()\": 123456}\n]\n");
}

template <class Builder, class T>
std::shared_ptr<arrow::Array> MakeOpt(const std::shared_ptr<arrow::DataType>& type,
                                      const std::vector<std::optional<T>>& values) {
  Builder b(type, arrow::default_memory_pool());
  for (const auto& v : values) {
    EXPECT_TRUE((v.has_value() ? b.Append(*v) : b.AppendNull()).ok());
  }
  return b.Finish().ValueOrDie();
}

QueryResult MakeResult(const std::vector<std::shared_ptr<arrow::ChunkedArray>>& columns,
                       std::vector<std::string> names, std::vector<LogicalType> types,
                       int64_t num_rows = -1) {
  arrow::FieldVector fields;
  for (std::size_t c = 0; c < columns.size(); ++c) {
    fields.push_back(arrow::field(names[c], columns[c]->type()));
  }
  QueryResult r;
  r.table = arrow::Table::Make(arrow::schema(fields), columns, num_rows);
  r.names = std::move(names);
  r.types = std::move(types);
  return r;
}

// A result has one chunk per batch, and Arrow keeps a binary column above 2 GiB in several chunks
// even after CombineChunks: every chunk layout (empty and sliced chunks, NULLs and values in later
// chunks, columns with different layouts) must print what the same data in one chunk prints.
TEST(FormatResultTest, ChunkedColumnsPrintLikeOneChunk) {
  using S = std::optional<std::string>;
  using I = std::optional<int64_t>;
  using D = std::optional<double>;
  auto strings = [](const std::vector<S>& v) {
    return MakeOpt<arrow::BinaryBuilder>(arrow::binary(), v);
  };
  auto ints = [](const std::vector<I>& v) {
    return MakeOpt<arrow::Int64Builder>(arrow::int64(), v);
  };
  auto doubles = [](const std::vector<D>& v) {
    return MakeOpt<arrow::DoubleBuilder>(arrow::float64(), v);
  };
  const arrow::ArrayVector s = {strings({"a", "b,c"}), strings({}),
                                strings({"skip", "x\"y", std::nullopt, "NULL"})->Slice(1),
                                strings({std::nullopt, "tail\xff"})};
  const arrow::ArrayVector n = {ints({1}), ints({2, std::nullopt, 4, 5, 6, std::nullopt})};
  const arrow::ArrayVector d = {
      doubles({0.5, std::nullopt, 1e300, -2.5}), doubles({}),
      doubles({0.0, std::numeric_limits<double>::infinity(), 0.1, 7.0})->Slice(1, 3)};
  const std::vector<std::string> names = {"s", "n", "d"};
  const std::vector<LogicalType> types = {LogicalType::kVarchar, LogicalType::kBigInt,
                                          LogicalType::kDouble};
  auto chunked = MakeResult(
      {std::make_shared<arrow::ChunkedArray>(s), std::make_shared<arrow::ChunkedArray>(n),
       std::make_shared<arrow::ChunkedArray>(d)},
      names, types);
  ASSERT_EQ(chunked.table->num_rows(), 7);
  ASSERT_EQ(chunked.table->column(0)->num_chunks(), 4);
  auto one = [](const arrow::ArrayVector& chunks) {
    return std::make_shared<arrow::ChunkedArray>(arrow::Concatenate(chunks).ValueOrDie());
  };
  auto single = MakeResult({one(s), one(n), one(d)}, names, types);
  ASSERT_EQ(single.table->column(0)->num_chunks(), 1);

  for (const auto format : {OutputFormat::kTable, OutputFormat::kCsv, OutputFormat::kJson}) {
    auto a = FormatResult(chunked, format);
    auto b = FormatResult(single, format);
    ASSERT_TRUE(a.ok()) << a.status();
    ASSERT_TRUE(b.ok()) << b.status();
    EXPECT_EQ(*a, *b) << static_cast<int>(format);
  }
  EXPECT_EQ(*FormatResult(chunked, OutputFormat::kCsv),
            "s,n,d\n"
            "a,1,0.5\n"
            "\"b,c\",2,\n"
            "\"x\"\"y\",,1e+300\n"
            ",4,-2.5\n"
            "NULL,5,inf\n"
            ",6,0.1\n"
            "tail\xff,,7\n");
  EXPECT_EQ(*FormatResult(chunked, OutputFormat::kJson),
            "[\n"
            " {\"s\": \"a\", \"n\": 1, \"d\": 0.5},\n"
            " {\"s\": \"b,c\", \"n\": 2, \"d\": null},\n"
            " {\"s\": \"x\\\"y\", \"n\": null, \"d\": 1e+300},\n"
            " {\"s\": null, \"n\": 4, \"d\": -2.5},\n"
            " {\"s\": \"NULL\", \"n\": 5, \"d\": \"inf\"},\n"
            " {\"s\": null, \"n\": 6, \"d\": 0.1},\n"
            " {\"s\": \"tail\\\\xff\", \"n\": null, \"d\": 7}\n"
            "]\n");
}

TEST(FormatResultTest, ColumnsWithoutChunks) {
  auto empty = std::make_shared<arrow::ChunkedArray>(arrow::ArrayVector{}, arrow::int64());
  auto r = MakeResult({empty}, {"n"}, {LogicalType::kBigInt});
  EXPECT_EQ(*FormatResult(r, OutputFormat::kTable), "n\n-\n(0 rows)\n");
  EXPECT_EQ(*FormatResult(r, OutputFormat::kCsv), "n\n");
  EXPECT_EQ(*FormatResult(r, OutputFormat::kJson), "[]\n");
}

TEST(FormatResultTest, ColumnLengthDiffersFromTable) {
  auto n = std::make_shared<arrow::ChunkedArray>(
      Make<arrow::Int64Builder, int64_t>(arrow::int64(), {1, 2, 3}));
  auto r = MakeResult({n}, {"n"}, {LogicalType::kBigInt}, 5);
  for (const auto format : {OutputFormat::kTable, OutputFormat::kCsv, OutputFormat::kJson}) {
    EXPECT_TRUE(FormatResult(r, format).status().IsInvalid());
  }
}

// The JSON text of a one-row VARCHAR result.
std::string Json(const std::string& value) {
  auto r = OneColumn(Make<arrow::BinaryBuilder, std::string>(arrow::binary(), {value}), "s",
                     LogicalType::kVarchar);
  return *FormatResult(r, OutputFormat::kJson);
}

std::string JsonRow(const std::string& body) { return "[\n {\"s\": \"" + body + "\"}\n]\n"; }

// Only well-formed UTF-8 (RFC 3629, Table 3-7 of the Unicode standard) is copied; every byte of an
// ill-formed sequence is written as the text \xHH, so the output is valid UTF-8 and valid JSON.
TEST(FormatResultTest, JsonEscapesEveryByteOfIllFormedUtf8) {
  const std::vector<std::pair<std::string, std::string>> ill_formed = {
      {"\xC0\x80", R"(\\xc0\\x80)"},                             // overlong U+0000
      {"\xC1\xBF", R"(\\xc1\\xbf)"},                             // overlong U+007F
      {"\xE0\x80\x80", R"(\\xe0\\x80\\x80)"},                    // overlong U+0000
      {"\xE0\x9F\xBF", R"(\\xe0\\x9f\\xbf)"},                    // overlong U+07FF
      {"\xED\xA0\x80", R"(\\xed\\xa0\\x80)"},                    // surrogate U+D800
      {"\xED\xBF\xBF", R"(\\xed\\xbf\\xbf)"},                    // surrogate U+DFFF
      {"\xF0\x80\x80\x80", R"(\\xf0\\x80\\x80\\x80)"},           // overlong U+0000
      {"\xF0\x8F\xBF\xBF", R"(\\xf0\\x8f\\xbf\\xbf)"},           // overlong U+FFFF
      {"\xF4\x90\x80\x80", R"(\\xf4\\x90\\x80\\x80)"},           // U+110000
      {"\xF5\x80\x80\x80", R"(\\xf5\\x80\\x80\\x80)"},           // lead byte above F4
      {"\xF8\x88\x80\x80\x80", R"(\\xf8\\x88\\x80\\x80\\x80)"},  // five-byte form
      {"\xFF", R"(\\xff)"},
      {"\x80", R"(\\x80)"},                         // continuation byte alone
      {"\xE2\x82", R"(\\xe2\\x82)"},                // truncated at the end
      {"\xF0\x9F\x98", R"(\\xf0\\x9f\\x98)"},       // truncated at the end
      {"\xE2\xE2\x82\xAC", "\\\\xe2\xE2\x82\xAC"},  // truncated, then a valid sequence
  };
  for (const auto& [bytes, escaped] : ill_formed) {
    EXPECT_EQ(Json(bytes), JsonRow(escaped)) << escaped;
    EXPECT_EQ(Json("<" + bytes + ">"), JsonRow("<" + escaped + ">")) << escaped;
  }
}

TEST(FormatResultTest, JsonCopiesWellFormedUtf8) {
  const std::vector<std::string> well_formed = {
      "\x7F",              // U+007F
      "\xC2\x80",          // U+0080
      "\xDF\xBF",          // U+07FF
      "\xE0\xA0\x80",      // U+0800
      "\xED\x9F\xBF",      // U+D7FF
      "\xEE\x80\x80",      // U+E000
      "\xEF\xBF\xBF",      // U+FFFF
      "\xF0\x90\x80\x80",  // U+10000
      "\xF4\x8F\xBF\xBF",  // U+10FFFF
      "na\xC3\xAFve \xE2\x82\xAC \xF0\x9F\x98\x80",
  };
  for (const auto& bytes : well_formed) {
    EXPECT_EQ(Json(bytes), JsonRow(bytes));
    EXPECT_EQ(Json("<" + bytes + ">"), JsonRow("<" + bytes + ">"));
  }
}

}  // namespace
}  // namespace antb1::engine
