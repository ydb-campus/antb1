#include "antb1/engine/format.h"

#include <chrono>
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

#include "antb1/engine/session.h"
#include "antb1/exec/profile.h"

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

  // TIMESTAMP as DuckDB prints it: whole seconds without a fraction, a fraction without trailing
  // zeros, the time of day forward from midnight before 1970.
  auto timestamps = Make<arrow::TimestampBuilder, int64_t>(
      arrow::timestamp(arrow::TimeUnit::MICRO),
      {1'373'896'800'000'000, 1'373'896'800'120'000, -61'000'000}, true);
  EXPECT_EQ(FormatValue(*timestamps, 0, LogicalType::kTimestamp), "2013-07-15 14:00:00");
  EXPECT_EQ(FormatValue(*timestamps, 1, LogicalType::kTimestamp), "2013-07-15 14:00:00.12");
  EXPECT_EQ(FormatValue(*timestamps, 2, LogicalType::kTimestamp), "1969-12-31 23:58:59");
  EXPECT_EQ(FormatValue(*timestamps, 3, LogicalType::kTimestamp), "NULL");
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
  EXPECT_EQ(FormatValue(*decs, 0, LogicalType::Decimal(38, 0)), "18446744073709551614");
}

QueryResult OneColumn(const std::shared_ptr<arrow::Array>& array, std::string name,
                      LogicalType type) {
  QueryResult r;
  r.table = arrow::Table::Make(arrow::schema({arrow::field(name, array->type())}), {array});
  r.names = {std::move(name)};
  r.types = {type};
  return r;
}

// A DECIMAL(p, s) shows s fraction digits, as DuckDB prints it, and is a JSON string like HUGEINT
// (ADR 0021 rule 15).
TEST(FormatValueTest, Decimals) {
  arrow::Decimal128Builder dec(arrow::decimal128(15, 2));
  for (const char* v : {"1700", "-25", "0", "999999999999999", "-999999999999999", "5"}) {
    ASSERT_TRUE(dec.Append(arrow::Decimal128(v)).ok());
  }
  ASSERT_TRUE(dec.AppendNull().ok());
  const auto decs = dec.Finish().ValueOrDie();
  const LogicalType type = LogicalType::Decimal(15, 2);
  const auto expected = std::to_array<std::string_view>(
      {"17.00", "-0.25", "0.00", "9999999999999.99", "-9999999999999.99", "0.05", "NULL"});
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(FormatValue(*decs, static_cast<int64_t>(i), type), expected[i]) << i;
  }
  arrow::Decimal128Builder fraction(arrow::decimal128(3, 3));
  ASSERT_TRUE(fraction.Append(arrow::Decimal128(500)).ok());
  ASSERT_TRUE(fraction.Append(arrow::Decimal128(-7)).ok());
  const auto fractions = fraction.Finish().ValueOrDie();
  EXPECT_EQ(FormatValue(*fractions, 0, LogicalType::Decimal(3, 3)), ".500");
  EXPECT_EQ(FormatValue(*fractions, 1, LogicalType::Decimal(3, 3)), "-.007");
  auto r = OneColumn(decs->Slice(0, 2), "p", type);
  EXPECT_EQ(*FormatResult(r, OutputFormat::kJson),
            "[\n {\"p\": \"17.00\"},\n {\"p\": \"-0.25\"}\n]\n");
  EXPECT_EQ(*FormatResult(r, OutputFormat::kCsv), "p\n17.00\n-0.25\n");
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

// A profile's metrics show in a fixed order, whatever order the threads recorded them in: a hash
// join's probe (find, gather, residual, window_rows) and build (parts, skipped, part_time, wait,
// lanes_tail, finish, null_keys, unique, direct) among the others, then the unknown ones by name;
// in the text and in the JSON.
TEST(FormatProfileTest, JoinMetricsShowInAFixedOrder) {
  constexpr int64_t kMs = 1000 * 1000;
  auto root = std::make_shared<exec::ProfileNode>();
  root->set_name("PartAggregate");
  root->set_detail("Aggregate COUNT(*)");
  root->AddRows(1);
  for (const char* name : {"merge", "wait", "part_time"}) {
    root->Add(name, exec::MetricUnit::kNanos, kMs);
  }
  root->Max("skipped", exec::MetricUnit::kCount, 0);
  root->Max("parts", exec::MetricUnit::kCount, 6);
  exec::ProfileNode& probe = *root->Child(0);
  probe.set_name("HashJoin");
  probe.set_detail("Join INNER build=right keys=[a = b]");
  probe.set_per_part(true);
  probe.Add("zeta", exec::MetricUnit::kCount, 1);
  probe.Add("window_rows", exec::MetricUnit::kCount, 8);
  for (const char* name : {"residual", "gather", "find"}) {
    probe.Add(name, exec::MetricUnit::kNanos, kMs);
  }
  probe.Add("alpha", exec::MetricUnit::kCount, 1);
  exec::ProfileNode& build = *root->Child(1);
  build.set_name("HashBuild");
  build.set_detail("Join INNER build=right keys=[a = b]");
  build.Max("direct", exec::MetricUnit::kCount, 0);
  build.Max("unique", exec::MetricUnit::kCount, 1);
  build.Add("null_keys", exec::MetricUnit::kCount, 2);
  for (const char* name : {"finish", "lanes_tail", "wait", "part_time"}) {
    build.Add(name, exec::MetricUnit::kNanos, kMs);
  }
  build.Max("skipped", exec::MetricUnit::kCount, 1);
  build.Max("parts", exec::MetricUnit::kCount, 3);
  const QueryProfile profile{.output = "Output: c:BIGINT",
                             .root = root,
                             .time = std::chrono::nanoseconds(0),
                             .rows = 1,
                             .peak_memory = 0,
                             .threads = 4};
  EXPECT_EQ(FormatProfile(profile, ProfileFormat::kText),
            "Output: c:BIGINT\n"
            "Total: time=0.000ms rows=1 peak_memory=0 bytes threads=4\n"
            "PartAggregate Aggregate COUNT(*)  [rows=1 batches=1 time=0.000ms self=0.000ms parts=6 "
            "skipped=0 part_time=1.000ms wait=1.000ms merge=1.000ms]\n"
            "  HashJoin Join INNER build=right keys=[a = b]  [rows=0 batches=0 parts=0 "
            "time=0.000ms (summed over parts) find=1.000ms gather=1.000ms residual=1.000ms "
            "window_rows=8 alpha=1 zeta=1]\n"
            "  HashBuild Join INNER build=right keys=[a = b]  [rows=0 batches=0 time=0.000ms "
            "parts=3 skipped=1 part_time=1.000ms wait=1.000ms lanes_tail=1.000ms finish=1.000ms "
            "null_keys=2 unique=1 direct=0]\n");
  const std::string json = FormatProfile(profile, ProfileFormat::kJson);
  EXPECT_NE(json.find(R"("metrics":{"find_ns":1000000,"gather_ns":1000000,"residual_ns":1000000,)"
                      R"("window_rows":8,"alpha":1,"zeta":1})"),
            std::string::npos)
      << json;
  EXPECT_NE(
      json.find(R"("metrics":{"parts":3,"skipped":1,"part_time_ns":1000000,"wait_ns":1000000,)"
                R"("lanes_tail_ns":1000000,"finish_ns":1000000,"null_keys":2,"unique":1,)"
                R"("direct":0})"),
      std::string::npos)
      << json;
}

}  // namespace
}  // namespace antb1::engine
