#include "antb1/engine/format.h"

#include <memory>

#include <arrow/api.h>
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

  auto dates = Make<arrow::Date32Builder, int32_t>(arrow::date32(), {19000, 0, -1});
  EXPECT_EQ(FormatValue(*dates, 0, LogicalType::kDate), "2022-01-08");
  EXPECT_EQ(FormatValue(*dates, 1, LogicalType::kDate), "1970-01-01");
  EXPECT_EQ(FormatValue(*dates, 2, LogicalType::kDate), "1969-12-31");

  auto bin = Make<arrow::BinaryBuilder, std::string>(arrow::binary(), {"a,b", ""});
  EXPECT_EQ(FormatValue(*bin, 0, LogicalType::kVarchar), "a,b");
  EXPECT_EQ(FormatValue(*bin, 1, LogicalType::kVarchar), "");

  arrow::Decimal128Builder dec(arrow::decimal128(38, 0));
  ASSERT_TRUE(dec.Append(arrow::Decimal128("-170141183460469231731687303715884105728")).ok());
  auto decs = dec.Finish().ValueOrDie();
  EXPECT_EQ(FormatValue(*decs, 0, LogicalType::kHugeInt),
            "-170141183460469231731687303715884105728");
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

}  // namespace
}  // namespace antb1::engine
