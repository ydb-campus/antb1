// Key tables (key_table.h), each kind against what any key table must do: one dense id per
// distinct row of key values (NULL a value, whatever bytes lie under it), ids that never change,
// the new ids of a call in [before, after), uniques of exactly the key types, one row per id, that
// stay valid after later calls, and every byte given back to the budget with the table. The
// reference is the keys' own bytes, not another table, so every kind is held to the same contract;
// the GroupTable case checks that a GROUP BY gives the same rows with every kind.

#include "../key_table.h"

#include <algorithm>
#include <array>
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

#include "antb1/common/narrow.h"
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

constexpr std::array kKinds = {KeyTableKind::kAuto, KeyTableKind::kGrouper,
                               KeyTableKind::kPackedStrings};

std::string_view KindName(KeyTableKind kind) {
  switch (kind) {
    case KeyTableKind::kAuto:
      return "Auto";
    case KeyTableKind::kGrouper:
      return "Grouper";
    case KeyTableKind::kPackedStrings:
      return "PackedStrings";
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

// A row's key values as bytes that tell any two rows apart: per column a tag, then its value's
// bytes, or only a NULL tag whatever bytes lie under it. Variable-width values include their
// length, so embedded NULs and boundaries between adjacent columns cannot collide.
std::string KeyBytes(const arrow::ArrayVector& columns, std::int64_t row) {
  std::string key;
  for (const auto& column : columns) {
    if (column->IsNull(row)) {
      key += 'N';
      continue;
    }
    key += 'V';
    if (column->type_id() == arrow::Type::BINARY || column->type_id() == arrow::Type::STRING) {
      const auto value = static_cast<const arrow::BinaryArray&>(*column).GetView(row);
      const auto length = Narrow<std::uint64_t>(value.size());
      key.append(reinterpret_cast<const char*>(&length), sizeof(length));
      key.append(value);
      continue;
    }
    const int width = column->type()->byte_width();
    const auto* bytes = column->data()->GetValues<char>(1, (column->offset() + row) * width);
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

void LookupAndCheck(KeyTable& table, const arrow::compute::ExecBatch& batch,
                    const Expected& expected) {
  const auto found = table.Lookup(batch);
  ASSERT_TRUE(found.ok()) << found.status().ToString();
  ASSERT_EQ((*found)->length(), batch.length);
  const arrow::ArrayVector columns = ArraysOf(batch);
  for (std::int64_t row = 0; row < batch.length; ++row) {
    const auto known = expected.id_of.find(KeyBytes(columns, row));
    if (known == expected.id_of.end()) {
      EXPECT_TRUE((*found)->IsNull(row)) << "row " << row;
    } else {
      ASSERT_TRUE((*found)->IsValid(row)) << "row " << row;
      EXPECT_EQ((*found)->Value(row), known->second) << "row " << row;
    }
  }
  EXPECT_EQ(table.num_groups(), expected.key_of.size()) << "a lookup adds no group";
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

std::shared_ptr<arrow::Array> StringKeys(
    const std::vector<std::optional<std::string>>& values,
    const std::shared_ptr<arrow::DataType>& type = arrow::binary()) {
  if (type->id() == arrow::Type::STRING) {
    return testing::ArrayOf<arrow::StringBuilder>(type, values);
  }
  return testing::ArrayOf<arrow::BinaryBuilder>(type, values);
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

TEST_P(KeyTableTest, StringKeysKeepNullEmptyPrefixesAndEmbeddedNulsDistinct) {
  for (const auto& type : {arrow::binary(), arrow::utf8()}) {
    SCOPED_TRACE(type->ToString());
    const auto keys = StringKeys(
        {std::nullopt, "", std::string("\0", 1), std::string("\0\0", 2), "a", std::string("a\0", 2),
         "abcdef", "abcdefg", std::string("abcdef\0", 7), "abcdefg", std::nullopt, ""},
        type);
    Check({BatchOf({keys}), BatchOf({keys->Slice(3, 7)}),
           BatchOf({StringKeys(
               {"abcdefgh", "abcdefg", std::string("abcdefg\0", 8), "a", "", std::nullopt}, type)}),
           BatchOf({keys})});
  }
}

TEST_P(KeyTableTest, StringSlicesEmptyBatchesAndAllNullBatches) {
  const auto binary = StringKeys(
      {"discarded long prefix", "x", "", std::nullopt, "1234567", "ignored long suffix"});
  const auto utf8 =
      StringKeys({"discarded long prefix", "", "x", "1234567", std::nullopt, "ignored long suffix"},
                 arrow::utf8());
  Check({BatchOf({binary->Slice(1, 0), utf8->Slice(1, 0)}),
         BatchOf({StringKeys({std::nullopt, std::nullopt}),
                  StringKeys({std::nullopt, std::nullopt}, arrow::utf8())}),
         BatchOf({binary->Slice(1, 4), utf8->Slice(1, 4)}),
         BatchOf({binary->Slice(3, 0), utf8->Slice(3, 0)}),
         BatchOf({binary->Slice(2, 2), utf8->Slice(2, 2)})});
}

TEST_P(KeyTableTest, StringColumnBoundariesAndUnsupportedMixedSchemas) {
  const auto first = StringKeys({"a", "ab", "", "abc", std::nullopt, "", "a"});
  const auto second = StringKeys({"bc", "c", "abc", "", "", std::nullopt, "bc"}, arrow::utf8());
  const auto numbers = Int64s({1, 2, 3, 4, 5, 6, 1});
  Check({BatchOf({first, second}), BatchOf({first->Slice(1, 5), second->Slice(1, 5)})});
  Check({BatchOf({first, numbers}), BatchOf({first, numbers})});
  Check({BatchOf({first, second, first}), BatchOf({first, second, first})});
}

TEST_P(KeyTableTest, StringPromotionPreservesIdsAndLookupNeverAddsGroups) {
  const std::unique_ptr<KeyTable> table = MakeTable({arrow::binary(), arrow::utf8()});
  ASSERT_NE(table, nullptr);
  Expected expected;
  const auto initial = BatchOf({StringKeys({std::nullopt, "", "a", "abc", "1234567"}),
                                StringKeys({"", std::nullopt, "bc", "", "x"}, arrow::utf8())});
  ASSERT_NO_FATAL_FAILURE(ConsumeAndCheck(*table, initial, expected));
  ASSERT_NO_FATAL_FAILURE(ConsumeAndCheck(
      *table,
      BatchOf({StringKeys({"b", "a", ""}), StringKeys({"c", "bc", std::nullopt}, arrow::utf8())}),
      expected));
  const auto early = table->Uniques();
  ASSERT_TRUE(early.ok()) << early.status().ToString();
  const std::size_t early_groups = expected.key_of.size();
  const auto short_probe = BatchOf({StringKeys({"a", "missing", std::nullopt, ""}),
                                    StringKeys({"bc", "", "", std::nullopt}, arrow::utf8())});
  ASSERT_NO_FATAL_FAILURE(LookupAndCheck(*table, short_probe, expected));
  // A long lookup is unknown even where replacing its value by NULL would find a group.
  const auto long_probe =
      BatchOf({StringKeys({"unknown long value", "a", ""}),
               StringKeys({"", "long unknown value", std::nullopt}, arrow::utf8())});
  ASSERT_NO_FATAL_FAILURE(LookupAndCheck(*table, long_probe, expected));
  ASSERT_NO_FATAL_FAILURE(
      ConsumeAndCheck(*table,
                      BatchOf({StringKeys({"abcdefgh", "a", std::nullopt, ""}),
                               StringKeys({"x", "bc", "", "long second column"}, arrow::utf8())}),
                      expected));
  ASSERT_NO_FATAL_FAILURE(ConsumeAndCheck(*table, initial, expected));
  ASSERT_NO_FATAL_FAILURE(LookupAndCheck(*table, short_probe, expected));
  ASSERT_NO_FATAL_FAILURE(LookupAndCheck(*table, long_probe, expected));
  ASSERT_NO_FATAL_FAILURE(
      LookupAndCheck(*table,
                     BatchOf({StringKeys({"abcdefgh", "", "abcdefgh"}),
                              StringKeys({"x", "long second column", "missing"}, arrow::utf8())}),
                     expected));
  const auto uniques = table->Uniques();
  ASSERT_TRUE(uniques.ok()) << uniques.status().ToString();
  CheckUniques(*uniques, expected, TypesOf(initial), expected.key_of.size());
  CheckUniques(*early, expected, TypesOf(initial), early_groups);
}

TEST_P(KeyTableTest, StringIdsAndUniquesOwnTheirMemoryAcrossPromotionAndDestruction) {
  MemoryBudget budget(std::nullopt);
  {
    arrow::compute::ExecContext ctx(&budget);
    auto made = MakeKeyTable({arrow::binary()}, &ctx, {.kind = GetParam()});
    ASSERT_TRUE(made.ok()) << made.status().ToString();
    auto table = std::move(*made);
    const auto initial = BatchOf({StringKeys({"a", "", std::nullopt, "1234567", "a"})});
    Expected expected;
    ASSERT_NO_FATAL_FAILURE(ConsumeAndCheck(*table, initial, expected));
    const auto retained_ids = table->Consume(initial);
    ASSERT_TRUE(retained_ids.ok()) << retained_ids.status().ToString();
    const std::vector<std::uint32_t> copied_ids(retained_ids->ids.begin(), retained_ids->ids.end());
    const auto retained_uniques = table->Uniques();
    ASSERT_TRUE(retained_uniques.ok()) << retained_uniques.status().ToString();
    const std::size_t retained_groups = expected.key_of.size();
    ASSERT_NO_FATAL_FAILURE(ConsumeAndCheck(
        *table, BatchOf({StringKeys({"12345678", "a", "new longer value", ""})}), expected));
    ASSERT_NO_FATAL_FAILURE(ConsumeAndCheck(*table, initial, expected));
    table.reset();
    EXPECT_EQ(std::vector<std::uint32_t>(retained_ids->ids.begin(), retained_ids->ids.end()),
              copied_ids);
    CheckUniques(*retained_uniques, expected, TypesOf(initial), retained_groups);
    EXPECT_GT(budget.bytes_allocated(), 0);
  }
  EXPECT_EQ(budget.bytes_allocated(), 0);
}

class PackedStringKeyTableTest : public testing::ExecTest {};

TEST_F(PackedStringKeyTableTest, FailedPromotionPreservesKeysAndReleasesAllocations) {
  constexpr std::int64_t kLimit = std::int64_t{4} * 1024 * 1024;
  std::vector<std::optional<std::string>> first;
  std::vector<std::optional<std::string>> second;
  for (int i = 0; i < 256; ++i) {
    first.emplace_back(std::to_string(i));
    second.emplace_back("tag");
  }
  const auto initial = BatchOf({StringKeys(first), StringKeys(second, arrow::utf8())});
  const auto long_key = BatchOf({StringKeys({"a new long key"}), StringKeys({"z"}, arrow::utf8())});
  // A failed promotion keeps all existing keys intact. Disposable scratch may be released
  // before the failure; retrying after restoring budget headroom must still work.
  for (const std::int64_t spare : {0, 1024}) {
    SCOPED_TRACE("spare bytes " + std::to_string(spare));
    MemoryBudget budget(kLimit);
    {
      arrow::compute::ExecContext ctx(&budget);
      auto table = MakeKeyTable({arrow::binary(), arrow::utf8()}, &ctx,
                                {.kind = KeyTableKind::kPackedStrings});
      ASSERT_TRUE(table.ok()) << table.status().ToString();
      Expected expected;
      ASSERT_NO_FATAL_FAILURE(ConsumeAndCheck(**table, initial, expected));
      EXPECT_GT(budget.bytes_allocated(), 0);
      const std::int64_t reserved = kLimit - budget.bytes_allocated() - spare;
      ASSERT_TRUE(budget.Reserve(reserved).ok());
      const auto failed = (*table)->Consume(long_key);
      EXPECT_TRUE(failed.status().IsOutOfMemory()) << failed.status().ToString();
      EXPECT_NE(failed.status().message().find("memory limit"), std::string::npos);
      budget.Release(reserved);
      ASSERT_NO_FATAL_FAILURE(LookupAndCheck(**table, initial, expected));
      const auto uniques = (*table)->Uniques();
      ASSERT_TRUE(uniques.ok()) << uniques.status().ToString();
      CheckUniques(*uniques, expected, TypesOf(initial), expected.key_of.size());
      ASSERT_NO_FATAL_FAILURE(ConsumeAndCheck(**table, long_key, expected));
      ASSERT_NO_FATAL_FAILURE(LookupAndCheck(**table, initial, expected));
    }
    EXPECT_EQ(budget.bytes_allocated(), 0);
  }
}

TEST_F(PackedStringKeyTableTest, RetainedResultsSurviveScratchReuseGrowthAndDestruction) {
  MemoryBudget budget(std::nullopt);
  {
    arrow::compute::ExecContext ctx(&budget);
    auto made = MakeKeyTable({arrow::binary(), arrow::utf8()}, &ctx,
                             {.kind = KeyTableKind::kPackedStrings});
    ASSERT_TRUE(made.ok()) << made.status().ToString();
    auto table = std::move(*made);
    const auto initial = BatchOf({StringKeys({"a", "", std::nullopt, "1234567"}),
                                  StringKeys({"x", std::nullopt, "", "z"}, arrow::utf8())});
    Expected expected;
    ASSERT_NO_FATAL_FAILURE(ConsumeAndCheck(*table, initial, expected));
    const auto ids = table->Consume(initial);
    ASSERT_TRUE(ids.ok()) << ids.status().ToString();
    const std::vector<std::uint32_t> copied_ids(ids->ids.begin(), ids->ids.end());
    const auto probe = BatchOf(
        {StringKeys({"a", "other", std::nullopt}), StringKeys({"x", "value", ""}, arrow::utf8())});
    ASSERT_NO_FATAL_FAILURE(LookupAndCheck(*table, probe, expected));
    const auto lookup = table->Lookup(probe);
    ASSERT_TRUE(lookup.ok()) << lookup.status().ToString();
    const auto lookup_copy = (*lookup)->ToString();
    const auto uniques = table->Uniques();
    ASSERT_TRUE(uniques.ok()) << uniques.status().ToString();
    const auto initial_groups = expected.key_of.size();
    std::vector<std::optional<std::string>> a(1024, "other");
    std::vector<std::optional<std::string>> b(1024, "value");
    const auto larger = BatchOf({StringKeys(a), StringKeys(b, arrow::utf8())});
    const auto cached_growth = larger.Slice(1, 256);
    ASSERT_NO_FATAL_FAILURE(LookupAndCheck(*table, cached_growth, expected));
    ASSERT_NO_FATAL_FAILURE(ConsumeAndCheck(*table, cached_growth, expected));
    ASSERT_NO_FATAL_FAILURE(LookupAndCheck(*table, larger, expected));
    ASSERT_NO_FATAL_FAILURE(ConsumeAndCheck(*table, larger, expected));
    ASSERT_NO_FATAL_FAILURE(ConsumeAndCheck(*table, initial, expected));
    table.reset();
    EXPECT_EQ(std::vector<std::uint32_t>(ids->ids.begin(), ids->ids.end()), copied_ids);
    EXPECT_EQ((*lookup)->ToString(), lookup_copy);
    CheckUniques(*uniques, expected, TypesOf(initial), initial_groups);
    EXPECT_GT(budget.bytes_allocated(), 0);
  }
  EXPECT_EQ(budget.bytes_allocated(), 0);
}

TEST_F(PackedStringKeyTableTest, SmallScratchSurvivesLargeTemporariesAndLookupPromotionReleasesIt) {
  MemoryBudget budget(std::nullopt);
  {
    arrow::compute::ExecContext ctx(&budget);
    auto made = MakeKeyTable({arrow::binary(), arrow::utf8()}, &ctx,
                             {.kind = KeyTableKind::kPackedStrings});
    ASSERT_TRUE(made.ok()) << made.status().ToString();
    auto table = std::move(*made);
    const auto repeated = [](std::size_t rows) {
      const std::vector<std::optional<std::string>> a(rows, "a");
      const std::vector<std::optional<std::string>> b(rows, "b");
      return BatchOf({StringKeys(a), StringKeys(b, arrow::utf8())});
    };
    const auto small = BatchOf({StringKeys({"a"}), StringKeys({"b"}, arrow::utf8())});
    Expected expected;
    ASSERT_NO_FATAL_FAILURE(ConsumeAndCheck(*table, repeated(255), expected));
    ASSERT_NO_FATAL_FAILURE(LookupAndCheck(*table, repeated(255), expected));
    const auto at_limit = repeated(256);
    ASSERT_NO_FATAL_FAILURE(LookupAndCheck(*table, at_limit, expected));
    ASSERT_NO_FATAL_FAILURE(ConsumeAndCheck(*table, at_limit, expected));
    const auto cached_bytes = budget.bytes_allocated();
    // Only one group exists. Larger batches must not retain packing capacity, and must keep
    // the existing small cache available. All returned ID/lookup buffers are released here.
    for (const std::size_t rows : {std::size_t{257}, std::size_t{65536}}) {
      SCOPED_TRACE(rows);
      const auto large = repeated(rows);
      ASSERT_NO_FATAL_FAILURE(LookupAndCheck(*table, large, expected));
      EXPECT_EQ(budget.bytes_allocated(), cached_bytes);
      ASSERT_NO_FATAL_FAILURE(ConsumeAndCheck(*table, large, expected));
      EXPECT_EQ(budget.bytes_allocated(), cached_bytes);
    }
    ASSERT_NO_FATAL_FAILURE(LookupAndCheck(*table, small, expected));
    ASSERT_NO_FATAL_FAILURE(ConsumeAndCheck(*table, small, expected));
    EXPECT_EQ(budget.bytes_allocated(), cached_bytes);
    const auto long_probe =
        BatchOf({StringKeys({"a longer missing key"}), StringKeys({"b"}, arrow::utf8())});
    ASSERT_NO_FATAL_FAILURE(LookupAndCheck(*table, long_probe, expected));
    EXPECT_LT(budget.bytes_allocated(), cached_bytes);
    ASSERT_NO_FATAL_FAILURE(LookupAndCheck(*table, small, expected));
  }
  EXPECT_EQ(budget.bytes_allocated(), 0);
}

TEST_F(PackedStringKeyTableTest, FailedCachedOrTemporaryPackingPreservesKeysAndAllowsRetry) {
  constexpr std::int64_t kLimit = std::int64_t{4} * 1024 * 1024;
  for (const bool lookup : {false, true}) {
    SCOPED_TRACE(lookup ? "lookup" : "consume");
    for (const std::size_t rows : {std::size_t{256}, std::size_t{4096}}) {
      SCOPED_TRACE(rows);
      MemoryBudget budget(kLimit);
      {
        arrow::compute::ExecContext ctx(&budget);
        auto made = MakeKeyTable({arrow::binary(), arrow::utf8()}, &ctx,
                                 {.kind = KeyTableKind::kPackedStrings});
        ASSERT_TRUE(made.ok()) << made.status().ToString();
        auto table = std::move(*made);
        const auto initial = BatchOf(
            {StringKeys({"a", "", std::nullopt}), StringKeys({"x", "y", "z"}, arrow::utf8())});
        Expected expected;
        ASSERT_NO_FATAL_FAILURE(ConsumeAndCheck(*table, initial, expected));
        const std::vector<std::optional<std::string>> a(rows, "new");
        const std::vector<std::optional<std::string>> b(rows, "pair");
        const auto large = BatchOf({StringKeys(a), StringKeys(b, arrow::utf8())});
        const auto reserved = kLimit - budget.bytes_allocated();
        ASSERT_TRUE(budget.Reserve(reserved).ok());
        const arrow::Status failed =
            lookup ? table->Lookup(large).status() : table->Consume(large).status();
        EXPECT_TRUE(failed.IsOutOfMemory()) << failed.ToString();
        budget.Release(reserved);
        ASSERT_NO_FATAL_FAILURE(LookupAndCheck(*table, initial, expected));
        ASSERT_NO_FATAL_FAILURE(LookupAndCheck(*table, large, expected));
        ASSERT_NO_FATAL_FAILURE(ConsumeAndCheck(*table, large, expected));
        ASSERT_NO_FATAL_FAILURE(LookupAndCheck(*table, initial, expected));
      }
      EXPECT_EQ(budget.bytes_allocated(), 0);
    }
  }
}

// The rows of a GROUP BY (COUNT(*) and SUM per key), through part tables of the kind whose
// partitions merge into lane tables of the kind, and through one serial table of the kind: both
// are the keys' own counts and sums.
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
        const std::int64_t row =
            (Narrow<std::int64_t>(p) * 2 * kBatchRows) + (std::int64_t{b} * kBatchRows) + r;
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

  // Parallel: a table per part (keeping no first keys, as a part without a DOUBLE key does),
  // partitioned, merged in part order into a lane table per partition.
  std::vector<std::unique_ptr<GroupTable>> part_tables;
  for (const auto& batches : parts) {
    auto table = GroupTable::Make(keys, calls, 2, arrow::default_memory_pool(), nullptr,
                                  /*first_keys=*/false, GroupTable::kPartitions, kind);
    ASSERT_TRUE(table.ok()) << table.status().ToString();
    for (const auto& batch : batches) {
      ASSERT_TRUE((*table)->Consume(*batch).ok());
    }
    ASSERT_TRUE((*table)->Partition().ok());
    part_tables.push_back(std::move(*table));
  }
  std::vector<std::string> merged;
  for (std::size_t p = 0; p < GroupTable::kPartitions; ++p) {
    auto lane = GroupTable::Make(keys, calls, 2, arrow::default_memory_pool(), nullptr,
                                 /*first_keys=*/true, GroupTable::kPartitions, kind);
    ASSERT_TRUE(lane.ok()) << lane.status().ToString();
    for (const auto& part : part_tables) {
      ASSERT_TRUE((*lane)->MergePartition(*part, p).ok());
    }
    while (true) {
      const auto chunk = (*lane)->NextChunk(out);
      ASSERT_TRUE(chunk.ok()) << chunk.status().ToString();
      if (*chunk == nullptr) {
        break;
      }
      rows_of(**chunk, merged);
    }
  }
  std::ranges::sort(merged);
  EXPECT_EQ(merged, expected);

  // Serial: one table over every batch.
  auto serial = GroupTable::Make(keys, calls, 2, arrow::default_memory_pool(), nullptr,
                                 /*first_keys=*/true, GroupTable::kPartitions, kind);
  ASSERT_TRUE(serial.ok()) << serial.status().ToString();
  for (const auto& batches : parts) {
    for (const auto& batch : batches) {
      ASSERT_TRUE((*serial)->Consume(*batch).ok());
    }
  }
  std::vector<std::string> single;
  while (true) {
    const auto chunk = (*serial)->NextChunk(out);
    ASSERT_TRUE(chunk.ok()) << chunk.status().ToString();
    if (*chunk == nullptr) {
      break;
    }
    rows_of(**chunk, single);
  }
  std::ranges::sort(single);
  EXPECT_EQ(single, expected);
}

TEST_P(KeyTableTest, TwoStringGroupsMergeAfterPromotionWithTheirOriginalValues) {
  using StringPair = std::pair<std::optional<std::string>, std::optional<std::string>>;
  using Totals = std::map<StringPair, std::pair<std::int64_t, std::int64_t>>;
  using Results = std::map<StringPair, std::pair<std::int64_t, std::string>>;
  const std::vector<plan::BoundColumn> keys = {Column(0, "a", LogicalType::kVarchar),
                                               Column(1, "b", LogicalType::kVarchar)};
  const std::vector<plan::AggregateCall> calls = {
      {.kind = plan::AggKind::kCountStar, .arg = {}, .type = LogicalType::kBigInt},
      {.kind = plan::AggKind::kSum,
       .arg = Column(2, "v", LogicalType::kBigInt),
       .type = LogicalType::kHugeInt}};
  const auto schema =
      arrow::schema({arrow::field("a", arrow::binary()), arrow::field("b", arrow::binary()),
                     arrow::field("v", arrow::int64())});
  const auto out =
      arrow::schema({arrow::field("a", arrow::binary()), arrow::field("b", arrow::binary()),
                     arrow::field("count", arrow::int64()),
                     arrow::field("sum", plan::ToArrow(LogicalType::kHugeInt))});
  const KeyTableOptions kind{.kind = GetParam()};
  const std::array<std::optional<std::string>, 6> palette = {
      std::nullopt, "", "a", "ab", std::string("a\0", 2), "1234567"};
  std::vector<std::vector<std::shared_ptr<arrow::RecordBatch>>> parts(3);
  Totals totals;
  for (std::size_t p = 0; p < parts.size(); ++p) {
    for (std::size_t b = 0; b < 2; ++b) {
      std::vector<std::optional<std::string>> first;
      std::vector<std::optional<std::string>> second;
      std::vector<std::optional<std::int64_t>> values;
      for (std::size_t r = 0; r < 30; ++r) {
        auto a = palette[r % palette.size()];
        auto other = palette[((r / palette.size()) + b) % palette.size()];
        if (p == 1 && b == 1 && r == 0) {
          a = "a longer first key";
        }
        if (p == 2 && b == 1 && r == 1) {
          other = "a longer second key";
        }
        const auto value = Narrow<std::int64_t>(r) - 12;
        first.push_back(a);
        second.push_back(other);
        values.emplace_back(value);
        auto& [count, sum] = totals[{a, other}];
        ++count;
        sum += value;
      }
      parts[p].push_back(
          arrow::RecordBatch::Make(schema, Narrow<std::int64_t>(values.size()),
                                   {StringKeys(first), StringKeys(second), Int64s(values)}));
    }
  }
  Results expected;
  for (const auto& [key, total] : totals) {
    expected.emplace(key, std::make_pair(total.first, std::to_string(total.second)));
  }
  const auto collect = [&out](GroupTable& table, Results& results) {
    while (true) {
      const auto chunk = table.NextChunk(out);
      ASSERT_TRUE(chunk.ok()) << chunk.status().ToString();
      if (*chunk == nullptr) {
        return;
      }
      const auto& first = static_cast<const arrow::BinaryArray&>(*(*chunk)->column(0));
      const auto& second = static_cast<const arrow::BinaryArray&>(*(*chunk)->column(1));
      const auto& counts = static_cast<const arrow::Int64Array&>(*(*chunk)->column(2));
      for (std::int64_t r = 0; r < (*chunk)->num_rows(); ++r) {
        const StringPair key = {
            first.IsNull(r) ? std::nullopt : std::optional<std::string>(first.GetString(r)),
            second.IsNull(r) ? std::nullopt : std::optional<std::string>(second.GetString(r))};
        const auto sum = (*chunk)->column(3)->GetScalar(r);
        ASSERT_TRUE(sum.ok()) << sum.status().ToString();
        EXPECT_TRUE(
            results.emplace(key, std::make_pair(counts.Value(r), (*sum)->ToString())).second)
            << "one group appeared twice";
      }
    }
  };
  MemoryBudget budget(std::nullopt);
  {
    std::vector<std::unique_ptr<GroupTable>> part_tables;
    for (const auto& batches : parts) {
      auto part = GroupTable::Make(keys, calls, 3, &budget, &budget,
                                   /*first_keys=*/false, GroupTable::kPartitions, kind);
      ASSERT_TRUE(part.ok()) << part.status().ToString();
      for (const auto& batch : batches) {
        ASSERT_TRUE((*part)->Consume(*batch).ok());
      }
      ASSERT_TRUE((*part)->Partition().ok());
      part_tables.push_back(std::move(*part));
    }
    Results merged;
    for (std::size_t p = 0; p < GroupTable::kPartitions; ++p) {
      auto lane = GroupTable::Make(keys, calls, 3, &budget, &budget,
                                   /*first_keys=*/true, GroupTable::kPartitions, kind);
      ASSERT_TRUE(lane.ok()) << lane.status().ToString();
      for (const auto& part : part_tables) {
        ASSERT_TRUE((*lane)->MergePartition(*part, p).ok());
      }
      ASSERT_NO_FATAL_FAILURE(collect(**lane, merged));
    }
    EXPECT_EQ(merged, expected);
    auto serial = GroupTable::Make(keys, calls, 3, &budget, &budget,
                                   /*first_keys=*/true, GroupTable::kPartitions, kind);
    ASSERT_TRUE(serial.ok()) << serial.status().ToString();
    for (const auto& batches : parts) {
      for (const auto& batch : batches) {
        ASSERT_TRUE((*serial)->Consume(*batch).ok());
      }
    }
    Results single;
    ASSERT_NO_FATAL_FAILURE(collect(**serial, single));
    EXPECT_EQ(single, expected);
  }
  EXPECT_EQ(budget.bytes_allocated(), 0);
}

// Lookup (a set operation's inputs that only look up, docs/adr/0039-set-operations.md): a known
// row's id, null for a row no group has -- NULL keys and columns without a values buffer included
// -- and no group added.
TEST_P(KeyTableTest, LookupFindsKnownRowsAndAddsNone) {
  const std::unique_ptr<KeyTable> table = MakeTable({arrow::int64(), arrow::int64()});
  ASSERT_NE(table, nullptr);
  auto ids = table->Consume(BatchOf({Int64s({1, std::nullopt, 2}), Int64s({1, 1, std::nullopt})}));
  ASSERT_TRUE(ids.ok()) << ids.status().ToString();
  const std::vector<std::uint32_t> consumed(ids->ids.begin(), ids->ids.end());
  auto found = table->Lookup(
      BatchOf({Int64s({std::nullopt, 2, 1, 3, 2}), Int64s({1, std::nullopt, 1, 1, 2})}));
  ASSERT_TRUE(found.ok()) << found.status().ToString();
  const arrow::UInt32Array& got = **found;
  ASSERT_EQ(got.length(), 5);
  EXPECT_TRUE(got.IsValid(0));
  EXPECT_EQ(got.Value(0), consumed[1]);
  EXPECT_EQ(got.Value(1), consumed[2]);
  EXPECT_EQ(got.Value(2), consumed[0]);
  EXPECT_TRUE(got.IsNull(3));
  EXPECT_TRUE(got.IsNull(4));
  EXPECT_EQ(table->num_groups(), 3U) << "a lookup adds no group";
  // A column of NULLs without a values buffer: found where (NULL, NULL) is a group, else null.
  auto none = table->Lookup(BatchOf({NullsWithoutValues(2), NullsWithoutValues(2)}));
  ASSERT_TRUE(none.ok()) << none.status().ToString();
  EXPECT_EQ((*none)->null_count(), 2);
  ASSERT_TRUE(table->Consume(BatchOf({Int64s({std::nullopt}), Int64s({std::nullopt})})).ok());
  auto some = table->Lookup(BatchOf({NullsWithoutValues(2), NullsWithoutValues(2)}));
  ASSERT_TRUE(some.ok()) << some.status().ToString();
  EXPECT_EQ((*some)->null_count(), 0);
}

// GroupTable's side: Consume hands out the groups it computed when asked, and Lookup normalizes a
// DOUBLE key as Consume does, so -0.0 finds 0.0's group and any NaN finds NaN's.
TEST_P(KeyTableTest, GroupTableConsumeIdsAndLookupNormalizeDoubles) {
  MemoryBudget budget(std::nullopt);
  auto table = GroupTable::Make({Column(0, "d", LogicalType::kDouble)}, {}, 1, &budget, &budget,
                                /*first_keys=*/true, GroupTable::kPartitions, {.kind = GetParam()});
  ASSERT_TRUE(table.ok()) << table.status().ToString();
  const auto schema = arrow::schema({arrow::field("d", arrow::float64())});
  const auto doubles = [&schema](const std::vector<double>& values) {
    arrow::DoubleBuilder builder;
    EXPECT_TRUE(builder.AppendValues(values).ok());
    const auto n = static_cast<std::int64_t>(values.size());
    return arrow::RecordBatch::Make(schema, n, {builder.Finish().ValueOrDie()});
  };
  const double nan = std::numeric_limits<double>::quiet_NaN();
  std::vector<std::uint32_t> ids;
  ASSERT_TRUE((*table)->Consume(*doubles({0.0, nan, 1.5, 0.0}), &ids).ok());
  ASSERT_EQ(ids.size(), 4U);
  EXPECT_EQ(ids[0], ids[3]);
  EXPECT_EQ((*table)->num_groups(), 3U);
  auto found = (*table)->Lookup(*doubles({-0.0, -nan, 2.5}));
  ASSERT_TRUE(found.ok()) << found.status().ToString();
  EXPECT_EQ((*found)->Value(0), ids[0]);
  EXPECT_EQ((*found)->Value(1), ids[1]);
  EXPECT_TRUE((*found)->IsNull(2));
  EXPECT_EQ((*table)->num_groups(), 3U);
}

INSTANTIATE_TEST_SUITE_P(, KeyTableTest, ::testing::ValuesIn(kKinds), ParamName);

}  // namespace
}  // namespace antb1::exec
