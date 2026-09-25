#include "canonical.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <arrow/api.h>
#include <arrow/util/decimal.h>
#include <gtest/gtest.h>

#include "antb1/common/int128.h"
#include "antb1/engine/format.h"
#include "antb1/plan/types.h"

#include "engine.h"
#include "sha256.h"

namespace antb1::slt {
namespace {

using Row = std::vector<std::optional<std::string>>;

ResultSet Result(std::vector<ColumnClass> classes, std::vector<Row> rows) {
  return ResultSet{.classes = std::move(classes), .type_names = {}, .rows = std::move(rows)};
}

TEST(Sha256, MatchesFips180Vectors) {
  EXPECT_EQ(Sha256Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(Sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(Sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  EXPECT_EQ(Sha256Hex(std::string(1'000'000, 'a')),
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Canonical, Doubles) {
  EXPECT_EQ(CanonicalDouble(0.1), "0.1");
  EXPECT_EQ(CanonicalDouble(1.0 / 3.0), "0.3333333333333333");
  EXPECT_EQ(CanonicalDouble(1513.0), "1513");
  EXPECT_EQ(CanonicalDouble(-0.5), "-0.5");
  EXPECT_EQ(CanonicalDouble(1e300), "1e+300");
  EXPECT_EQ(CanonicalDouble(std::numeric_limits<double>::infinity()), "inf");
  EXPECT_EQ(CanonicalDouble(-std::numeric_limits<double>::infinity()), "-inf");
  EXPECT_EQ(CanonicalDouble(std::numeric_limits<double>::quiet_NaN()), "nan");
}

TEST(Canonical, Dates) {
  EXPECT_EQ(CanonicalDate(0), "1970-01-01");
  EXPECT_EQ(CanonicalDate(15'887), "2013-07-01");
  EXPECT_EQ(CanonicalDate(-1), "1969-12-31");
}

// One canonical formatter: engine::FormatValue (antb1) and the helpers the DuckDB adapter uses
// agree.
TEST(Canonical, AgreesWithEngineFormatValue) {
  arrow::DoubleBuilder doubles;
  ASSERT_TRUE(doubles.AppendValues({0.1, 1.0 / 3.0, 1513.0, -2.5e-300, 9.223372036854776e18}).ok());
  const auto d = doubles.Finish().ValueOrDie();
  for (int64_t i = 0; i < d->length(); ++i) {
    EXPECT_EQ(engine::FormatValue(*d, i, plan::LogicalType::kDouble),
              CanonicalDouble(std::static_pointer_cast<arrow::DoubleArray>(d)->Value(i)));
  }
  arrow::Date32Builder dates;
  ASSERT_TRUE(dates.AppendValues({0, 15'887, 15'917, -1}).ok());
  const auto dt = dates.Finish().ValueOrDie();
  for (int64_t i = 0; i < dt->length(); ++i) {
    EXPECT_EQ(engine::FormatValue(*dt, i, plan::LogicalType::kDate),
              CanonicalDate(std::static_pointer_cast<arrow::Date32Array>(dt)->Value(i)));
  }
  arrow::Int64Builder ints;
  ASSERT_TRUE(ints.AppendValues({std::numeric_limits<int64_t>::min(), -1, 0, 42}).ok());
  const auto in = ints.Finish().ValueOrDie();
  for (int64_t i = 0; i < in->length(); ++i) {
    EXPECT_EQ(engine::FormatValue(*in, i, plan::LogicalType::kBigInt),
              std::to_string(std::static_pointer_cast<arrow::Int64Array>(in)->Value(i)));
  }
  // HUGEINT (decimal128(38, 0)) vs the Int128 text the DuckDB adapter prints for HUGEINT.
  arrow::Decimal128Builder huge(arrow::decimal128(38, 0));
  const Int128 big = static_cast<Int128>(std::numeric_limits<int64_t>::max()) * 1000;
  ASSERT_TRUE(huge.Append(arrow::Decimal128(-12345)).ok());
  ASSERT_TRUE(huge.Append(arrow::Decimal128(static_cast<int64_t>(static_cast<UInt128>(big) >> 64U),
                                            static_cast<uint64_t>(big)))
                  .ok());
  const auto h = huge.Finish().ValueOrDie();
  EXPECT_EQ(engine::FormatValue(*h, 0, plan::LogicalType::kHugeInt), Int128ToString(-12345));
  EXPECT_EQ(engine::FormatValue(*h, 1, plan::LogicalType::kHugeInt), Int128ToString(big));
}

TEST(SltCell, NullEmptyAndEscapes) {
  EXPECT_EQ(SltCell(std::nullopt), "NULL");
  EXPECT_EQ(SltCell(""), "(empty)");
  EXPECT_EQ(SltCell("NULL"), "\\x4eULL");
  EXPECT_EQ(SltCell("(empty)"), "\\x28empty)");
  EXPECT_EQ(SltCell("a\tb\nc\rd\\e"), "a\\tb\\nc\\rd\\\\e");
  EXPECT_EQ(SltCell("x\x01y\x7f"), "x\\x01y\\x7f");
  EXPECT_EQ(SltCell(" padded  "), "\\x20padded \\x20");
  EXPECT_EQ(SltCell(" "), "\\x20");
  EXPECT_EQ(SltCell("in side"), "in side");
}

TEST(SltCell, KeepsValidUtf8AndEscapesInvalidBytes) {
  EXPECT_EQ(SltCell("Привет, 日本語 naïve"), "Привет, 日本語 naïve");
  EXPECT_EQ(SltCell("\xff"), "\\xff");
  EXPECT_EQ(SltCell("a\xc3"), "a\\xc3");                  // truncated sequence
  EXPECT_EQ(SltCell("\xc0\xaf"), "\\xc0\\xaf");           // overlong
  EXPECT_EQ(SltCell("\xed\xa0\x80"), "\\xed\\xa0\\x80");  // surrogate
  EXPECT_EQ(SltCell("\xf0\x9f\x98\x80"), "\xf0\x9f\x98\x80");
}

TEST(RenderBlock, RowsAndSortModes) {
  const auto r = Result({ColumnClass::kInteger, ColumnClass::kText},
                        {{"2", "b"}, {"10", std::nullopt}, {"1", ""}});
  EXPECT_EQ(RenderBlock(r, SortMode::kNoSort, 0),
            (std::vector<std::string>{"2\tb", "10\tNULL", "1\t(empty)"}));
  EXPECT_EQ(RenderBlock(r, SortMode::kRowSort, 0),
            (std::vector<std::string>{"1\t(empty)", "10\tNULL", "2\tb"}));
  EXPECT_EQ(RenderBlock(r, SortMode::kValueSort, 0),
            (std::vector<std::string>{"(empty)", "1", "10", "2", "NULL", "b"}));
}

TEST(RenderBlock, HashesAboveTheThresholdButNeverRealColumns) {
  const auto r = Result({ColumnClass::kInteger}, {{"1"}, {"2"}, {"3"}});
  EXPECT_EQ(RenderBlock(r, SortMode::kNoSort, 3).size(), 3U);
  const auto hashed = RenderBlock(r, SortMode::kNoSort, 2);
  ASSERT_EQ(hashed.size(), 1U);
  EXPECT_EQ(hashed[0], "3 values hashing to " + Sha256Hex("1\n2\n3\n"));
  EXPECT_TRUE(ParseHashLine(hashed[0]).has_value());
  const auto reals = Result({ColumnClass::kReal}, {{"1.5"}, {"2.5"}, {"3.5"}});
  EXPECT_EQ(RenderBlock(reals, SortMode::kNoSort, 2).size(), 3U);
}

TEST(CompareBlocks, ExactForIntegersAndText) {
  EXPECT_FALSE(
      CompareBlocks({"1\ta"}, {"1\ta"}, "IT", SortMode::kNoSort, kDefaultRelTolerance).has_value());
  const auto diff = CompareBlocks({"1\ta", "2\tb"}, {"1\ta", "2\tc"}, "IT", SortMode::kNoSort,
                                  kDefaultRelTolerance);
  ASSERT_TRUE(diff.has_value());
  EXPECT_EQ(diff.value_or(BlockDiff{}).first_row, 1U);
  const auto count = CompareBlocks({"1", "2"}, {"1"}, "I", SortMode::kNoSort, kDefaultRelTolerance);
  ASSERT_TRUE(count.has_value());
  EXPECT_EQ(count.value_or(BlockDiff{}).reason, "expected 2 rows, got 1");
  EXPECT_EQ(count.value_or(BlockDiff{}).first_row, 1U);
}

TEST(CompareBlocks, RealsUseTheTolerance) {
  EXPECT_FALSE(CompareBlocks({"1\t0.3333333333333333"}, {"1\t0.33333333333333337"}, "IR",
                             SortMode::kNoSort, kDefaultRelTolerance)
                   .has_value());
  EXPECT_TRUE(
      CompareBlocks({"1.0001"}, {"1"}, "R", SortMode::kNoSort, kDefaultRelTolerance).has_value());
  EXPECT_FALSE(CompareBlocks({"1.0001"}, {"1"}, "R", SortMode::kNoSort, 1e-3).has_value());
  EXPECT_FALSE(
      CompareBlocks({"0"}, {"1e-13"}, "R", SortMode::kNoSort, kDefaultRelTolerance).has_value());
  EXPECT_TRUE(
      CompareBlocks({"NULL"}, {"0"}, "R", SortMode::kNoSort, kDefaultRelTolerance).has_value());
  EXPECT_TRUE(CompareBlocks({"2\t1.5"}, {"3\t1.5"}, "IR", SortMode::kNoSort, kDefaultRelTolerance)
                  .has_value());
}

TEST(CompareBlocks, HashLinesCompareExactly) {
  const std::string a = "3 values hashing to " + Sha256Hex("a");
  const std::string b = "3 values hashing to " + Sha256Hex("b");
  EXPECT_FALSE(CompareBlocks({a}, {a}, "I", SortMode::kNoSort, kDefaultRelTolerance).has_value());
  const auto diff = CompareBlocks({a}, {b}, "I", SortMode::kNoSort, kDefaultRelTolerance);
  ASSERT_TRUE(diff.has_value());
  EXPECT_EQ(diff.value_or(BlockDiff{}).reason, "hashed results differ");
  EXPECT_FALSE(ParseHashLine("3 values hashing to xyz").has_value());
}

}  // namespace
}  // namespace antb1::slt
