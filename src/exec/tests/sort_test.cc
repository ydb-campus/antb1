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
#include "antb1/exec/memory_budget.h"
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
  // DECIMAL(15,2) orders by the unscaled value: 17.00, -0.25, NULL, -999999999999.99. Through the
  // width-aware helper, so the column's physical type is the one the sort key's Bind expects at
  // either setting of the narrowing switch (ADR 0021's decimal64 item).
  const auto scaled = testing::Decimals(LogicalType::Decimal(15, 2),
                                        {"1700", "-25", std::nullopt, "-99999999999999"});
  const auto batch = WithIds({
      testing::ArrayOf<arrow::Int16Builder, int16_t>(arrow::int16(), {3, -4, std::nullopt, 0}),
      testing::ArrayOf<arrow::Int32Builder, int32_t>(arrow::int32(), {3, -4, std::nullopt, 0}),
      testing::ArrayOf<arrow::UInt16Builder, uint16_t>(arrow::uint16(),
                                                       {65535, 1, std::nullopt, 0}),
      testing::ArrayOf<arrow::Date32Builder, int32_t>(arrow::date32(), {3, -4, std::nullopt, 0}),
      decimals.Finish().ValueOrDie(),
      scaled,
  });
  EXPECT_EQ(SortedIds(batch, {Key(1, LogicalType::kSmallInt)}), (Ids{1, 3, 0, 2}));
  EXPECT_EQ(SortedIds(batch, {Key(2, LogicalType::kInteger)}), (Ids{1, 3, 0, 2}));
  EXPECT_EQ(SortedIds(batch, {Key(3, LogicalType::kUSmallInt)}), (Ids{3, 1, 0, 2}));
  EXPECT_EQ(SortedIds(batch, {Key(4, LogicalType::kDate)}), (Ids{1, 3, 0, 2}));
  EXPECT_EQ(SortedIds(batch, {Key(5, LogicalType::kHugeInt)}), (Ids{1, 3, 0, 2}));
  EXPECT_EQ(SortedIds(batch, {Key(6, LogicalType::Decimal(15, 2))}), (Ids{3, 1, 0, 2}));
  EXPECT_EQ(SortedIds(batch, {Key(6, LogicalType::Decimal(15, 2), true)}), (Ids{0, 1, 3, 2}));
}

// A narrow DECIMAL key takes its own comparator and its own prefix, and is `exact` -- the whole
// value fits the prefix -- so the comparator is never called for it and only the order can say the
// prefix is right (ADR 0021's decimal64 item).
TEST_F(SortTest, ANarrowDecimalKeySortsByItsValue) {
  const LogicalType money = LogicalType::Decimal(15, 2);
  // 17.00, -0.25, NULL, -999999999999.99, 0.00 and 17.00 again -- the repeat is there because a
  // tie is the one thing a prefix cannot decide on its own.
  const auto batch = WithIds(
      {testing::Decimals(money, {"1700", "-25", std::nullopt, "-99999999999999", "0", "1700"})});
  ASSERT_EQ(batch->column(1)->type_id(), arrow::Type::DECIMAL64);

  // By value, NULLS LAST, and the tie between rows 0 and 5 broken by their order in the input.
  EXPECT_EQ(SortedIds(batch, {Key(1, money)}), (Ids{3, 1, 4, 0, 5, 2}));
  EXPECT_EQ(SortedIds(batch, {Key(1, money, /*descending=*/true)}), (Ids{0, 5, 4, 1, 3, 2}));
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

// ---- reading the buffer: every column, from many chunks only the buffer holds ----

// Rows of six columns kept as plain vectors, so that every batch made from them is held by the
// sort buffer it is given to alone (a read of one the buffer dropped is a read of freed memory,
// which ASan reports): a row id, a BIGINT key with NULLs and many ties, a BIGINT twin (a read of
// another column's arrays shows), VARCHAR, DOUBLE and BOOLEAN (the gather's slice path).
struct WideRows {
  std::vector<std::optional<int64_t>> keys;
  std::vector<std::optional<int64_t>> twins;
  std::vector<std::optional<std::string>> strings;
  std::vector<std::optional<double>> doubles;
  std::vector<std::optional<bool>> bools;

  static std::shared_ptr<arrow::Schema> Schema() {
    return arrow::schema({arrow::field("id", arrow::int64()), arrow::field("k", arrow::int64()),
                          arrow::field("twin", arrow::int64()), arrow::field("s", arrow::binary()),
                          arrow::field("d", arrow::float64()),
                          arrow::field("b", arrow::boolean())});
  }

  // Rows [begin, end) as a new batch that only the caller holds.
  [[nodiscard]] std::shared_ptr<arrow::RecordBatch> Chunk(int64_t begin, int64_t end) const {
    const auto rows = [begin, end](const auto& values) {
      return std::vector(values.begin() + static_cast<std::ptrdiff_t>(begin),
                         values.begin() + static_cast<std::ptrdiff_t>(end));
    };
    std::vector<std::optional<int64_t>> ids;
    ids.reserve(static_cast<std::size_t>(end - begin));
    for (int64_t id = begin; id < end; ++id) {
      ids.emplace_back(id);
    }
    return arrow::RecordBatch::Make(
        Schema(), end - begin,
        {Int64s(ids), Int64s(rows(keys)), Int64s(rows(twins)), Strings(rows(strings)),
         Doubles(rows(doubles)), testing::Bools(rows(bools))});
  }
};

WideRows MakeWideRows(int64_t n, Rng& rng) {
  WideRows rows;
  const auto size = static_cast<std::size_t>(n);
  rows.keys.reserve(size);
  rows.twins.reserve(size);
  rows.strings.reserve(size);
  rows.doubles.reserve(size);
  rows.bools.reserve(size);
  for (int64_t i = 0; i < n; ++i) {
    rows.keys.push_back(rng.Below(6) == 0 ? std::nullopt : std::optional(rng.Below(16)));
    rows.twins.push_back(i % 11 == 5 ? std::nullopt : std::optional(((i * 7919) % 1009) - 500));
    rows.strings.push_back(i % 8 == 3
                               ? std::nullopt
                               : std::optional("s" + std::to_string(i) +
                                               std::string(static_cast<std::size_t>(i % 9), 'x')));
    rows.doubles.push_back(i % 7 == 2 ? std::nullopt
                                      : std::optional((static_cast<double>(i) / 2) - 3));
    rows.bools.push_back(i % 5 == 1 ? std::nullopt : std::optional(i % 3 == 0));
  }
  return rows;
}

// The ids [0, n) in the order of a stable sort by the key.
std::vector<int64_t> KeyOrder(const WideRows& rows, int64_t n, bool descending, bool nulls_first) {
  std::vector<int64_t> order;
  order.reserve(static_cast<std::size_t>(n));
  for (int64_t id = 0; id < n; ++id) {
    order.push_back(id);
  }
  std::ranges::stable_sort(order, [&](int64_t a, int64_t b) {
    const std::optional<int64_t>& x = rows.keys[static_cast<std::size_t>(a)];
    const std::optional<int64_t>& y = rows.keys[static_cast<std::size_t>(b)];
    if (!x.has_value() || !y.has_value()) {  // NULLs first or last whatever the direction
      return x.has_value() != y.has_value() && x.has_value() != nulls_first;
    }
    return descending ? *x > *y : *x < *y;
  });
  return order;
}

std::optional<int64_t> IntAt(const arrow::Array& array, int64_t i) {
  return array.IsNull(i) ? std::nullopt
                         : std::optional(static_cast<const arrow::Int64Array&>(array).Value(i));
}

std::optional<std::string> StringAt(const arrow::Array& array, int64_t i) {
  return array.IsNull(i)
             ? std::nullopt
             : std::optional(static_cast<const arrow::BinaryArray&>(array).GetString(i));
}

std::optional<double> DoubleAt(const arrow::Array& array, int64_t i) {
  return array.IsNull(i) ? std::nullopt
                         : std::optional(static_cast<const arrow::DoubleArray&>(array).Value(i));
}

std::optional<bool> BoolAt(const arrow::Array& array, int64_t i) {
  return array.IsNull(i) ? std::nullopt
                         : std::optional(static_cast<const arrow::BooleanArray&>(array).Value(i));
}

// Every column of `got` holds the rows order[begin], order[begin + 1], ... of `rows`.
void ExpectRowsOf(const arrow::RecordBatch& got, const WideRows& rows,
                  const std::vector<int64_t>& order, int64_t begin) {
  ASSERT_EQ(got.num_columns(), 6);
  for (int64_t i = 0; i < got.num_rows(); ++i) {
    const int64_t id = order[static_cast<std::size_t>(begin + i)];
    const auto row = static_cast<std::size_t>(id);
    ASSERT_EQ(IntAt(*got.column(0), i), id) << "row " << begin + i;
    EXPECT_EQ(IntAt(*got.column(1), i), rows.keys[row]) << "id " << id;
    EXPECT_EQ(IntAt(*got.column(2), i), rows.twins[row]) << "id " << id;
    EXPECT_EQ(StringAt(*got.column(3), i), rows.strings[row]) << "id " << id;
    EXPECT_EQ(DoubleAt(*got.column(4), i), rows.doubles[row]) << "id " << id;
    EXPECT_EQ(BoolAt(*got.column(5), i), rows.bools[row]) << "id " << id;
  }
}

// Reads the buffer's sorted rows in windows of 1, 5, 64 and all of them, and clamped past both
// ends, each window checked against `order`, the ids the buffer should hold in their order.
void ExpectWindows(const SortBuffer& buffer, const WideRows& rows,
                   const std::vector<int64_t>& order) {
  const int64_t n = buffer.num_rows();
  ASSERT_EQ(n, static_cast<int64_t>(order.size()));
  for (const int64_t size : {int64_t{1}, int64_t{5}, int64_t{64}, n}) {
    for (int64_t begin = 0; begin < n; begin += size) {
      const auto got = buffer.Slice(begin, begin + size, arrow::default_memory_pool());
      ASSERT_TRUE(got.ok()) << got.status().ToString();
      ASSERT_EQ((*got)->num_rows(), std::min(size, n - begin));
      ExpectRowsOf(**got, rows, order, begin);
    }
  }
  const auto head = buffer.Slice(-5, 3, arrow::default_memory_pool());
  ASSERT_TRUE(head.ok()) << head.status().ToString();
  ASSERT_EQ((*head)->num_rows(), std::min<int64_t>(3, n));
  ExpectRowsOf(**head, rows, order, 0);
  const auto tail = buffer.Slice(n - 2, n + 10, arrow::default_memory_pool());
  ASSERT_TRUE(tail.ok()) << tail.status().ToString();
  ASSERT_EQ((*tail)->num_rows(), std::min<int64_t>(2, n));
  ExpectRowsOf(**tail, rows, order, std::max<int64_t>(n - 2, 0));
  const auto past = buffer.Slice(n, n + 5, arrow::default_memory_pool());
  ASSERT_TRUE(past.ok()) << past.status().ToString();
  EXPECT_EQ((*past)->num_rows(), 0);
}

// Adds rows [begin, end) to `buffer` in chunks of 1 to `max_rows` rows, each held by it alone.
void AddChunks(SortBuffer& buffer, const WideRows& rows, int64_t begin, int64_t end,
               int64_t max_rows, Rng& rng) {
  for (int64_t start = begin; start < end;) {
    const int64_t stop = std::min(end, start + 1 + rng.Below(max_rows));
    ASSERT_TRUE(buffer.Add(rows.Chunk(start, stop), arrow::default_memory_pool()).ok());
    start = stop;
  }
}

// Every window of every column is the model's, read from the arrays the buffer took as each chunk
// arrived: over many chunks of 1-7 rows, in every direction and NULL order, again after more rows
// (one per chunk) arrive and the buffer is sorted again, and from a buffer it was moved to.
TEST_F(SortTest, EveryWindowOfEveryColumnIsTheModels) {
  Rng rng(20261023);
  const WideRows rows = MakeWideRows(400, rng);
  for (unsigned flags = 0; flags < 4; ++flags) {
    const bool descending = (flags & 1U) != 0;
    const bool nulls_first = (flags & 2U) != 0;
    SCOPED_TRACE(flags);
    SortBuffer buffer(RowComparator::Make(WideRows::Schema(),
                                          {Key(1, LogicalType::kBigInt, descending, nulls_first)})
                          .ValueOrDie(),
                      std::nullopt);
    AddChunks(buffer, rows, 0, 300, 7, rng);
    ASSERT_TRUE(buffer.Sort(arrow::default_memory_pool()).ok());
    ExpectWindows(buffer, rows, KeyOrder(rows, 300, descending, nulls_first));

    AddChunks(buffer, rows, 300, 400, 1, rng);
    EXPECT_FALSE(buffer.Slice(0, 1, arrow::default_memory_pool()).ok()) << "unsorted again";
    ASSERT_TRUE(buffer.Sort(arrow::default_memory_pool()).ok());
    const std::vector<int64_t> order = KeyOrder(rows, 400, descending, nulls_first);
    ExpectWindows(buffer, rows, order);
    const SortBuffer moved = std::move(buffer);
    ExpectWindows(moved, rows, order);
  }
}

// A top-N is read from the one chunk its latest compaction made: right after an Add that compacted
// the rows (they outgrew the slack), after Tighten, and after Sort. Once it has a threshold, a
// batch only some of whose rows beat it is taken into a new batch, and read from that one. Every
// window against the model's first rows of everything added so far.
TEST_F(SortTest, EveryWindowOfATopNIsTheModels) {
  Rng rng(4600);
  WideRows rows = MakeWideRows(4600, rng);
  // The last batch: two rows before every other row and one after every other value, so that only
  // some of its rows beat the threshold, whatever value it is (the first rows hold no NULL).
  rows.keys[4597] = -1;
  rows.keys[4598] = 1000;
  rows.keys[4599] = -2;
  for (const int64_t keep : {int64_t{1}, int64_t{7}, int64_t{300}}) {
    SCOPED_TRACE(keep);
    SortBuffer buffer(
        RowComparator::Make(WideRows::Schema(), {Key(1, LogicalType::kBigInt)}).ValueOrDie(), keep);
    const auto expect_first = [&](int64_t added) {
      std::vector<int64_t> order = KeyOrder(rows, added, false, false);
      order.resize(static_cast<std::size_t>(std::min(keep, added)));
      ExpectWindows(buffer, rows, order);
    };
    int64_t compactions = 0;
    for (int64_t start = 0; start < 4597;) {
      const int64_t stop = std::min<int64_t>(4597, start + 1 + rng.Below(7));
      ASSERT_TRUE(buffer.Add(rows.Chunk(start, stop), arrow::default_memory_pool()).ok());
      if (buffer.Slice(0, 0, arrow::default_memory_pool()).ok()) {  // this Add compacted
        ++compactions;
        expect_first(stop);
      }
      if (start < 4500 && stop >= 4500) {
        ASSERT_TRUE(buffer.Tighten(arrow::default_memory_pool()).ok());
        expect_first(stop);
      }
      start = stop;
    }
    EXPECT_GE(compactions, 1);
    ASSERT_TRUE(buffer.Add(rows.Chunk(4597, 4600), arrow::default_memory_pool()).ok());
    ASSERT_TRUE(buffer.Sort(arrow::default_memory_pool()).ok());
    expect_first(4600);
  }
}

using IdRows = arrow::Result<std::shared_ptr<arrow::RecordBatch>>;

// Column 0 (the ids) of a buffer's rows.
Ids IdsOf(const IdRows& rows) {
  EXPECT_TRUE(rows.ok()) << rows.status().ToString();
  Ids ids;
  if (rows.ok()) {
    const std::shared_ptr<arrow::Array> column = (*rows)->column(0);
    for (int64_t i = 0; i < column->length(); ++i) {
      ids.push_back(IntAt(*column, i));
    }
  }
  return ids;
}

// A batch of (id, key) rows, ids from `first_id` on.
std::shared_ptr<arrow::RecordBatch> IdKeys(int64_t first_id,
                                           const std::vector<std::optional<int64_t>>& keys) {
  std::vector<std::optional<int64_t>> ids;
  ids.reserve(keys.size());
  for (std::size_t i = 0; i < keys.size(); ++i) {
    ids.emplace_back(first_id + static_cast<int64_t>(i));
  }
  return arrow::RecordBatch::Make(
      arrow::schema({arrow::field("id", arrow::int64()), arrow::field("k", arrow::int64())}),
      static_cast<int64_t>(keys.size()), {Int64s(ids), Int64s(keys)});
}

SortBuffer IdKeyBuffer(std::optional<int64_t> keep) {
  return {RowComparator::Make(IdKeys(0, {})->schema(), {Key(1, LogicalType::kBigInt)}).ValueOrDie(),
          keep};
}

// The same, read by hand: a top-N reads as the rows its latest compaction kept, from Tighten and
// from Adds that compacted, and as unsorted after any other Add.
TEST_F(SortTest, ATopNIsReadFromItsLatestCompaction) {
  arrow::MemoryPool* pool = arrow::default_memory_pool();
  SortBuffer three = IdKeyBuffer(3);
  ASSERT_TRUE(three.Add(IdKeys(0, {5, 3, 8, 1, 9}), pool).ok());
  EXPECT_FALSE(three.Slice(0, 3, pool).ok());
  ASSERT_TRUE(three.Tighten(pool).ok());
  EXPECT_EQ(IdsOf(three.Slice(0, 3, pool)), (Ids{3, 1, 0}));  // keys 1, 3, 5: the threshold is 5
  ASSERT_TRUE(three.Add(IdKeys(5, {4, 7, 2}), pool).ok());    // 4 and 2 beat it: a new batch
  EXPECT_FALSE(three.Slice(0, 3, pool).ok());
  ASSERT_TRUE(three.Tighten(pool).ok());
  EXPECT_EQ(IdsOf(three.Slice(0, 3, pool)), (Ids{3, 7, 1}));
  ASSERT_TRUE(three.Sort(pool).ok());
  EXPECT_EQ(IdsOf(three.Slice(0, 3, pool)), (Ids{3, 7, 1}));

  // Keep 2 compacts in the Add that brings more than 2 + 4096 rows.
  SortBuffer two = IdKeyBuffer(2);
  std::vector<std::optional<int64_t>> rising;
  std::vector<std::optional<int64_t>> falling;
  rising.reserve(4099);
  falling.reserve(4097);
  for (int64_t id = 0; id < 4099; ++id) {
    rising.emplace_back(id);
  }
  for (int64_t id = 4099; id < 8196; ++id) {
    falling.emplace_back(-id);
  }
  ASSERT_TRUE(two.Add(IdKeys(0, rising), pool).ok());
  EXPECT_EQ(IdsOf(two.Slice(0, 2, pool)), (Ids{0, 1}));
  ASSERT_TRUE(two.Add(IdKeys(4099, falling), pool).ok());  // every row beats key 1: all kept
  EXPECT_EQ(IdsOf(two.Slice(0, 2, pool)), (Ids{8195, 8194}));
  ASSERT_TRUE(two.Sort(pool).ok());
  EXPECT_EQ(IdsOf(two.Slice(0, 2, pool)), (Ids{8195, 8194}));
}

// Parts filled on their own and merged in input order, every other one sorted first (a top-N part
// is then one compacted chunk); with a limit the merged rows are tightened after every merge and
// read right away. A merged part is left empty and unsorted, and reads as no rows once sorted, as
// does a buffer that never had any, whatever its keep.
TEST_F(SortTest, MergedPartsAreReadFromTheirLatestCompaction) {
  arrow::MemoryPool* pool = arrow::default_memory_pool();
  Rng rng(6);
  const WideRows rows = MakeWideRows(600, rng);
  const auto buffer = [](std::optional<int64_t> keep) {
    return SortBuffer(
        RowComparator::Make(WideRows::Schema(), {Key(1, LogicalType::kBigInt)}).ValueOrDie(), keep);
  };
  for (const std::optional<int64_t> keep : {std::optional<int64_t>{}, std::optional<int64_t>{5}}) {
    SCOPED_TRACE(keep.value_or(-1));
    SortBuffer merged = buffer(keep);
    for (int64_t part = 0; part < 6; ++part) {
      SortBuffer rows_of_part = buffer(keep);
      AddChunks(rows_of_part, rows, part * 100, (part + 1) * 100, 7, rng);
      if (part % 2 == 1) {
        ASSERT_TRUE(rows_of_part.Sort(pool).ok());
      }
      ASSERT_TRUE(merged.Merge(rows_of_part, pool).ok());
      EXPECT_EQ(rows_of_part.num_rows(), 0);
      EXPECT_FALSE(rows_of_part.Slice(0, 1, pool).ok()) << "a merged part is unsorted";
      ASSERT_TRUE(rows_of_part.Sort(pool).ok());
      const auto none = rows_of_part.Slice(0, 5, pool);
      ASSERT_TRUE(none.ok()) << none.status().ToString();
      EXPECT_EQ((*none)->num_rows(), 0);
      if (keep.has_value()) {
        ASSERT_TRUE(merged.Tighten(pool).ok());
        std::vector<int64_t> order = KeyOrder(rows, (part + 1) * 100, false, false);
        order.resize(static_cast<std::size_t>(*keep));
        ExpectWindows(merged, rows, order);
      }
    }
    ASSERT_TRUE(merged.Sort(pool).ok());
    std::vector<int64_t> order = KeyOrder(rows, 600, false, false);
    order.resize(static_cast<std::size_t>(keep.value_or(600)));
    ExpectWindows(merged, rows, order);
  }
  for (const std::optional<int64_t> keep :
       {std::optional<int64_t>{}, std::optional<int64_t>{0}, std::optional<int64_t>{3}}) {
    SortBuffer empty = buffer(keep);
    ASSERT_TRUE(empty.Sort(pool).ok());
    const auto none = empty.Slice(0, 5, pool);
    ASSERT_TRUE(none.ok()) << none.status().ToString();
    EXPECT_EQ((*none)->num_rows(), 0);
    EXPECT_TRUE((*none)->schema()->Equals(*WideRows::Schema()));
  }
}

// A merge that fails part way -- no memory for the batch of the rows that beat the threshold --
// has still emptied the other buffer, which reads as unsorted and works as a new one. The chunk it
// gave was held by it alone and was freed with the failed Add, so reading it would read freed
// memory.
TEST_F(SortTest, AFailedMergeLeavesTheOtherBufferNew) {
  arrow::MemoryPool* pool = arrow::default_memory_pool();
  SortBuffer merged = IdKeyBuffer(3);
  ASSERT_TRUE(merged.Add(IdKeys(0, {10, 20, 30, 40}), pool).ok());
  ASSERT_TRUE(merged.Tighten(pool).ok());  // keeps 10, 20, 30: the threshold is 30
  SortBuffer other = IdKeyBuffer(3);
  ASSERT_TRUE(other.Add(IdKeys(4, {5, 50, 25, 60}), pool).ok());
  ASSERT_TRUE(other.Sort(pool).ok());  // one chunk: 5, 25, 50

  MemoryBudget no_memory(0);
  const arrow::Status failed = merged.Merge(other, &no_memory);
  EXPECT_TRUE(failed.IsOutOfMemory()) << failed.ToString();
  EXPECT_EQ(other.num_rows(), 0);
  EXPECT_FALSE(other.Slice(0, 3, pool).ok());
  ASSERT_TRUE(other.Add(IdKeys(8, {7, 1}), pool).ok());
  ASSERT_TRUE(other.Sort(pool).ok());
  EXPECT_EQ(IdsOf(other.Slice(0, 3, pool)), (Ids{9, 8}));
  ASSERT_TRUE(merged.Sort(pool).ok());
  EXPECT_EQ(IdsOf(merged.Slice(0, 3, pool)), (Ids{0, 1, 2}));
}

// A buffer is not merged into itself (Merge moves another buffer's rows behind its own, and would
// only add these back, unsorted): refused, the rows kept.
TEST_F(SortTest, ABufferDoesNotMergeIntoItself) {
  arrow::MemoryPool* pool = arrow::default_memory_pool();
  SortBuffer buffer = IdKeyBuffer(std::nullopt);
  ASSERT_TRUE(buffer.Add(IdKeys(0, {2, 1}), pool).ok());
  EXPECT_FALSE(buffer.Merge(buffer, pool).ok());
  EXPECT_EQ(buffer.num_rows(), 2);
  ASSERT_TRUE(buffer.Sort(pool).ok());
  EXPECT_EQ(IdsOf(buffer.Slice(0, 2, pool)), (Ids{1, 0}));
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
