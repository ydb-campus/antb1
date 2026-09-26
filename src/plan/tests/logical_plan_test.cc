#include "antb1/plan/logical_plan.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <arrow/api.h>
#include <gtest/gtest.h>

#include "antb1/common/int128.h"
#include "antb1/plan/types.h"

#include "plan_test_util.h"

namespace antb1::plan {
namespace {

using testing::FakeTable;

TEST(LogicalPlanTest, OperatorAndAggregateNames) {
  EXPECT_EQ(ToString(CompareOp::kEq), "=");
  EXPECT_EQ(ToString(CompareOp::kNe), "<>");
  EXPECT_EQ(ToString(CompareOp::kLt), "<");
  EXPECT_EQ(ToString(CompareOp::kLe), "<=");
  EXPECT_EQ(ToString(CompareOp::kGt), ">");
  EXPECT_EQ(ToString(CompareOp::kGe), ">=");
  EXPECT_EQ(ToString(AggKind::kCountStar), "COUNT");
  EXPECT_EQ(ToString(AggKind::kCount), "COUNT");
  EXPECT_EQ(ToString(AggKind::kSum), "SUM");
  EXPECT_EQ(ToString(AggKind::kAvg), "AVG");
  EXPECT_EQ(ToString(AggKind::kMin), "MIN");
  EXPECT_EQ(ToString(AggKind::kMax), "MAX");
}

TEST(LogicalPlanTest, ConstantText) {
  EXPECT_EQ(ToString(Constant{.type = LogicalType::kSmallInt, .value = Int128{-7}}), "-7");
  EXPECT_EQ(ToString(Constant{.type = LogicalType::kHugeInt, .value = kInt128Min}),
            "-170141183460469231731687303715884105728");
  EXPECT_EQ(ToString(Constant{.type = LogicalType::kDate, .value = Int128{19000}}),
            "DATE '2022-01-08'");
  EXPECT_EQ(ToString(Constant{.type = LogicalType::kDate, .value = kInt128Max}),
            "DATE <170141183460469231731687303715884105727 days>");
  EXPECT_EQ(ToString(Constant{.type = LogicalType::kDouble, .value = 1.5}), "1.5");
  EXPECT_EQ(ToString(Constant{.type = LogicalType::kDouble, .value = 0.1}), "0.1");
  EXPECT_EQ(ToString(Constant{.type = LogicalType::kDouble, .value = -0.0}), "-0");
  EXPECT_EQ(ToString(Constant{.type = LogicalType::kDouble,
                              .value = std::numeric_limits<double>::infinity()}),
            "inf");
  EXPECT_EQ(ToString(Constant{.type = LogicalType::kVarchar, .value = std::string("O'Brien")}),
            "'O''Brien'");
  EXPECT_EQ(ToString(Constant{.type = LogicalType::kVarchar,
                              .value = std::string("a\tb\\c\x7f\xC3\xA9\0", 9)}),
            R"('a\x09b\x5Cc\x7F\xC3\xA9\x00')");
  EXPECT_EQ(ToString(Constant{.type = LogicalType::kVarchar, .value = std::string()}), "''");
}

TEST(LogicalPlanTest, ArrowScalars) {
  const auto scalar = [](const Constant& c) {
    auto s = ToArrowScalar(c);
    EXPECT_TRUE(s.ok()) << s.status().ToString();
    return s.ok() ? *s : nullptr;
  };
  auto s = scalar({.type = LogicalType::kSmallInt, .value = Int128{-32768}});
  ASSERT_NE(s, nullptr);
  EXPECT_TRUE(s->Equals(arrow::Int16Scalar(-32768)));
  s = scalar({.type = LogicalType::kInteger, .value = Int128{7}});
  ASSERT_NE(s, nullptr);
  EXPECT_TRUE(s->Equals(arrow::Int32Scalar(7)));
  s = scalar({.type = LogicalType::kBigInt, .value = Int128{std::numeric_limits<int64_t>::max()}});
  ASSERT_NE(s, nullptr);
  EXPECT_TRUE(s->Equals(arrow::Int64Scalar(std::numeric_limits<int64_t>::max())));
  s = scalar({.type = LogicalType::kUSmallInt, .value = Int128{65535}});
  ASSERT_NE(s, nullptr);
  EXPECT_TRUE(s->Equals(arrow::UInt16Scalar(65535)));
  s = scalar({.type = LogicalType::kDate, .value = Int128{15887}});
  ASSERT_NE(s, nullptr);
  EXPECT_TRUE(s->Equals(arrow::Date32Scalar(15887)));
  s = scalar({.type = LogicalType::kDouble, .value = -2.5});
  ASSERT_NE(s, nullptr);
  EXPECT_TRUE(s->Equals(arrow::DoubleScalar(-2.5)));
  s = scalar({.type = LogicalType::kVarchar, .value = std::string("x\0y", 3)});
  ASSERT_NE(s, nullptr);
  EXPECT_TRUE(s->Equals(arrow::BinaryScalar(arrow::Buffer::FromString(std::string("x\0y", 3)))));
  const Int128 big = -static_cast<Int128>(UInt128{1} << 100U);
  s = scalar({.type = LogicalType::kHugeInt, .value = big});
  ASSERT_NE(s, nullptr);
  ASSERT_TRUE(s->type->Equals(*arrow::decimal128(38, 0)));
  EXPECT_EQ(static_cast<const arrow::Decimal128Scalar&>(*s).value.ToIntegerString(),
            Int128ToString(big));
}

TEST(LogicalPlanTest, ArrowScalarErrors) {
  const auto invalid = [](const Constant& c) { return ToArrowScalar(c).status().IsInvalid(); };
  EXPECT_TRUE(invalid({.type = LogicalType::kSmallInt, .value = Int128{32768}}));
  EXPECT_TRUE(invalid({.type = LogicalType::kUSmallInt, .value = Int128{-1}}));
  EXPECT_TRUE(invalid({.type = LogicalType::kBigInt, .value = kInt128Max}));
  EXPECT_TRUE(invalid({.type = LogicalType::kHugeInt, .value = kInt128Max}));
  EXPECT_TRUE(
      invalid({.type = LogicalType::kDate, .value = Int128{1'099'511'627'776}}));  // 2^40 days
  EXPECT_TRUE(invalid({.type = LogicalType::kDouble, .value = Int128{1}}));
  EXPECT_TRUE(invalid({.type = LogicalType::kVarchar, .value = Int128{1}}));
  EXPECT_TRUE(invalid({.type = LogicalType::kBigInt, .value = 1.0}));
  EXPECT_TRUE(invalid({.type = LogicalType::kDouble, .value = std::string("1")}));
}

TEST(LogicalPlanTest, NodeNamesSpansAndInputs) {
  const auto table = std::make_shared<FakeTable>(testing::AllTypesSchema(), 1);
  const auto span = [](std::size_t offset) { return SourceSpan{.offset = offset, .length = 1}; };
  const LogicalNodePtr scan =
      std::make_shared<const LogicalNode>(ScanNode{.table = table, .span = span(1)});
  const std::vector<std::pair<LogicalNode, std::string_view>> nodes = {
      {ScanNode{.table = table, .span = span(1)}, "Scan"},
      {FilterNode{.input = scan, .span = span(2)}, "Filter"},
      {ProjectNode{.input = scan, .span = span(3)}, "Project"},
      {AggregateNode{.input = scan, .span = span(4)}, "Aggregate"},
      {LimitNode{.input = scan, .span = span(5)}, "Limit"},
      {RowCountNode{.table = table, .span = span(6)}, "RowCount"},
  };
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    const auto& [node, name] = nodes[i];
    EXPECT_EQ(NodeName(node), name);
    EXPECT_EQ(SpanOf(node), span(i + 1)) << name;
    const LogicalNodePtr* input = InputOf(node);
    const bool leaf = name == "Scan" || name == "RowCount";
    EXPECT_EQ(input == nullptr, leaf) << name;
    if (input != nullptr) {
      EXPECT_EQ(*input, scan) << name;
    }
  }
}

}  // namespace
}  // namespace antb1::plan
