// Key tables (key_table.h), each kind against what any key table must do: one dense id per
// distinct row of key values (NULL a value, whatever bytes lie under it), ids that never change,
// the new ids of a call in [before, after), uniques of exactly the key types, one row per id, that
// stay valid after later calls, and every byte given back to the budget with the table. The
// reference is the keys' own bytes, not another table, so every kind is held to the same contract;
// the GroupTable case checks that a GROUP BY gives the same rows with every kind.

#include "../key_table.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/compute/exec.h>
#include <gtest/gtest.h>

#include "antb1/exec/memory_budget.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

#include "../group_table.h"
#include "exec_test_util.h"

namespace antb1::exec {
namespace {

using plan::LogicalType;
using testing::Column;
using testing::Int32s;
using testing::Int64s;

constexpr std::array kKinds = {KeyTableKind::kGrouper, KeyTableKind::kFixed,
                               KeyTableKind::kFixedIdOrder};

std::string_view KindName(KeyTableKind kind) {
  switch (kind) {
    case KeyTableKind::kAuto:
      return "Auto";
    case KeyTableKind::kGrouper:
      return "Grouper";
    case KeyTableKind::kFixed:
      return "Fixed";
    case KeyTableKind::kFixedIdOrder:
      return "FixedIdOrder";
  }
  return "Unknown";
}

std::string ParamName(const ::testing::TestParamInfo<KeyTableKind>& param) {
  return std::string(KindName(param.param));
}

arrow::compute::ExecBatch BatchOf(arrow::ArrayVector columns) {
  const std::int64_t length = columns.empty() ? 0 : columns.front()->length();
  return {std::vector<arrow::Datum>(columns.begin(), columns.end()), length};
}

arrow::ArrayVector ArraysOf(const arrow::compute::ExecBatch& batch) {
  arrow::ArrayVector columns;
  columns.reserve(batch.values.size());
  for (const arrow::Datum& value : batch.values) {
    columns.push_back(value.make_array());
  }
  return columns;
}

std::vector<std::shared_ptr<arrow::DataType>> TypesOf(const arrow::compute::ExecBatch& batch) {
  std::vector<std::shared_ptr<arrow::DataType>> types;
  types.reserve(batch.values.size());
  for (const arrow::Datum& value : batch.values) {
    types.push_back(value.type()->GetSharedPtr());
  }
  return types;
}

// A row's key values as bytes that tell any two rows apart: per fixed-width column a tag, then
// its value's bytes, or only a NULL tag whatever bytes lie under it. One or two keys stay within a
// string's own small buffer.
std::string KeyBytes(const arrow::ArrayVector& columns, std::int64_t row) {
  std::string key;
  for (const auto& column : columns) {
    if (column->IsNull(row)) {
      key += 'N';
      continue;
    }
    const int width = column->type()->byte_width();
    const auto* bytes = column->data()->GetValues<char>(1, (column->offset() + row) * width);
    key += 'V';
    key.append(bytes, static_cast<std::size_t>(width));
  }
  return key;
}

// KeyBytes as text, for messages.
std::string Printable(std::string_view key) {
  constexpr std::string_view kHex = "0123456789abcdef";
  std::string text;
  for (const char c : key) {
    const auto byte = static_cast<std::size_t>(static_cast<unsigned char>(c));
    text += kHex[byte >> 4U];
    text += kHex[byte & 15U];
  }
  return text;
}

// The groups a table must have: each key's id, and each id's key.
struct Expected {
  std::unordered_map<std::string, std::uint32_t> id_of;
  std::vector<std::string> key_of;
};

// Consumes `batch` and checks every id against the keys seen so far: a known key keeps its id, and
// the call's new keys take the new ids [before, after), one each.
void ConsumeAndCheck(KeyTable& table, const arrow::compute::ExecBatch& batch, Expected& expected) {
  const auto before = static_cast<std::uint32_t>(expected.key_of.size());
  ASSERT_EQ(table.num_groups(), before);
  const auto ids = table.Consume(batch);
  ASSERT_TRUE(ids.ok()) << ids.status().ToString();
  ASSERT_EQ(ids->before, before);
  ASSERT_GE(ids->after, before);
  ASSERT_LE(ids->after - before, static_cast<std::uint64_t>(batch.length));
  ASSERT_EQ(ids->ids.size(), static_cast<std::size_t>(batch.length));
  expected.key_of.resize(ids->after);
  expected.id_of.reserve(ids->after);
  std::vector<bool> taken(ids->after - before, false);
  const arrow::ArrayVector columns = ArraysOf(batch);
  for (std::int64_t row = 0; row < batch.length; ++row) {
    std::string key = KeyBytes(columns, row);
    const std::uint32_t id = ids->ids[static_cast<std::size_t>(row)];
    ASSERT_LT(id, ids->after) << "row " << row;
    if (const auto known = expected.id_of.find(key); known != expected.id_of.end()) {
      ASSERT_EQ(id, known->second) << "row " << row << " key " << Printable(key);
      continue;
    }
    ASSERT_GE(id, before) << "row " << row << " key " << Printable(key) << " takes an old id";
    ASSERT_FALSE(taken[id - before]) << "two keys share the new id " << id;
    taken[id - before] = true;
    expected.key_of[id] = key;
    expected.id_of.emplace(std::move(key), id);
  }
  EXPECT_TRUE(std::ranges::all_of(taken, [](bool t) { return t; })) << "a new id without a key";
  EXPECT_EQ(table.num_groups(), ids->after);
}

// `uniques`: one row per group of `expected` (or its first `groups`), in id order, each its key,
// of exactly `types`.
void CheckUniques(const arrow::compute::ExecBatch& uniques, const Expected& expected,
                  const std::vector<std::shared_ptr<arrow::DataType>>& types, std::size_t groups) {
  ASSERT_EQ(uniques.length, static_cast<std::int64_t>(groups));
  const arrow::ArrayVector columns = ArraysOf(uniques);
  ASSERT_EQ(columns.size(), types.size());
  for (std::size_t c = 0; c < columns.size(); ++c) {
    EXPECT_TRUE(columns[c]->type()->Equals(*types[c]))
        << columns[c]->type()->ToString() << " for " << types[c]->ToString();
    ASSERT_EQ(columns[c]->length(), uniques.length) << "column " << c;
  }
  for (std::size_t g = 0; g < groups; ++g) {
    EXPECT_EQ(KeyBytes(columns, static_cast<std::int64_t>(g)), expected.key_of[g]) << "group " << g;
  }
}

// An int64 column whose NULL rows hold the bytes of `under` (which a NULL must not depend on).
std::shared_ptr<arrow::Array> Int64sOver(const std::vector<std::optional<std::int64_t>>& keys,
                                         const std::vector<std::optional<std::int64_t>>& under) {
  const auto valid = Int64s(keys);
  const auto values = Int64s(under);
  return arrow::MakeArray(arrow::ArrayData::Make(
      arrow::int64(), valid->length(), {valid->data()->buffers[0], values->data()->buffers[1]},
      valid->null_count()));
}

std::shared_ptr<arrow::Array> Doubles(const std::vector<std::optional<double>>& values) {
  return testing::ArrayOf<arrow::DoubleBuilder>(arrow::float64(), values);
}

// An int64 column of `rows` NULLs with no values buffer, which the Arrow format allows.
std::shared_ptr<arrow::Array> NullsWithoutValues(std::int64_t rows) {
  auto bitmap = arrow::AllocateBitmap(rows).ValueOrDie();
  std::fill_n(bitmap->mutable_data(), bitmap->size(), 0);
  return arrow::MakeArray(arrow::ArrayData::Make(arrow::int64(), rows, {bitmap, nullptr}, rows));
}

class KeyTableTest : public testing::ExecTest, public ::testing::WithParamInterface<KeyTableKind> {
 protected:
  // A table of the test's kind over `types`, allocating from `pool` (the fixture's budget unless
  // given).
  std::unique_ptr<KeyTable> MakeTable(const std::vector<std::shared_ptr<arrow::DataType>>& types,
                                      arrow::MemoryPool* pool = nullptr) {
    contexts_.push_back(
        std::make_unique<arrow::compute::ExecContext>(pool == nullptr ? &budget_ : pool));
    const std::vector<arrow::TypeHolder> holders(types.begin(), types.end());
    auto table = MakeKeyTable(holders, contexts_.back().get(), {.kind = GetParam()});
    EXPECT_TRUE(table.ok()) << table.status().ToString();
    return table.ok() ? std::move(*table) : nullptr;
  }

  // Every batch through one table, each call checked and the uniques after it. The first uniques
  // with groups are checked again at the end (they stay valid after later calls), and the table's
  // memory, charged to the budget while it lives, all goes back with it.
  void Check(const std::vector<arrow::compute::ExecBatch>& batches) {
    ASSERT_FALSE(batches.empty());
    const auto types = TypesOf(batches.front());
    {
      const std::unique_ptr<KeyTable> table = MakeTable(types);
      ASSERT_NE(table, nullptr);
      Expected expected;
      std::optional<arrow::compute::ExecBatch> early;
      std::size_t early_groups = 0;
      for (std::size_t b = 0; b < batches.size(); ++b) {
        SCOPED_TRACE("batch " + std::to_string(b));
        ASSERT_NO_FATAL_FAILURE(ConsumeAndCheck(*table, batches[b], expected));
        const auto uniques = table->Uniques();
        ASSERT_TRUE(uniques.ok()) << uniques.status().ToString();
        ASSERT_NO_FATAL_FAILURE(CheckUniques(*uniques, expected, types, expected.key_of.size()));
        if (!early.has_value() && !expected.key_of.empty()) {
          early = *uniques;
          early_groups = expected.key_of.size();
        }
      }
      if (early.has_value()) {
        SCOPED_TRACE("the first uniques, after the last call");
        CheckUniques(*early, expected, types, early_groups);
      }
      EXPECT_GT(budget_.bytes_allocated(), 0);
    }
    EXPECT_EQ(budget_.bytes_allocated(), 0);
  }

 private:
  MemoryBudget budget_{std::nullopt};                                   // outlives what follows
  std::vector<std::unique_ptr<arrow::compute::ExecContext>> contexts_;  // outlive the tables
};

// A key NULL in one batch and present in the next, at the same row: NULL is one group of its own,
// apart from 0.
TEST_P(KeyTableTest, KeysKeepTheirIdsAcrossBatchesWithNulls) {
  Check({
      BatchOf({Int64s({1, std::nullopt, 2, 1})}),
      BatchOf({Int64s({0, 1, std::nullopt, 3})}),
      BatchOf({Int64s({std::nullopt, 0, 4, 4})}),
  });
  Check({
      BatchOf({Int64s({1, std::nullopt, 0}), Int32s({std::nullopt, 0, 0})}),
      BatchOf({Int64s({std::nullopt, 1, 0}), Int32s({0, std::nullopt, 0})}),
      BatchOf({Int64s({std::nullopt, std::nullopt, 0}), Int32s({std::nullopt, 0, std::nullopt})}),
  });
}

// The bytes under a NULL are arbitrary: two NULLs over different bytes are one group, and a NULL
// over a value's bytes is not that value's group, in either of two keys.
TEST_P(KeyTableTest, NullsAreOneKeyWhateverTheirBytes) {
  Check({
      BatchOf({Int64sOver({5, std::nullopt, std::nullopt, 0}, {5, 5, 9, 0})}),
      BatchOf({Int64sOver({std::nullopt, 9, 0, std::nullopt}, {0, 9, 0, 123456789})}),
  });
  Check({
      BatchOf({Int64sOver({0, std::nullopt, 0, std::nullopt}, {0, 7, 0, 0}),
               Int64sOver({0, 0, std::nullopt, std::nullopt}, {0, 0, 7, 3})}),
      BatchOf({Int64sOver({std::nullopt, 0, std::nullopt, 7}, {0, 0, 7, 7}),
               Int64sOver({0, std::nullopt, std::nullopt, 0}, {0, 7, 0, 0})}),
  });
}

// A fixed-width column with no rows, or with only NULLs, may carry no values buffer at all, alone
// or beside a column that has one; and a table with no groups has empty uniques.
TEST_P(KeyTableTest, ColumnsWithoutAValuesBuffer) {
  const auto no_rows =
      arrow::MakeArray(arrow::ArrayData::Make(arrow::int64(), 0, {nullptr, nullptr}, 0));
  const auto all_null = NullsWithoutValues(8);
  ASSERT_EQ(all_null->data()->buffers[1], nullptr);
  Check({BatchOf({no_rows}), BatchOf({all_null}), BatchOf({Int64s({std::nullopt, 1})}),
         BatchOf({no_rows})});
  const auto other = Int64s({1, 2, 1, 2, 3, 3, std::nullopt, 2});
  Check({BatchOf({all_null, other}), BatchOf({other, all_null}), BatchOf({all_null, all_null}),
         BatchOf({Int64s({std::nullopt, 1}), Int64s({1, std::nullopt})})});
}

// Routed rows and a partition's uniques are slices: the offset applies to the values and to the
// validity of each column, which may differ from column to column.
TEST_P(KeyTableTest, SlicedColumnsWithNulls) {
  std::vector<std::optional<std::int64_t>> wide;
  std::vector<std::optional<std::int32_t>> narrow;
  wide.reserve(24);
  narrow.reserve(24);
  for (int i = 0; i < 24; ++i) {
    wide.emplace_back(i % 5 == 2 ? std::nullopt : std::optional<std::int64_t>(i % 4));
    narrow.emplace_back(i % 7 == 3 ? std::nullopt : std::optional<std::int32_t>(i % 3));
  }
  const auto k = Int64s(wide);
  const auto j = Int32s(narrow);
  std::vector<arrow::compute::ExecBatch> batches;
  for (const std::int64_t offset : {1, 3, 5}) {
    batches.push_back(BatchOf({k->Slice(offset, 11), j->Slice(offset, 11)}));
  }
  batches.push_back(BatchOf({k->Slice(1, 9), j->Slice(6, 9)}));
  batches.push_back(BatchOf({k->Slice(13, 7), j->Slice(2, 7)}));
  Check(batches);
}

// Every fixed-width type plan::ToArrow gives a GROUP BY key (BOOLEAN, which only conditions have,
// never is one), DOUBLE as GroupTable hands it over (normalized), each with repeats and a NULL.
TEST_P(KeyTableTest, EveryFixedWidthKeyType) {
  constexpr auto kTwoToThe40 = std::int64_t{1'099'511'627'776};
  constexpr auto kTwoToThe50 = std::int64_t{1'125'899'906'842'624};
  const std::vector<std::pair<std::string, std::vector<arrow::compute::ExecBatch>>> cases = {
      {"int16",
       {BatchOf({testing::Int16s({3, std::nullopt, -1, 3})}),
        BatchOf({testing::Int16s({-1, 0, std::nullopt, 32767})})}},
      {"uint16",
       {BatchOf({testing::UInt16s({3, std::nullopt, 65535, 3})}),
        BatchOf({testing::UInt16s({0, 65535, std::nullopt, 1})})}},
      {"int32",
       {BatchOf({Int32s({3, std::nullopt, -1, 3})}),
        BatchOf({Int32s({-2147483647, 0, std::nullopt, 3})})}},
      {"date32",
       {BatchOf({testing::Dates({19000, std::nullopt, 0, 19000})}),
        BatchOf({testing::Dates({-1, 0, std::nullopt, 20000})})}},
      {"int64",
       {BatchOf({Int64s({3, std::nullopt, -1, 3})}),
        BatchOf({Int64s({-1, kTwoToThe40, std::nullopt, 3})})}},
      {"timestamp",
       {BatchOf({testing::Timestamps({1, std::nullopt, -1, 1})}),
        BatchOf({testing::Timestamps({0, 1, std::nullopt, kTwoToThe50})})}},
      {"float64",
       {BatchOf({Doubles({1.5, std::nullopt, -2.0, 1.5})}),
        BatchOf({Doubles({0.0, -2.0, std::nullopt, 1e300})})}},
      {"decimal64",
       {BatchOf({testing::Decimals(15, 2, {"1234", std::nullopt, "-1", "1234"})}),
        BatchOf({testing::Decimals(15, 2, {"-1", "0", std::nullopt, "999999999999999"})})}},
      {"decimal128",
       {BatchOf({testing::Decimals(38, 0, {"1", std::nullopt, "-99999999999999999999", "1"})}),
        BatchOf({testing::Decimals(38, 0, {"0", "1", std::nullopt, "12345678901234567890123"})})}},
      {"decimal128 with a scale",
       {BatchOf({testing::Decimals(20, 3, {"1000", std::nullopt, "-5", "1000"})}),
        BatchOf({testing::Decimals(20, 3, {"-5", "0", std::nullopt, "1"})})}},
  };
  for (const auto& [name, batches] : cases) {
    SCOPED_TRACE(name);
    Check(batches);
  }
}

// Two keys that differ only in the second column, and pairs whose columns trade values: the
// second key separates the groups, and no pair is its swap.
TEST_P(KeyTableTest, TwoKeysThatDifferInOneColumn) {
  const std::vector<std::optional<std::int64_t>> seven(100, 7);
  const std::vector<std::optional<std::int32_t>> seven_narrow(100, 7);
  std::vector<std::optional<std::int64_t>> wide;
  std::vector<std::optional<std::int32_t>> narrow;
  wide.reserve(100);
  narrow.reserve(100);
  for (int i = 0; i < 100; ++i) {
    wide.emplace_back(i);
    narrow.emplace_back(i);
  }
  Check({BatchOf({Int64s(seven), Int32s(narrow)}), BatchOf({Int64s(wide), Int32s(seven_narrow)}),
         BatchOf({Int64s(wide), Int32s(narrow)})});
  Check({BatchOf({testing::Decimals(38, 0, {"1", "1", std::nullopt, "2"}),
                  testing::Int16s({1, 2, 1, std::nullopt})}),
         BatchOf({testing::Decimals(38, 0, {"2", std::nullopt, "1", "1"}),
                  testing::Int16s({std::nullopt, 1, 2, 3})})});
}

// More than 64 Ki new groups in one call, then batches of 64 Ki rows that mix old and new keys:
// the table grows many times over and keeps every id.
TEST_P(KeyTableTest, ManyGroupsInOneCallAndAcrossCalls) {
  constexpr std::uint64_t kScramble = 0x9E3779B97F4A7C15ULL;  // odd: a bijection modulo 2^64
  constexpr std::uint64_t kFirst = 70000;                     // more than 64 Ki
  constexpr std::uint64_t kBatch = 65536;
  const auto key = [](std::uint64_t i) { return static_cast<std::int64_t>(i * kScramble); };
  std::vector<std::optional<std::int64_t>> first;
  first.reserve(kFirst);
  for (std::uint64_t i = 0; i < kFirst; ++i) {
    first.emplace_back(key(i));
  }
  std::vector<arrow::compute::ExecBatch> batches = {BatchOf({Int64s(first)})};
  for (std::uint64_t b = 0; b < 2; ++b) {
    std::vector<std::optional<std::int64_t>> mixed;
    mixed.reserve(kBatch);
    for (std::uint64_t i = 0; i < kBatch; ++i) {
      // Every other row an old key, the rest new ones; one NULL a batch.
      const std::uint64_t id = i % 2 == 0 ? ((i * 7) + b) % kFirst : kFirst + (b * kBatch) + i;
      mixed.emplace_back(i == 77 ? std::nullopt : std::optional(key(id)));
    }
    batches.push_back(BatchOf({Int64s(mixed)}));
  }
  Check(batches);

  std::vector<std::optional<std::int64_t>> wide;
  std::vector<std::optional<std::int32_t>> narrow;
  wide.reserve(kFirst);
  narrow.reserve(kFirst);
  for (std::uint64_t i = 0; i < kFirst; ++i) {
    wide.emplace_back(key(i / 1024));
    narrow.emplace_back(static_cast<std::int32_t>(i % 1024));
  }
  Check({BatchOf({Int64s(wide), Int32s(narrow)}), BatchOf({Int64s(wide), Int32s(narrow)})});
}

// DOUBLE keys through a GroupTable, which normalizes them for its key table (double_key.h): -0.0
// groups with 0.0 and every NaN with every other NaN, as in DuckDB, and a group keeps the value of
// its first row as it came (-0.0, the first NaN's bits).
TEST_P(KeyTableTest, DoubleKeysGroupAsNormalizedThroughAGroupTable) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double payload = std::bit_cast<double>(std::uint64_t{0x7ff8000000000123ULL});
  const double negative = std::bit_cast<double>(std::uint64_t{0xfff8000000000000ULL});
  const auto schema = arrow::schema({arrow::field("d", arrow::float64())});
  const auto out =
      arrow::schema({arrow::field("d", arrow::float64()), arrow::field("count", arrow::int64())});
  auto table = GroupTable::Make(
      {Column(0, "d", LogicalType::kDouble)},
      {{.kind = plan::AggKind::kCountStar, .arg = {}, .type = LogicalType::kBigInt}}, 1,
      arrow::default_memory_pool(), nullptr, /*first_keys=*/true, GroupTable::kPartitions,
      {.kind = GetParam()});
  ASSERT_TRUE(table.ok()) << table.status().ToString();
  for (const auto& values :
       std::vector<std::vector<std::optional<double>>>{{-0.0, nan, 1.5, 0.0, payload, std::nullopt},
                                                       {0.0, negative, std::nullopt, 1.5, -0.0}}) {
    const auto batch = arrow::RecordBatch::Make(schema, static_cast<std::int64_t>(values.size()),
                                                {Doubles(values)});
    ASSERT_TRUE((*table)->Consume(*batch).ok());
  }
  std::vector<std::string> rows;
  while (true) {
    const auto chunk = (*table)->NextChunk(out);
    ASSERT_TRUE(chunk.ok()) << chunk.status().ToString();
    if (*chunk == nullptr) {
      break;
    }
    const auto& keys = static_cast<const arrow::DoubleArray&>(*(*chunk)->column(0));
    const auto& counts = static_cast<const arrow::Int64Array&>(*(*chunk)->column(1));
    for (std::int64_t r = 0; r < (*chunk)->num_rows(); ++r) {
      rows.push_back((keys.IsNull(r) ? std::string("NULL")
                                     : Printable(std::string(
                                           reinterpret_cast<const char*>(keys.raw_values() + r),
                                           sizeof(double)))) +
                     "|" + std::to_string(counts.Value(r)));
    }
  }
  std::ranges::sort(rows);
  const auto bytes_of = [](double value) {
    return Printable(std::string(reinterpret_cast<const char*>(&value), sizeof(value)));
  };
  std::vector<std::string> expected = {bytes_of(-0.0) + "|4", bytes_of(nan) + "|3",
                                       bytes_of(1.5) + "|2", "NULL|2"};
  std::ranges::sort(expected);
  EXPECT_EQ(rows, expected);
}

// KeyHashes of a group's uniques row is the hash of each of its input rows: the partition of a
// group (Partition) and of its rows (RouteRows) agree.
TEST_P(KeyTableTest, KeyHashesOfUniquesAreTheHashesOfTheirRows) {
  const std::vector<arrow::compute::ExecBatch> batches = {
      BatchOf({Int64s({1, std::nullopt, 2, 1, 0}), testing::Dates({5, 5, std::nullopt, 5, 0})}),
      BatchOf({Int64s({2, 1, std::nullopt, 3, 0}), testing::Dates({std::nullopt, 5, 5, 0, 0})}),
  };
  const auto types = TypesOf(batches.front());
  const std::unique_ptr<KeyTable> table = MakeTable(types);
  ASSERT_NE(table, nullptr);
  Expected expected;
  for (const auto& batch : batches) {
    ASSERT_NO_FATAL_FAILURE(ConsumeAndCheck(*table, batch, expected));
  }
  const auto uniques = table->Uniques();
  ASSERT_TRUE(uniques.ok()) << uniques.status().ToString();
  const auto group_hashes = KeyHashes(*uniques);
  ASSERT_TRUE(group_hashes.ok()) << group_hashes.status().ToString();
  ASSERT_EQ(group_hashes->size(), expected.key_of.size());
  for (std::size_t b = 0; b < batches.size(); ++b) {
    SCOPED_TRACE("batch " + std::to_string(b));
    const auto row_hashes = KeyHashes(batches[b]);
    ASSERT_TRUE(row_hashes.ok()) << row_hashes.status().ToString();
    const arrow::ArrayVector columns = ArraysOf(batches[b]);
    for (std::int64_t row = 0; row < batches[b].length; ++row) {
      const std::uint32_t id = expected.id_of.at(KeyBytes(columns, row));
      EXPECT_EQ((*row_hashes)[static_cast<std::size_t>(row)], (*group_hashes)[id]) << "row " << row;
    }
  }
}

// A budget too small for the table fails it with the budget's own OutOfMemory, when the table is
// made or when it first grows, and leaves nothing charged once the table is gone.
TEST_P(KeyTableTest, ATinyBudgetFailsWithItsOwnErrorAndLeavesNothing) {
  constexpr std::int64_t kRows = 50000;
  std::vector<std::optional<std::int64_t>> keys;
  keys.reserve(kRows);
  for (std::int64_t i = 0; i < kRows; ++i) {
    keys.emplace_back(i * 7);
  }
  const auto batch = BatchOf({Int64s(keys)});
  MemoryBudget budget(512);
  {
    arrow::compute::ExecContext ctx(&budget);
    auto table = MakeKeyTable({arrow::int64()}, &ctx, {.kind = GetParam()});
    arrow::Status failure = table.status();
    if (table.ok()) {
      failure = (*table)->Consume(batch).status();
    }
    EXPECT_TRUE(failure.IsOutOfMemory()) << failure.ToString();
    EXPECT_NE(failure.message().find("memory limit"), std::string::npos) << failure.ToString();
  }
  EXPECT_EQ(budget.bytes_allocated(), 0);
}

// The rows of a GROUP BY (COUNT(*) and SUM per key), through part tables of the kind whose
// partitions merge into lane tables of the kind, and through one serial table of the kind: both
// are the keys' own counts and sums, in chunks of the sizes the Grouper's tables make.
TEST_P(KeyTableTest, AGroupByGivesTheSameRowsWithEveryKind) {
  const std::vector<plan::BoundColumn> keys = {Column(0, "k", LogicalType::kBigInt)};
  const std::vector<plan::AggregateCall> calls = {
      {.kind = plan::AggKind::kCountStar, .arg = {}, .type = LogicalType::kBigInt},
      {.kind = plan::AggKind::kSum,
       .arg = Column(1, "v", LogicalType::kBigInt),
       .type = LogicalType::kHugeInt}};
  const auto schema =
      arrow::schema({arrow::field("k", arrow::int64()), arrow::field("v", arrow::int64())});
  const auto out =
      arrow::schema({arrow::field("k", arrow::int64()), arrow::field("count", arrow::int64()),
                     arrow::field("sum", plan::ToArrow(LogicalType::kHugeInt))});
  const KeyTableOptions kind{.kind = GetParam()};

  // 6 parts of 2 batches; keys repeat across parts, every 11th is NULL.
  constexpr int kBatchRows = 200;
  std::vector<std::vector<std::shared_ptr<arrow::RecordBatch>>> parts(6);
  std::map<std::string, std::pair<std::int64_t, std::int64_t>> totals;  // key: count, sum
  for (std::size_t p = 0; p < parts.size(); ++p) {
    for (int b = 0; b < 2; ++b) {
      std::vector<std::optional<std::int64_t>> k;
      std::vector<std::optional<std::int64_t>> v;
      k.reserve(kBatchRows);
      v.reserve(kBatchRows);
      for (int r = 0; r < kBatchRows; ++r) {
        const auto row = static_cast<std::int64_t>((p * 2 * kBatchRows) +
                                                   static_cast<std::size_t>((b * kBatchRows) + r));
        const std::optional<std::int64_t> key =
            row % 11 == 0 ? std::nullopt : std::optional<std::int64_t>((row * 13) % 97);
        k.push_back(key);
        v.emplace_back(r - 50);
        auto& [count, sum] = totals[key.has_value() ? std::to_string(*key) : "NULL"];
        ++count;
        sum += r - 50;
      }
      parts[p].push_back(arrow::RecordBatch::Make(schema, kBatchRows, {Int64s(k), Int64s(v)}));
    }
  }
  std::vector<std::string> expected;
  expected.reserve(totals.size());
  for (const auto& [key, total] : totals) {
    expected.push_back(key + "|" + std::to_string(total.first) + "|" +
                       std::to_string(total.second));
  }
  std::ranges::sort(expected);

  // A batch's rows as "key|count|sum".
  const auto rows_of = [](const arrow::RecordBatch& batch, std::vector<std::string>& rows) {
    for (std::int64_t r = 0; r < batch.num_rows(); ++r) {
      const auto key = batch.column(0)->GetScalar(r).ValueOrDie();
      rows.push_back((key->is_valid ? key->ToString() : std::string("NULL")) + "|" +
                     batch.column(1)->GetScalar(r).ValueOrDie()->ToString() + "|" +
                     batch.column(2)->GetScalar(r).ValueOrDie()->ToString());
    }
  };

  // The rows a GROUP BY gives with `kind`, and the sizes of the chunks NextChunk returns, through
  // part tables (keeping no first keys, as a part without a DOUBLE key does) partitioned and merged
  // in part order into a lane table per partition, and through one serial table.
  struct Run {
    std::vector<std::string> merged;
    std::vector<std::int64_t> merged_chunks;
    std::vector<std::string> single;
    std::vector<std::int64_t> single_chunks;
  };
  const auto drain = [&](GroupTable& table, std::vector<std::string>& rows,
                         std::vector<std::int64_t>& chunks) {
    while (true) {
      const auto chunk = table.NextChunk(out);
      ASSERT_TRUE(chunk.ok()) << chunk.status().ToString();
      if (*chunk == nullptr) {
        return;
      }
      chunks.push_back((*chunk)->num_rows());
      rows_of(**chunk, rows);
    }
  };
  const auto run = [&](KeyTableKind kind_of_run, Run& result) {
    const KeyTableOptions options{.kind = kind_of_run};
    std::vector<std::unique_ptr<GroupTable>> part_tables;
    for (const auto& batches : parts) {
      auto table = GroupTable::Make(keys, calls, 2, arrow::default_memory_pool(), nullptr,
                                    /*first_keys=*/false, GroupTable::kPartitions, options);
      ASSERT_TRUE(table.ok()) << table.status().ToString();
      for (const auto& batch : batches) {
        ASSERT_TRUE((*table)->Consume(*batch).ok());
      }
      ASSERT_TRUE((*table)->Partition().ok());
      part_tables.push_back(std::move(*table));
    }
    for (std::size_t p = 0; p < GroupTable::kPartitions; ++p) {
      auto lane = GroupTable::Make(keys, calls, 2, arrow::default_memory_pool(), nullptr,
                                   /*first_keys=*/true, GroupTable::kPartitions,
                                   {.kind = kind_of_run, .estimate = false});
      ASSERT_TRUE(lane.ok()) << lane.status().ToString();
      for (const auto& part : part_tables) {
        ASSERT_TRUE((*lane)->MergePartition(*part, p).ok());
      }
      ASSERT_NO_FATAL_FAILURE(drain(**lane, result.merged, result.merged_chunks));
    }
    std::ranges::sort(result.merged);
    auto serial = GroupTable::Make(keys, calls, 2, arrow::default_memory_pool(), nullptr,
                                   /*first_keys=*/true, GroupTable::kPartitions, options);
    ASSERT_TRUE(serial.ok()) << serial.status().ToString();
    for (const auto& batches : parts) {
      for (const auto& batch : batches) {
        ASSERT_TRUE((*serial)->Consume(*batch).ok());
      }
    }
    ASSERT_NO_FATAL_FAILURE(drain(**serial, result.single, result.single_chunks));
    std::ranges::sort(result.single);
  };
  Run ours;
  ASSERT_NO_FATAL_FAILURE(run(kind.kind, ours));
  EXPECT_EQ(ours.merged, expected);
  EXPECT_EQ(ours.single, expected);
  // A chunk is the groups one call made, whichever table numbers them.
  Run grouper;
  ASSERT_NO_FATAL_FAILURE(run(KeyTableKind::kGrouper, grouper));
  EXPECT_EQ(ours.merged_chunks, grouper.merged_chunks);
  EXPECT_EQ(ours.single_chunks, grouper.single_chunks);
}

INSTANTIATE_TEST_SUITE_P(, KeyTableTest, ::testing::ValuesIn(kKinds), ParamName);

}  // namespace
}  // namespace antb1::exec
