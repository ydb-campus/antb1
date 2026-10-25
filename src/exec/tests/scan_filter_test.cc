#include "antb1/exec/scan_filter.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/util/bit_util.h>
#include <gtest/gtest.h>

#include "antb1/common/int128.h"
#include "antb1/exec/filter.h"
#include "antb1/exec/key_filter.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/table.h"

#include "exec_test_util.h"

namespace antb1::exec {
namespace {

using Kind = plan::Predicate::Kind;
using plan::CompareOp;
using plan::LogicalType;
using testing::Column;
using testing::Int64s;
using testing::Strings;

constexpr std::array<CompareOp, 6> kOps = {CompareOp::kEq, CompareOp::kNe, CompareOp::kLt,
                                           CompareOp::kLe, CompareOp::kGt, CompareOp::kGe};

class ScanFilterTest : public testing::ExecTest {
 protected:
  // Columns: b BIGINT, i INTEGER, d DOUBLE, s VARCHAR, n VARCHAR (no NULL), with NULLs, extremes,
  // NaN and signed zeros, and strings with bytes above 0x7f (compared unsigned).
  static std::shared_ptr<arrow::RecordBatch> Batch() {
    const std::vector<std::optional<int64_t>> b = {0,
                                                   1,
                                                   -1,
                                                   std::nullopt,
                                                   std::numeric_limits<int64_t>::min(),
                                                   std::numeric_limits<int64_t>::max(),
                                                   2,
                                                   2,
                                                   std::nullopt,
                                                   -7,
                                                   5,
                                                   3,
                                                   1};
    const std::vector<std::optional<std::string>> s = {
        "",    "a", "ab",         std::nullopt, "abc", "b", "\xff", "\xc3\xa9t\xc3\xa9",
        "a%b", "_", std::nullopt, "abcabc",     "bc"};
    const auto rows = static_cast<int64_t>(b.size());
    arrow::Int32Builder i;
    arrow::DoubleBuilder d;
    arrow::BinaryBuilder n;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const std::vector<std::optional<double>> doubles = {
        0.0, -0.0, 1.5, std::nullopt, nan, inf, -inf, 2.0, 1.5, std::nullopt, -1.0, 3.0, nan};
    for (int64_t r = 0; r < rows; ++r) {
      EXPECT_TRUE((r % 5 == 4 ? i.AppendNull() : i.Append(static_cast<int32_t>(r % 4) - 1)).ok());
      const auto& v = doubles[static_cast<std::size_t>(r)];
      EXPECT_TRUE((v.has_value() ? d.Append(*v) : d.AppendNull()).ok());
      EXPECT_TRUE(n.Append(std::string(static_cast<std::size_t>(r % 3), 'a')).ok());
    }
    const auto schema =
        arrow::schema({arrow::field("b", arrow::int64()), arrow::field("i", arrow::int32()),
                       arrow::field("d", arrow::float64()), arrow::field("s", arrow::binary()),
                       arrow::field("n", arrow::binary())});
    return arrow::RecordBatch::Make(schema, rows,
                                    {Int64s(b), i.Finish().ValueOrDie(), d.Finish().ValueOrDie(),
                                     Strings(s), n.Finish().ValueOrDie()});
  }

  static plan::BoundColumn B() { return Column(0, "b", LogicalType::kBigInt); }
  static plan::BoundColumn I() { return Column(1, "i", LogicalType::kInteger); }
  static plan::BoundColumn D() { return Column(2, "d", LogicalType::kDouble); }
  static plan::BoundColumn S() { return Column(3, "s", LogicalType::kVarchar); }
  static plan::BoundColumn N() { return Column(4, "n", LogicalType::kVarchar); }

  static plan::Constant Text(std::string bytes) {
    return plan::Constant{.type = LogicalType::kVarchar, .value = std::move(bytes)};
  }
  static plan::Constant Integer(int64_t v) {
    return plan::Constant{.type = LogicalType::kInteger, .value = Int128{v}};
  }
  static plan::Constant Double(double v) {
    return plan::Constant{.type = LogicalType::kDouble, .value = v};
  }

  static plan::Predicate Of(Kind kind, plan::BoundColumn column) {
    return plan::Predicate{.kind = kind, .column = std::move(column), .span = {}};
  }
  static plan::Predicate Like(plan::BoundColumn column, std::string pattern, bool negated) {
    plan::Predicate p = Of(negated ? Kind::kNotLike : Kind::kLike, std::move(column));
    p.constant = Text(std::move(pattern));
    return p;
  }
  static plan::Predicate In(plan::BoundColumn column, std::vector<plan::Constant> values,
                            bool negated) {
    plan::Predicate p = Of(negated ? Kind::kNotIn : Kind::kIn, std::move(column));
    p.values = std::move(values);
    return p;
  }

  // The rows the Filter operator keeps: every predicate valid and true.
  static std::vector<bool> Expected(const std::vector<plan::Predicate>& predicates,
                                    const arrow::RecordBatch& batch) {
    std::vector<bool> keep(static_cast<std::size_t>(batch.num_rows()), true);
    for (const plan::Predicate& p : predicates) {
      auto evaluator = PredicateEvaluator::Make(p, *batch.schema());
      EXPECT_TRUE(evaluator.ok()) << evaluator.status().ToString();
      if (!evaluator.ok()) {
        return {};
      }
      auto result = evaluator->Evaluate(batch, arrow::default_memory_pool(), /*kleene=*/false);
      EXPECT_TRUE(result.ok()) << result.status().ToString();
      if (!result.ok()) {
        return {};
      }
      const auto array =
          result->is_array()
              ? result->make_array()
              : arrow::MakeArrayFromScalar(*result->scalar(), batch.num_rows()).ValueOrDie();
      const auto& mask = static_cast<const arrow::BooleanArray&>(*array);
      for (int64_t r = 0; r < batch.num_rows(); ++r) {
        if (mask.IsNull(r) || !mask.Value(r)) {
          keep[static_cast<std::size_t>(r)] = false;
        }
      }
    }
    return keep;
  }

  // The rows the scan filter keeps, applied as a table applies it: fixed-width columns whole,
  // VARCHAR ones as views in pieces of `piece` rows, the selection starting at bit `shift`.
  static std::vector<bool> Applied(const plan::ScanFilter& filter, const arrow::RecordBatch& batch,
                                   int64_t piece, int64_t shift) {
    const int64_t rows = batch.num_rows();
    std::vector<std::uint8_t> selected(static_cast<std::size_t>((shift + rows + 7) / 8), 0);
    arrow::bit_util::SetBitsTo(selected.data(), shift, rows, true);
    for (std::size_t k = 0; k < filter.columns().size(); ++k) {
      const int column = static_cast<int>(k);
      const auto& values = batch.column(filter.columns()[k]);
      if (values->type_id() != arrow::Type::BINARY) {
        const auto status = filter.Apply(
            column,
            plan::ScanValues{.array = values, .strings = {}, .validity = nullptr, .rows = rows},
            shift, selected.data());
        EXPECT_TRUE(status.ok()) << status.ToString();
        continue;
      }
      const auto& strings = static_cast<const arrow::BinaryArray&>(*values);
      for (int64_t start = 0; start < rows; start += piece) {
        const int64_t count = std::min(piece, rows - start);
        std::vector<std::string_view> views;
        std::vector<std::uint8_t> valid(static_cast<std::size_t>((count + 7) / 8), 0);
        for (int64_t r = 0; r < count; ++r) {
          // A NULL row's view is garbage on purpose: only the validity may decide.
          views.push_back(strings.IsNull(start + r) ? std::string_view("a")
                                                    : strings.GetView(start + r));
          if (strings.IsValid(start + r)) {
            arrow::bit_util::SetBit(valid.data(), r);
          }
        }
        const auto status = filter.Apply(
            column,
            plan::ScanValues{.array = nullptr,
                             .strings = std::span<const std::string_view>(views),
                             .validity = strings.null_count() > 0 ? valid.data() : nullptr,
                             .rows = count},
            shift + start, selected.data());
        EXPECT_TRUE(status.ok()) << status.ToString();
      }
    }
    std::vector<bool> keep;
    keep.reserve(static_cast<std::size_t>(rows));
    for (int64_t r = 0; r < rows; ++r) {
      keep.push_back(
          arrow::bit_util::GetBit(selected.data(), static_cast<std::uint64_t>(shift + r)));
    }
    return keep;
  }

  static void ExpectSame(const std::vector<plan::Predicate>& predicates, const std::string& what) {
    const auto batch = Batch();
    const auto filter = MakeScanFilter(predicates, *batch->schema(), arrow::default_memory_pool());
    ASSERT_TRUE(filter.ok()) << what << ": " << filter.status().ToString();
    const std::vector<bool> expected = Expected(predicates, *batch);
    for (const int64_t piece : {int64_t{1}, int64_t{3}, int64_t{64}}) {
      for (const int64_t shift : {int64_t{0}, int64_t{5}}) {
        EXPECT_EQ(Applied(**filter, *batch, piece, shift), expected)
            << what << " piece=" << piece << " shift=" << shift;
      }
    }
  }
};

// Two filters over one scan keep exactly the rows one filter over all of their predicates keeps,
// however a table feeds the columns in (every piece size and bit offset the harness drives). That
// is the property ADR 0029 deferred as "a plan::ScanFilter that is the conjunction of two", and it
// holds because a ScanFilter only ever *clears* bits: neither filter has to know what the other
// did, and the order they run in cannot matter.
//
// The pair is built from predicates here rather than from a key filter so that one expectation
// covers both -- the rows a key filter keeps are a superset of a join's, which no predicate list
// can state -- and MakeBothScanFilters does not know what it is composing.
TEST_F(ScanFilterTest, TheConjunctionOfTwoKeepsWhatBothKeep) {
  const auto batch = Batch();
  const std::vector<std::vector<plan::Predicate>> halves = {
      {testing::Compare(B(), CompareOp::kGe, testing::BigInt(1))},
      {testing::Compare(B(), CompareOp::kLe, testing::BigInt(7))},
      {Like(S(), "a%", /*negated=*/false)},
      {testing::Compare(I(), CompareOp::kNe, Integer(3))},
  };
  for (std::size_t a = 0; a < halves.size(); ++a) {
    for (std::size_t b = 0; b < halves.size(); ++b) {
      if (a == b) {
        continue;
      }
      const std::string what = std::format("halves {} and {}", a, b);
      auto first = MakeScanFilter(halves[a], *batch->schema(), arrow::default_memory_pool());
      ASSERT_TRUE(first.ok()) << what << ": " << first.status().ToString();
      auto second = MakeScanFilter(halves[b], *batch->schema(), arrow::default_memory_pool());
      ASSERT_TRUE(second.ok()) << what << ": " << second.status().ToString();
      auto both = MakeBothScanFilters(*first, *second);
      ASSERT_TRUE(both.ok()) << what << ": " << both.status().ToString();

      // **Each column once**, which plan::ScanFilter requires and io's filtered scan enforces:
      // concatenating the two lists repeated any column they both read -- a predicate on a column
      // that is also the join's key, the ordinary case -- and every such scan failed to open.
      std::vector<int> seen = (*both)->columns();
      std::ranges::sort(seen);
      EXPECT_EQ(std::ranges::unique(seen).begin(), seen.end()) << what << ": a column twice";
      for (const int column : (*first)->columns()) {
        EXPECT_NE(std::ranges::find((*both)->columns(), column), (*both)->columns().end()) << what;
      }
      for (const int column : (*second)->columns()) {
        EXPECT_NE(std::ranges::find((*both)->columns(), column), (*both)->columns().end()) << what;
      }

      std::vector<plan::Predicate> all = halves[a];
      all.insert(all.end(), halves[b].begin(), halves[b].end());
      const std::vector<bool> expected = Expected(all, *batch);
      for (const int64_t piece : {int64_t{1}, int64_t{3}, int64_t{64}}) {
        for (const int64_t shift : {int64_t{0}, int64_t{5}}) {
          EXPECT_EQ(Applied(**both, *batch, piece, shift), expected)
              << what << " piece=" << piece << " shift=" << shift;
        }
      }
    }
  }
  // Both are required: a conjunction of one is a programming error, not a filter.
  auto one = MakeScanFilter(halves[0], *batch->schema(), arrow::default_memory_pool());
  ASSERT_TRUE(one.ok());
  EXPECT_TRUE(MakeBothScanFilters(*one, nullptr).status().IsInvalid());
  EXPECT_TRUE(MakeBothScanFilters(nullptr, *one).status().IsInvalid());

  // An offset outside the selection is refused rather than written through. The key filter is the
  // one that computes a bit from it, and `offset + row` either side of the range would clear a bit
  // outside the mask it was handed -- below it when negative, and when the sum wraps.
  auto built = KeyFilter::Make(/*num_keys=*/8, /*budget=*/nullptr);
  ASSERT_TRUE(built.ok()) << built.status().ToString();
  auto keys = MakeKeyScanFilter(*built, 0);
  ASSERT_TRUE(keys.ok()) << keys.status().ToString();
  std::vector<std::uint8_t> mask(8, 0xFF);
  const plan::ScanValues values{
      .array = batch->column(0), .strings = {}, .validity = nullptr, .rows = batch->num_rows()};
  EXPECT_TRUE((*keys)->Apply(0, values, -1, mask.data()).IsInvalid());
  EXPECT_TRUE(
      (*keys)->Apply(0, values, std::numeric_limits<int64_t>::max(), mask.data()).IsInvalid());
}

// Every pushed predicate kind, on every column type and NULL pattern, keeps exactly the rows the
// Filter operator keeps.
TEST_F(ScanFilterTest, KeepsTheRowsTheFilterOperatorKeeps) {
  for (const CompareOp op : kOps) {
    const std::string name = "op " + std::to_string(static_cast<int>(op));
    ExpectSame({testing::Compare(B(), op, testing::BigInt(1))}, name + " BIGINT");
    ExpectSame({testing::Compare(B(), op, testing::BigInt(std::numeric_limits<int64_t>::min()))},
               name + " BIGINT min");
    ExpectSame({testing::Compare(I(), op, Integer(0))}, name + " INTEGER");
    ExpectSame({testing::Compare(D(), op, Double(1.5))}, name + " DOUBLE");
    ExpectSame({testing::Compare(D(), op, Double(-0.0))}, name + " DOUBLE -0");
    ExpectSame({testing::Compare(D(), op, Double(std::numeric_limits<double>::quiet_NaN()))},
               name + " DOUBLE NaN");
    for (const std::string& c : {std::string(), std::string("ab"), std::string("\xff"),
                                 std::string("b"), std::string("a")}) {
      ExpectSame({testing::Compare(S(), op, Text(c))}, std::format("{} VARCHAR {}", name, c));
      ExpectSame({testing::Compare(N(), op, Text(c))},
                 std::format("{} VARCHAR without NULLs {}", name, c));
    }
  }
  for (const bool negated : {false, true}) {
    const std::string name = negated ? "NOT " : "";
    for (const std::string& pattern :
         {std::string("a%"), std::string("%b%"), std::string("_"), std::string("%c"),
          std::string("a_c"), std::string("%"), std::string(), std::string("\xff%"),
          std::string("_t_"), std::string("a%b%c")}) {
      ExpectSame({Like(S(), pattern, negated)}, std::format("{}LIKE {}", name, pattern));
      ExpectSame({Like(N(), pattern, negated)},
                 std::format("{}LIKE without NULLs {}", name, pattern));
    }
    ExpectSame({In(S(), {Text("a"), Text("\xff"), Text("b")}, negated)}, name + "IN VARCHAR");
    ExpectSame({In(S(), {Text("zzz")}, negated)}, name + "IN VARCHAR none");
    ExpectSame({In(N(), {Text(""), Text("aa")}, negated)}, name + "IN VARCHAR without NULLs");
    ExpectSame({In(B(), {testing::BigInt(2), testing::BigInt(-7)}, negated)}, name + "IN BIGINT");
    ExpectSame({In(I(), {Integer(0), Integer(-1)}, negated)}, name + "IN INTEGER");
    ExpectSame({In(D(), {Double(1.5), Double(std::numeric_limits<double>::quiet_NaN())}, negated)},
               name + "IN DOUBLE");
  }
  for (const plan::BoundColumn& c : {B(), I(), D(), S(), N()}) {
    ExpectSame({Of(Kind::kIsNotNull, c)}, "IS NOT NULL " + c.name);
  }
  // Conjunctions: several predicates on one column, and on several columns.
  ExpectSame({Like(S(), "a%", false), Like(S(), "%c", true),
              testing::Compare(B(), CompareOp::kGe, testing::BigInt(0))},
             "conjunction");
  ExpectSame({testing::Compare(D(), CompareOp::kGt, Double(0.0)), Of(Kind::kIsNotNull, I()),
              In(S(), {Text("ab"), Text("abc")}, true)},
             "conjunction over three columns");
}

// A DOUBLE constant or IN list on a DECIMAL column compares in DOUBLE (ADR 0021 rule 11), also
// pushed into a scan (a Parquet table never does: a scan that reads a DECIMAL column is not
// filtered, but a test table pushes predicates on every type): 9007199254740993.5 is 2^53 there.
TEST_F(ScanFilterTest, DecimalsComparedInDouble) {
  arrow::Decimal128Builder builder(arrow::decimal128(38, 10));
  const Int128 scale = 10'000'000'000;
  for (const std::optional<Int128>& v :
       {std::optional<Int128>(Int128{90071992547409935} * 1'000'000'000), std::optional<Int128>{},
        std::optional<Int128>(Int128{15} * 1'000'000'000), std::optional<Int128>(-scale),
        std::optional<Int128>(Int128{0})}) {
    const auto bits = static_cast<UInt128>(v.value_or(0));
    const arrow::Decimal128 d(static_cast<int64_t>(bits >> 64U), static_cast<uint64_t>(bits));
    ASSERT_TRUE((v.has_value() ? builder.Append(d) : builder.AppendNull()).ok());
  }
  const auto values = builder.Finish().ValueOrDie();
  const auto batch = arrow::RecordBatch::Make(
      arrow::schema({arrow::field("w", arrow::decimal128(38, 10))}), values->length(), {values});
  const auto w = Column(0, "w", LogicalType::Decimal(38, 10));
  const auto same_as_filter = [&](const plan::Predicate& p, const std::vector<bool>& expected,
                                  const std::string& what) {
    const auto filter = MakeScanFilter({p}, *batch->schema(), arrow::default_memory_pool());
    ASSERT_TRUE(filter.ok()) << what << ": " << filter.status().ToString();
    EXPECT_EQ(Expected({p}, *batch), expected) << what;
    for (const int64_t shift : {int64_t{0}, int64_t{3}}) {
      EXPECT_EQ(Applied(**filter, *batch, /*piece=*/64, shift), expected) << what;
    }
  };
  same_as_filter(testing::Compare(w, CompareOp::kEq, Double(0x1p53)),
                 {true, false, false, false, false}, "= 2^53");
  same_as_filter(testing::Compare(w, CompareOp::kLt, Double(1.5)),
                 {false, false, false, true, true}, "< 1.5");
  same_as_filter(In(w, {Double(0x1p53), Double(-1.0)}, false), {true, false, false, true, false},
                 "IN");
  same_as_filter(In(w, {Double(0x1p53), Double(-1.0)}, true), {false, false, true, false, true},
                 "NOT IN");
}

// columns() lists each column a predicate reads once, cheapest work per row removed first -- not in
// the order the predicates were given. Here a BIGINT column (decode, keeping a third of the rows)
// outranks a VARCHAR column asked for a LIKE and an IS NOT NULL, even though the VARCHAR one is
// named first (ADR 0020's predicate ordering).
TEST_F(ScanFilterTest, ReadsEachColumnOnceCheapestFirst) {
  const auto batch = Batch();
  const auto filter = MakeScanFilter(
      {Like(S(), "a%", false), testing::Compare(B(), CompareOp::kGt, testing::BigInt(0)),
       Of(Kind::kIsNotNull, S())},
      *batch->schema(), arrow::default_memory_pool());
  ASSERT_TRUE(filter.ok()) << filter.status().ToString();
  EXPECT_EQ((*filter)->columns(), (std::vector<int>{0, 3}));
}

// And the order does not depend on the order the query wrote its conjuncts in, which is the whole
// point: the same filter spelled either way reads its columns in the same order. Spelled with the
// VARCHAR IN first, this cost 2.486 s against 1.091 s the other way round over 600 million rows
// (ADR 0020), because the scan applied the columns as the WHERE clause named them.
TEST_F(ScanFilterTest, TheOrderDoesNotDependOnHowTheQueryWroteIt) {
  const auto batch = Batch();
  const std::vector<plan::Predicate> cheap_first = {
      testing::Compare(B(), CompareOp::kGt, testing::BigInt(0)),
      In(S(), {Text("a"), Text("b")}, false)};
  const std::vector<plan::Predicate> dear_first = {cheap_first[1], cheap_first[0]};
  const auto order = [&](const std::vector<plan::Predicate>& predicates) {
    auto filter = MakeScanFilter(predicates, *batch->schema(), arrow::default_memory_pool());
    EXPECT_TRUE(filter.ok()) << filter.status().ToString();
    return filter.ok() ? (*filter)->columns() : std::vector<int>{};
  };
  EXPECT_EQ(order(cheap_first), (std::vector<int>{0, 3}));
  EXPECT_EQ(order(dear_first), (std::vector<int>{0, 3})) << "the spelling decided the order";
}

// Two columns of equal rank keep the order the query wrote: a plan may depend on the query and the
// metadata and on nothing else, so the tie-break has to be something the query fixes.
TEST_F(ScanFilterTest, TwoColumnsOfOneCostGoByWhatTheyRemove) {
  const auto batch = Batch();
  // Both fixed-width, so both cost their decode and nothing else: only selectivity is left to
  // decide, and an equality is assumed to remove more than a range bound.
  const auto filter = MakeScanFilter({testing::Compare(I(), CompareOp::kGt, Integer(0)),
                                      testing::Compare(B(), CompareOp::kEq, testing::BigInt(0))},
                                     *batch->schema(), arrow::default_memory_pool());
  ASSERT_TRUE(filter.ok()) << filter.status().ToString();
  EXPECT_EQ((*filter)->columns(), (std::vector<int>{0, 1}));
}

TEST_F(ScanFilterTest, AnInListOfSeveralValuesCostsOnePerValue) {
  const auto batch = Batch();
  // Two VARCHAR columns, each read once: three values compared cost more than one pattern matched,
  // enough to outweigh the pattern being assumed the more selective of the two.
  const auto filter =
      MakeScanFilter({In(S(), {Text("a"), Text("b"), Text("c")}, false), Like(N(), "a%", false)},
                     *batch->schema(), arrow::default_memory_pool());
  ASSERT_TRUE(filter.ok()) << filter.status().ToString();
  EXPECT_EQ((*filter)->columns(), (std::vector<int>{4, 3}));
}

TEST_F(ScanFilterTest, APatternMatchCostsMoreThanTwoValuesCompared) {
  const auto batch = Batch();
  // The other side of the same coin: a pattern match is dearer per row than two values compared,
  // by enough to outweigh the pattern being assumed the more selective. Written pattern-first, so
  // only the cost of the kinds can produce the expected order.
  const auto filter =
      MakeScanFilter({Like(S(), "a%", false), In(N(), {Text("a"), Text("b")}, false)},
                     *batch->schema(), arrow::default_memory_pool());
  ASSERT_TRUE(filter.ok()) << filter.status().ToString();
  EXPECT_EQ((*filter)->columns(), (std::vector<int>{4, 3}));
}

// The next two cases are the ones where the cost decides against the selectivity, so neither can
// come out right by accident: a fixed-width column goes first although it removes far less, and a
// VARCHAR column's second predicate adds its own weight rather than riding along with the first.
TEST_F(ScanFilterTest, AFixedWidthColumnGoesFirstEvenWhenItRemovesLittle) {
  const auto batch = Batch();
  const auto filter = MakeScanFilter({testing::Compare(S(), CompareOp::kLt, Text("a")),
                                      testing::Compare(B(), CompareOp::kNe, testing::BigInt(0)),
                                      testing::Compare(B(), CompareOp::kNe, testing::BigInt(1))},
                                     *batch->schema(), arrow::default_memory_pool());
  ASSERT_TRUE(filter.ok()) << filter.status().ToString();
  EXPECT_EQ((*filter)->columns(), (std::vector<int>{0, 3}))
      << "a column that removes a fifth of the rows for nothing still goes first";
}

TEST_F(ScanFilterTest, ASecondPredicateOnAColumnAddsItsOwnCost) {
  const auto batch = Batch();
  const auto filter = MakeScanFilter({testing::Compare(S(), CompareOp::kGt, Text("a")),
                                      testing::Compare(S(), CompareOp::kLt, Text("b")),
                                      testing::Compare(N(), CompareOp::kEq, Text("a"))},
                                     *batch->schema(), arrow::default_memory_pool());
  ASSERT_TRUE(filter.ok()) << filter.status().ToString();
  EXPECT_EQ((*filter)->columns(), (std::vector<int>{4, 3}))
      << "two predicates on one column were weighed as one";
}

TEST_F(ScanFilterTest, AFixedWidthColumnsPredicateCountDoesNotMoveIt) {
  const auto batch = Batch();
  // A two-sided range and an equality are assumed to remove the same share, and a fixed-width
  // column's predicates are free, so these two tie and the query's order stands. Were the second
  // predicate on the first column billed anything, it would sink below the one-predicate column.
  const auto filter = MakeScanFilter({testing::Compare(B(), CompareOp::kGt, testing::BigInt(0)),
                                      testing::Compare(B(), CompareOp::kLt, testing::BigInt(5)),
                                      testing::Compare(I(), CompareOp::kEq, Integer(0))},
                                     *batch->schema(), arrow::default_memory_pool());
  ASSERT_TRUE(filter.ok()) << filter.status().ToString();
  EXPECT_EQ((*filter)->columns(), (std::vector<int>{0, 1}));
}

TEST_F(ScanFilterTest, ColumnsOfEqualRankKeepTheOrderTheQueryGave) {
  const auto batch = Batch();
  // Two VARCHAR columns asked the same thing: equal cost and equal assumed selectivity.
  const auto both = [&](const plan::BoundColumn& first, const plan::BoundColumn& second) {
    auto filter = MakeScanFilter({testing::Compare(first, CompareOp::kGt, Text("a")),
                                  testing::Compare(second, CompareOp::kGt, Text("a"))},
                                 *batch->schema(), arrow::default_memory_pool());
    EXPECT_TRUE(filter.ok()) << filter.status().ToString();
    return filter.ok() ? (*filter)->columns() : std::vector<int>{};
  };
  EXPECT_EQ(both(S(), N()), (std::vector<int>{3, 4}));
  EXPECT_EQ(both(N(), S()), (std::vector<int>{4, 3}));
}

TEST_F(ScanFilterTest, PushesSingleColumnComparisonsWithConstants) {
  for (const Kind kind :
       {Kind::kCompare, Kind::kIn, Kind::kNotIn, Kind::kLike, Kind::kNotLike, Kind::kIsNotNull}) {
    EXPECT_TRUE(PushableToScan(Of(kind, S()))) << static_cast<int>(kind);
  }
  plan::Predicate columns = Of(Kind::kCompareColumns, B());
  columns.other = I();
  EXPECT_FALSE(PushableToScan(columns));
  EXPECT_FALSE(PushableToScan(Of(Kind::kIsTrue, B())));
  EXPECT_FALSE(PushableToScan(Of(Kind::kFalse, B())));
  EXPECT_FALSE(PushableToScan(plan::Predicate{.kind = Kind::kFalse, .span = {}}));
  EXPECT_FALSE(PushableToScan(plan::Predicate{.kind = Kind::kCompare, .span = {}}))
      << "without a column";
}

// Malformed predicates fail as the Filter operator's do; so do values a table should not pass.
TEST_F(ScanFilterTest, RejectsMalformedPredicatesAndValues) {
  const auto batch = Batch();
  const auto make = [&](const std::vector<plan::Predicate>& predicates) {
    return MakeScanFilter(predicates, *batch->schema(), arrow::default_memory_pool()).status();
  };
  plan::Predicate columns = Of(Kind::kCompareColumns, B());
  columns.other = I();
  EXPECT_TRUE(make({columns}).IsInvalid());
  EXPECT_TRUE(make({Of(Kind::kFalse, B())}).IsInvalid());
  EXPECT_TRUE(make({Of(Kind::kIsNotNull, Column(5, "x", LogicalType::kBigInt))}).IsInvalid());
  EXPECT_TRUE(make({Of(Kind::kIsNotNull, Column(-1, "x", LogicalType::kBigInt))}).IsInvalid());
  EXPECT_TRUE(make({Like(B(), "a%", false)}).IsInvalid()) << "LIKE of a BIGINT";
  EXPECT_TRUE(make({testing::Compare(S(), CompareOp::kEq, testing::BigInt(1))}).IsInvalid());
  EXPECT_TRUE(make({testing::Compare(B(), CompareOp::kEq, Text("1"))}).IsInvalid());
  EXPECT_TRUE(make({In(S(), {}, false)}).IsInvalid());
  EXPECT_TRUE(make({In(S(), {Text("a"), testing::BigInt(1)}, false)}).IsInvalid());
  plan::Predicate like = Like(S(), "", false);
  like.constant = testing::BigInt(1);
  EXPECT_TRUE(make({like}).IsInvalid()) << "a LIKE pattern that is not VARCHAR";

  const auto filter = MakeScanFilter({Of(Kind::kIsNotNull, S()), Of(Kind::kIsNotNull, B())},
                                     *batch->schema(), arrow::default_memory_pool());
  ASSERT_TRUE(filter.ok()) << filter.status().ToString();
  std::uint8_t selected = 0xff;
  const std::vector<std::string_view> views = {"a", "b"};
  const plan::ScanValues strings{.array = nullptr,
                                 .strings = std::span<const std::string_view>(views),
                                 .validity = nullptr,
                                 .rows = 2};
  EXPECT_TRUE((*filter)->Apply(2, strings, 0, &selected).IsInvalid()) << "no third column";
  EXPECT_TRUE((*filter)->Apply(-1, strings, 0, &selected).IsInvalid());
  EXPECT_TRUE((*filter)->Apply(1, strings, 0, &selected).IsInvalid()) << "views of BIGINT";
  const plan::ScanValues ints{
      .array = Int64s({1, 2}), .strings = {}, .validity = nullptr, .rows = 2};
  EXPECT_TRUE((*filter)->Apply(0, ints, 0, &selected).IsInvalid()) << "an array of VARCHAR";
  const plan::ScanValues short_views{.array = nullptr,
                                     .strings = std::span<const std::string_view>(views),
                                     .validity = nullptr,
                                     .rows = 3};
  EXPECT_TRUE((*filter)->Apply(0, short_views, 0, &selected).IsInvalid()) << "too few views";
  const plan::ScanValues short_ints{
      .array = Int64s({1, 2}), .strings = {}, .validity = nullptr, .rows = 3};
  EXPECT_TRUE((*filter)->Apply(1, short_ints, 0, &selected).IsInvalid()) << "too few values";
  const plan::ScanValues other_type{
      .array = Strings({"1", "2"}), .strings = {}, .validity = nullptr, .rows = 2};
  EXPECT_TRUE((*filter)->Apply(1, other_type, 0, &selected).IsInvalid()) << "another type";
  EXPECT_EQ(selected, 0xff) << "a failed call clears nothing";
}

// The second shape plan::ScanValues allows for a VARCHAR column: a dictionary and one index a row,
// with **no views at all**. That is what a column no batch carries is read as -- its views would
// exist only to be compared -- and it is admissible only where the dictionary is certain to answer
// the predicate, because the per-row path would otherwise read values nobody wrote.
TEST_F(ScanFilterTest, AVarcharColumnMayArriveAsADictionaryWithNoViews) {
  const auto filter = MakeScanFilter({testing::Compare(S(), CompareOp::kEq, Text("a"))},
                                     *Batch()->schema(), arrow::default_memory_pool());
  ASSERT_TRUE(filter.ok()) << filter.status().ToString();
  const std::vector<std::string_view> dictionary = {"a", "b", "c"};
  const std::vector<std::int32_t> indices = {0, 1, 0, 2};

  // No views, a dictionary smaller than the piece: accepted, and it answers.
  std::uint8_t selected = 0x0f;
  const plan::ScanValues by_dictionary{.array = nullptr,
                                       .strings = {},
                                       .validity = nullptr,
                                       .rows = 4,
                                       .dictionary = std::span<const std::string_view>(dictionary),
                                       .indices = std::span<const std::int32_t>(indices)};
  ASSERT_TRUE((*filter)->Apply(0, by_dictionary, 0, &selected).ok());
  EXPECT_EQ(selected, 0x05) << "rows 0 and 2 hold 'a'";

  // Neither shape: Invalid rather than a read of nothing.
  selected = 0x0f;
  const plan::ScanValues neither{.array = nullptr, .strings = {}, .validity = nullptr, .rows = 4};
  EXPECT_TRUE((*filter)->Apply(0, neither, 0, &selected).IsInvalid()) << "no views, no dictionary";
  EXPECT_EQ(selected, 0x0f) << "a failed call clears nothing";

  // **A dictionary larger than the piece with no views is refused**, which is the trap this check
  // exists for: the per-row path is the one that would be taken, and it has nothing to read. A
  // table must not produce this, and the refusal is what says so rather than a comment.
  const std::vector<std::string_view> big = {"a", "b", "c", "d", "e"};
  const std::vector<std::int32_t> two = {0, 1};
  const plan::ScanValues larger{.array = nullptr,
                                .strings = {},
                                .validity = nullptr,
                                .rows = 2,
                                .dictionary = std::span<const std::string_view>(big),
                                .indices = std::span<const std::int32_t>(two)};
  EXPECT_TRUE((*filter)->Apply(0, larger, 0, &selected).IsInvalid())
      << "a dictionary bigger than the piece falls back to the views that are not there";

  // With views as well it is the ordinary shape, whatever the dictionary's size: the fallback has
  // something to read.
  const std::vector<std::string_view> views = {"a", "b"};
  selected = 0x03;
  const plan::ScanValues both{.array = nullptr,
                              .strings = std::span<const std::string_view>(views),
                              .validity = nullptr,
                              .rows = 2,
                              .dictionary = std::span<const std::string_view>(big),
                              .indices = std::span<const std::int32_t>(two)};
  ASSERT_TRUE((*filter)->Apply(0, both, 0, &selected).ok());
  EXPECT_EQ(selected, 0x01);
}

}  // namespace
}  // namespace antb1::exec
