#include "antb1/exec/sort.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <gtest/gtest.h>

#include "antb1/exec/limit.h"
#include "antb1/exec/operator.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

#include "exec_test_util.h"

namespace antb1::exec {
namespace {

using plan::LogicalType;
using plan::SortKey;
using testing::Column;
using testing::Int64Column;
using testing::Int64s;
using testing::ScriptedSource;
using testing::Strings;

class SortTest : public testing::ExecTest {};

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

std::shared_ptr<arrow::Array> Doubles(const std::vector<std::optional<double>>& values) {
  return testing::ArrayOf<arrow::DoubleBuilder>(arrow::float64(), values);
}

SortKey Key(int index, LogicalType type, bool descending = false, bool nulls_first = false) {
  return SortKey{
      .column = Column(index, "k", type), .descending = descending, .nulls_first = nulls_first};
}

// Sorts a batch whose column 0 is a row id (BIGINT) and returns the ids in the output order.
std::vector<std::optional<int64_t>> SortedIds(const std::shared_ptr<arrow::RecordBatch>& batch,
                                              const std::vector<SortKey>& keys,
                                              std::optional<int64_t> limit = std::nullopt,
                                              int64_t offset = 0, int64_t batch_size = 3) {
  auto source =
      std::make_unique<ScriptedSource>(batch->schema(), std::vector<Batch>{Batch{.data = batch}});
  SortOperator op(std::move(source), keys, limit, offset);
  ExecContext ctx{.batch_size = batch_size};
  auto table = Drain(op, ctx);
  EXPECT_TRUE(table.ok()) << table.status().ToString();
  return table.ok() ? Int64Column(**table) : std::vector<std::optional<int64_t>>{};
}

std::shared_ptr<arrow::RecordBatch> WithIds(
    const std::vector<std::shared_ptr<arrow::Array>>& cols) {
  const int64_t n = cols.front()->length();
  std::vector<std::optional<int64_t>> ids;
  ids.reserve(static_cast<std::size_t>(n));
  for (int64_t i = 0; i < n; ++i) {
    ids.emplace_back(i);
  }
  arrow::FieldVector fields{arrow::field("id", arrow::int64())};
  arrow::ArrayVector arrays{Int64s(ids)};
  for (std::size_t i = 0; i < cols.size(); ++i) {
    fields.push_back(arrow::field("c" + std::to_string(i), cols[i]->type()));
    arrays.push_back(cols[i]);
  }
  return arrow::RecordBatch::Make(arrow::schema(fields), n, arrays);
}

using Ids = std::vector<std::optional<int64_t>>;

TEST_F(SortTest, NullsLastByDefaultInBothDirections) {
  const auto batch = WithIds({Int64s({2, std::nullopt, 1, 3})});
  EXPECT_EQ(SortedIds(batch, {Key(1, LogicalType::kBigInt)}), (Ids{2, 0, 3, 1}));
  EXPECT_EQ(SortedIds(batch, {Key(1, LogicalType::kBigInt, /*descending=*/true)}),
            (Ids{3, 0, 2, 1}));
  EXPECT_EQ(SortedIds(batch, {Key(1, LogicalType::kBigInt, false, /*nulls_first=*/true)}),
            (Ids{1, 2, 0, 3}));
  EXPECT_EQ(SortedIds(batch, {Key(1, LogicalType::kBigInt, true, true)}), (Ids{1, 3, 0, 2}));
}

TEST_F(SortTest, NaNAboveEveryNumberAndSignedZerosTie) {
  const auto batch = WithIds({Doubles({kNaN, 1.0, -0.0, kInf, std::nullopt, 0.0, -kInf, -kNaN})});
  // Ties keep their input order: -0.0 (2) before 0.0 (5), NaN (0) before -NaN (7).
  EXPECT_EQ(SortedIds(batch, {Key(1, LogicalType::kDouble)}), (Ids{6, 2, 5, 1, 3, 0, 7, 4}));
  EXPECT_EQ(SortedIds(batch, {Key(1, LogicalType::kDouble, true)}), (Ids{0, 7, 3, 1, 2, 5, 6, 4}));
}

TEST_F(SortTest, VarcharComparesBytes) {
  const auto batch = WithIds({Strings({"b", "\xFF", "a", "", "ab", "B", std::nullopt})});
  EXPECT_EQ(SortedIds(batch, {Key(1, LogicalType::kVarchar)}), (Ids{3, 5, 2, 4, 0, 1, 6}));
}

TEST_F(SortTest, EveryEngineType) {
  const auto hugeint = [](int64_t v) { return arrow::Decimal128(v); };
  arrow::Decimal128Builder decimals(plan::ToArrow(LogicalType::kHugeInt));
  ASSERT_TRUE(decimals.Append(hugeint(5)).ok());
  ASSERT_TRUE(decimals.Append(arrow::Decimal128::GetMaxValue(38).Negate()).ok());
  ASSERT_TRUE(decimals.AppendNull().ok());
  ASSERT_TRUE(decimals.Append(hugeint(-1)).ok());
  const auto batch = WithIds({
      testing::ArrayOf<arrow::Int16Builder, int16_t>(arrow::int16(), {3, -4, std::nullopt, 0}),
      testing::ArrayOf<arrow::Int32Builder, int32_t>(arrow::int32(), {3, -4, std::nullopt, 0}),
      testing::ArrayOf<arrow::UInt16Builder, uint16_t>(arrow::uint16(),
                                                       {65535, 1, std::nullopt, 0}),
      testing::ArrayOf<arrow::Date32Builder, int32_t>(arrow::date32(), {3, -4, std::nullopt, 0}),
      decimals.Finish().ValueOrDie(),
  });
  EXPECT_EQ(SortedIds(batch, {Key(1, LogicalType::kSmallInt)}), (Ids{1, 3, 0, 2}));
  EXPECT_EQ(SortedIds(batch, {Key(2, LogicalType::kInteger)}), (Ids{1, 3, 0, 2}));
  EXPECT_EQ(SortedIds(batch, {Key(3, LogicalType::kUSmallInt)}), (Ids{3, 1, 0, 2}));
  EXPECT_EQ(SortedIds(batch, {Key(4, LogicalType::kDate)}), (Ids{1, 3, 0, 2}));
  EXPECT_EQ(SortedIds(batch, {Key(5, LogicalType::kHugeInt)}), (Ids{1, 3, 0, 2}));
}

TEST_F(SortTest, LaterKeysBreakTies) {
  const auto batch =
      WithIds({Int64s({1, 2, 1, 2, 1}), Strings({"x", "y", "z", "a", std::nullopt})});
  EXPECT_EQ(SortedIds(batch, {Key(1, LogicalType::kBigInt, true), Key(2, LogicalType::kVarchar)}),
            (Ids{3, 1, 0, 2, 4}));
  EXPECT_EQ(SortedIds(batch, {Key(1, LogicalType::kBigInt)}), (Ids{0, 2, 4, 1, 3}));
}

// splitmix64 with a fixed seed: deterministic test data.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed) {}
  std::uint64_t Next() {
    std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
  }
  int64_t Below(int64_t n) { return static_cast<int64_t>(Next() % static_cast<std::uint64_t>(n)); }

 private:
  std::uint64_t state_;
};

struct RandomRows {
  std::vector<std::optional<int64_t>> ints;
  std::vector<std::optional<double>> doubles;
  std::shared_ptr<arrow::RecordBatch> batch;  // id, ints, doubles
};

RandomRows MakeRandomRows(int64_t n, Rng& rng) {
  RandomRows rows;
  const std::vector<double> specials{kNaN, -0.0, 0.0, kInf, -kInf, 1.5, -1.5};
  for (int64_t i = 0; i < n; ++i) {
    rows.ints.push_back(rng.Below(6) == 0 ? std::nullopt : std::optional(rng.Below(20) - 10));
    const int64_t pick = rng.Below(10);
    std::optional<double> value = static_cast<double>(rng.Below(8));
    if (pick == 0) {
      value.reset();
    } else if (pick < 4) {
      value = specials[static_cast<std::size_t>(rng.Below(static_cast<int64_t>(specials.size())))];
    }
    rows.doubles.push_back(value);
  }
  rows.batch = WithIds({Int64s(rows.ints), Doubles(rows.doubles)});
  return rows;
}

// An independent model of the order: std::stable_sort over the values.
Ids ModelOrder(const RandomRows& rows, bool int_desc, bool int_nulls_first, bool double_desc,
               bool double_nulls_first) {
  // Exactly one of the two values is NULL.
  const auto nulls = [](bool a_null, bool first) { return a_null == first ? -1 : 1; };
  const auto three_way = [](auto x, auto y) {
    return static_cast<int>(y < x) - static_cast<int>(x < y);
  };
  const auto cmp_int = [&](std::size_t a, std::size_t b) {
    const auto& x = rows.ints[a];
    const auto& y = rows.ints[b];
    if (!x.has_value() && !y.has_value()) {
      return 0;
    }
    if (!x.has_value() || !y.has_value()) {
      return nulls(!x.has_value(), int_nulls_first);
    }
    const int c = three_way(*x, *y);
    return int_desc ? -c : c;
  };
  const auto rank = [](double v) { return std::isnan(v) ? 1 : 0; };
  const auto cmp_double = [&](std::size_t a, std::size_t b) {
    const auto& x = rows.doubles[a];
    const auto& y = rows.doubles[b];
    if (!x.has_value() && !y.has_value()) {
      return 0;
    }
    if (!x.has_value() || !y.has_value()) {
      return nulls(!x.has_value(), double_nulls_first);
    }
    int c = rank(*x) - rank(*y);
    if (c == 0 && rank(*x) == 0) {
      c = three_way(*x, *y);
    }
    return double_desc ? -c : c;
  };
  std::vector<std::size_t> order(rows.ints.size());
  for (std::size_t i = 0; i < order.size(); ++i) {
    order[i] = i;
  }
  std::ranges::stable_sort(order, [&](std::size_t a, std::size_t b) {
    const int c = cmp_int(a, b);
    return c != 0 ? c < 0 : cmp_double(a, b) < 0;
  });
  Ids ids;
  for (const std::size_t i : order) {
    ids.emplace_back(static_cast<int64_t>(i));
  }
  return ids;
}

// Sort and top-N against the model, over every direction and NULL placement, in batches of several
// sizes and with LIMIT/OFFSET windows around the batch and compaction boundaries.
TEST_F(SortTest, MatchesModelAndTopNMatchesSortWindow) {
  Rng rng(20260927);
  for (const int64_t n : {int64_t{0}, int64_t{1}, int64_t{7}, int64_t{300}, int64_t{9000}}) {
    const RandomRows rows = MakeRandomRows(n, rng);
    for (unsigned flags = 0; flags < 16; ++flags) {
      const bool int_desc = (flags & 1U) != 0;
      const bool int_nulls_first = (flags & 2U) != 0;
      const bool double_desc = (flags & 4U) != 0;
      const bool double_nulls_first = (flags & 8U) != 0;
      const std::vector<SortKey> keys{
          Key(1, LogicalType::kBigInt, int_desc, int_nulls_first),
          Key(2, LogicalType::kDouble, double_desc, double_nulls_first)};
      const Ids expected =
          ModelOrder(rows, int_desc, int_nulls_first, double_desc, double_nulls_first);
      ASSERT_EQ(SortedIds(rows.batch, keys, std::nullopt, 0, 1000), expected)
          << "n=" << n << " flags=" << flags;
      for (const auto& [limit, offset] :
           std::vector<std::pair<int64_t, int64_t>>{{1, 0},
                                                    {3, 2},
                                                    {10, 0},
                                                    {4097, 5},
                                                    {8, 8990},
                                                    {std::numeric_limits<int64_t>::max(), 3}}) {
        const auto begin = static_cast<std::size_t>(std::min(offset, n));
        const auto end =
            static_cast<std::size_t>(std::min(n, static_cast<int64_t>(begin) + std::min(limit, n)));
        const Ids window(expected.begin() + static_cast<std::ptrdiff_t>(begin),
                         expected.begin() + static_cast<std::ptrdiff_t>(end));
        ASSERT_EQ(SortedIds(rows.batch, keys, limit, offset, 1000), window)
            << "n=" << n << " flags=" << flags << " limit=" << limit << " offset=" << offset;
      }
    }
  }
}

// Buffers filled from consecutive parts of the input and merged in input order give the
// single-buffer result, for Sort and for top-N (whose parts are compacted separately).
TEST_F(SortTest, MergedBuffersEqualOneBuffer) {
  Rng rng(7);
  const RandomRows rows = MakeRandomRows(20000, rng);
  const std::vector<SortKey> keys{Key(1, LogicalType::kBigInt), Key(2, LogicalType::kDouble, true)};
  for (const std::optional<int64_t> keep :
       {std::optional<int64_t>{}, std::optional<int64_t>{1}, std::optional<int64_t>{5000}}) {
    const auto make = [&] {
      return SortBuffer(RowComparator::Make(rows.batch->schema(), keys).ValueOrDie(), keep);
    };
    SortBuffer single = make();
    for (int64_t start = 0; start < 20000; start += 1000) {
      ASSERT_TRUE(single.Add(rows.batch->Slice(start, 1000), arrow::default_memory_pool()).ok());
    }
    ASSERT_TRUE(single.Sort(arrow::default_memory_pool()).ok());

    SortBuffer merged = make();
    for (int64_t part = 0; part < 20000; part += 7000) {
      SortBuffer partial = make();
      for (int64_t start = part; start < std::min<int64_t>(part + 7000, 20000); start += 700) {
        ASSERT_TRUE(partial.Add(rows.batch->Slice(start, 700), arrow::default_memory_pool()).ok());
      }
      if (part == 7000) {
        ASSERT_TRUE(partial.Sort(arrow::default_memory_pool()).ok());  // a sorted part merges too
      }
      ASSERT_TRUE(merged.Merge(partial, arrow::default_memory_pool()).ok());
      EXPECT_EQ(partial.num_rows(), 0);
    }
    ASSERT_TRUE(merged.Sort(arrow::default_memory_pool()).ok());
    ASSERT_EQ(merged.num_rows(), single.num_rows());
    const auto a = single.Slice(0, single.num_rows(), arrow::default_memory_pool()).ValueOrDie();
    const auto b = merged.Slice(0, merged.num_rows(), arrow::default_memory_pool()).ValueOrDie();
    // The ids (column 0) name the rows; NaN keys would not compare equal.
    EXPECT_TRUE(a->column(0)->Equals(*b->column(0))) << "keep=" << keep.value_or(-1);
  }
}

// The first-key prefix never contradicts the comparator: a smaller (group, bits) sorts first, and
// for an exact prefix equal ones tie on that key, for every engine type, direction and NULL order.
TEST_F(SortTest, PrefixesAgreeWithTheComparator) {
  Rng rng(99);
  const auto pick = [&](std::size_t n) {
    return static_cast<std::size_t>(rng.Below(static_cast<int64_t>(n)));
  };
  std::vector<std::pair<LogicalType, std::shared_ptr<arrow::Array>>> columns;
  {
    std::vector<std::optional<int64_t>> v;
    v.reserve(64);
    const std::vector<int64_t> edges = {std::numeric_limits<int64_t>::min(),
                                        std::numeric_limits<int64_t>::min() + 1,
                                        -2,
                                        -1,
                                        0,
                                        1,
                                        2,
                                        std::numeric_limits<int64_t>::max() - 1,
                                        std::numeric_limits<int64_t>::max()};
    for (int i = 0; i < 64; ++i) {
      v.emplace_back(i % 9 == 0 ? std::optional<int64_t>{}
                                : std::optional(edges[pick(edges.size())]));
    }
    columns.emplace_back(LogicalType::kBigInt, Int64s(v));
  }
  {
    std::vector<std::optional<int16_t>> v;
    v.reserve(64);
    for (int i = 0; i < 64; ++i) {
      v.emplace_back(i % 9 == 0 ? std::optional<int16_t>{}
                                : std::optional(static_cast<int16_t>(rng.Below(65536) - 32768)));
    }
    columns.emplace_back(LogicalType::kSmallInt,
                         testing::ArrayOf<arrow::Int16Builder, int16_t>(arrow::int16(), v));
  }
  {
    std::vector<std::optional<uint16_t>> v;
    v.reserve(64);
    for (int i = 0; i < 64; ++i) {
      v.emplace_back(i % 9 == 0 ? std::optional<uint16_t>{}
                                : std::optional(static_cast<uint16_t>(rng.Below(65536))));
    }
    columns.emplace_back(LogicalType::kUSmallInt,
                         testing::ArrayOf<arrow::UInt16Builder, uint16_t>(arrow::uint16(), v));
  }
  {
    std::vector<std::optional<double>> v;
    v.reserve(64);
    const std::vector<double> specials = {kNaN,   -kNaN,   kInf, -kInf, 0.0,  -0.0,
                                          1e-310, -1e-310, 1.5,  -1.5,  1e300};
    for (int i = 0; i < 64; ++i) {
      v.emplace_back(i % 9 == 0 ? std::optional<double>{}
                                : std::optional(specials[pick(specials.size())]));
    }
    columns.emplace_back(LogicalType::kDouble, Doubles(v));
  }
  {
    std::vector<std::optional<std::string>> v;
    v.reserve(64);
    const std::vector<std::string> specials = {"",
                                               "a",
                                               "ab",
                                               std::string("a\0", 2),
                                               std::string("a\0b", 3),
                                               "abcdefgh",
                                               "abcdefghi",
                                               "abcdefgh\x01",
                                               "\xFF",
                                               "\x7F",
                                               "b"};
    for (int i = 0; i < 64; ++i) {
      v.emplace_back(i % 9 == 0 ? std::optional<std::string>{}
                                : std::optional(specials[pick(specials.size())]));
    }
    columns.emplace_back(LogicalType::kVarchar, Strings(v));
  }
  {
    arrow::Decimal128Builder decimals(plan::ToArrow(LogicalType::kHugeInt));
    const std::vector<arrow::Decimal128> specials = {arrow::Decimal128(0),
                                                     arrow::Decimal128(-1),
                                                     arrow::Decimal128(1),
                                                     arrow::Decimal128(1, 0),
                                                     arrow::Decimal128(1, 5),
                                                     arrow::Decimal128(-1, 5),
                                                     arrow::Decimal128::GetMaxValue(38),
                                                     arrow::Decimal128::GetMaxValue(38).Negate()};
    for (int i = 0; i < 64; ++i) {
      ASSERT_TRUE(
          (i % 9 == 0 ? decimals.AppendNull() : decimals.Append(specials[pick(specials.size())]))
              .ok());
    }
    columns.emplace_back(LogicalType::kHugeInt, decimals.Finish().ValueOrDie());
  }
  for (const auto& [type, column] : columns) {
    const auto batch = WithIds({column});
    for (unsigned flags = 0; flags < 4; ++flags) {
      const bool descending = (flags & 1U) != 0;
      const bool nulls_first = (flags & 2U) != 0;
      const auto comparator =
          RowComparator::Make(batch->schema(), {Key(1, type, descending, nulls_first)})
              .ValueOrDie();
      const auto keys = comparator.KeysOf(*batch);
      for (int64_t i = 0; i < batch->num_rows(); ++i) {
        for (int64_t j = 0; j < batch->num_rows(); ++j) {
          const auto a = comparator.PrefixOf(keys, i);
          const auto b = comparator.PrefixOf(keys, j);
          const int c = comparator.Compare(keys, i, keys, j);
          const auto pa = std::pair(a.group, a.bits);
          const auto pb = std::pair(b.group, b.bits);
          if (pa < pb) {
            ASSERT_LT(c, 0) << plan::ToString(type) << " flags " << flags << " rows " << i << ", "
                            << j;
          } else if (pa == pb && comparator.prefix_is_exact()) {
            ASSERT_EQ(c, 0) << plan::ToString(type) << " flags " << flags << " rows " << i << ", "
                            << j;
          }
        }
      }
    }
  }
}

TEST_F(SortTest, ConsumesSelections) {
  const auto batch = WithIds({Int64s({5, 4, 3, 2, 1})});
  auto source = std::make_unique<ScriptedSource>(
      batch->schema(),
      std::vector<Batch>{
          Batch{.data = batch, .selection = testing::Bools({true, false, true, false, true})}});
  SortOperator op(std::move(source), {Key(1, LogicalType::kBigInt)}, 2, 0);
  ExecContext ctx;
  const auto table = Drain(op, ctx);
  ASSERT_TRUE(table.ok()) << table.status().ToString();
  EXPECT_EQ(Int64Column(**table), (Ids{4, 2}));
}

TEST_F(SortTest, RejectsBadKeysAndCalls) {
  const auto batch = WithIds({Int64s({1})});
  EXPECT_FALSE(RowComparator::Make(batch->schema(), {Key(2, LogicalType::kBigInt)}).ok());
  EXPECT_FALSE(RowComparator::Make(batch->schema(), {Key(1, LogicalType::kDouble)}).ok());
  EXPECT_FALSE(RowComparator::Make(nullptr, {}).ok());

  const auto source = [&] {
    return std::make_unique<ScriptedSource>(batch->schema(), std::vector<Batch>{});
  };
  ExecContext ctx;
  SortOperator no_keys(source(), {});
  EXPECT_FALSE(no_keys.Open(ctx).ok());
  SortOperator negative(source(), {Key(1, LogicalType::kBigInt)}, 1, -1);
  EXPECT_FALSE(negative.Open(ctx).ok());
  SortOperator unopened(source(), {Key(1, LogicalType::kBigInt)});
  EXPECT_FALSE(unopened.Next().ok());

  SortBuffer buffer(
      RowComparator::Make(batch->schema(), {Key(1, LogicalType::kBigInt)}).ValueOrDie(),
      std::nullopt);
  EXPECT_FALSE(buffer.Slice(0, 1, arrow::default_memory_pool()).ok());  // before Sort
  const auto other = WithIds({Strings({"x"})});
  EXPECT_FALSE(buffer.Add(other, arrow::default_memory_pool()).ok());
  SortBuffer keeping(
      RowComparator::Make(batch->schema(), {Key(1, LogicalType::kBigInt)}).ValueOrDie(), 1);
  EXPECT_FALSE(buffer.Merge(keeping, arrow::default_memory_pool()).ok());
}

// OFFSET skips selected rows across batches; the kept rows keep their selection.
TEST_F(SortTest, LimitOffsetNarrowsSelections) {
  const auto a = WithIds({Int64s({0, 0, 0, 0})});
  const auto b = WithIds({Int64s({0, 0, 0, 0})});
  const auto run = [&](std::optional<int64_t> limit, int64_t offset) {
    auto source = std::make_unique<ScriptedSource>(
        a->schema(),
        std::vector<Batch>{
            Batch{.data = a, .selection = testing::Bools({true, false, true, true})},
            Batch{.data = b->Slice(0, 0)},
            Batch{.data = b, .selection = testing::Bools({false, true, true, false})}});
    const auto* raw = source.get();
    LimitOperator op(std::move(source), limit, offset);
    ExecContext ctx;
    const auto table = Drain(op, ctx);
    EXPECT_TRUE(table.ok()) << table.status().ToString();
    return std::pair(table.ok() ? Int64Column(**table) : Ids{}, raw->pulls());
  };
  EXPECT_EQ(run(std::nullopt, 0).first, (Ids{0, 2, 3, 1, 2}));
  EXPECT_EQ(run(std::nullopt, 1).first, (Ids{2, 3, 1, 2}));
  EXPECT_EQ(run(2, 1).first, (Ids{2, 3}));
  EXPECT_EQ(run(2, 1).second, 1);  // the limit is reached inside the first batch
  EXPECT_EQ(run(1, 2).first, (Ids{3}));
  EXPECT_EQ(run(2, 3).first, (Ids{1, 2}));
  EXPECT_EQ(run(10, 4).first, (Ids{2}));
  EXPECT_EQ(run(10, 5).first, (Ids{}));
  EXPECT_EQ(run(0, 0).second, 0);

  LimitOperator negative(std::make_unique<ScriptedSource>(a->schema(), std::vector<Batch>{}),
                         std::nullopt, -1);
  ExecContext ctx;
  EXPECT_FALSE(negative.Open(ctx).ok());
}

}  // namespace
}  // namespace antb1::exec
