// FixedKeyTable (fixed_key_table.h) on what the differential cases of key_table_test.cc cannot
// see: which key shapes it takes, probes that collide and wrap through the mirrored control bytes,
// ids in first-seen order, allocations that follow the groups rather than the rows, the group
// limit, and the size estimate. Every case runs both growth policies.

#include "../fixed_key_table.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/compute/api_vector.h>
#include <arrow/compute/exec.h>
#include <gtest/gtest.h>

#include "antb1/exec/memory_budget.h"

#include "../column_key_table.h"
#include "../group_table.h"
#include "../key_table.h"
#include "exec_test_util.h"

namespace antb1::exec {
namespace {

using testing::Int32s;
using testing::Int64s;

// The first word of a key as its hash: a key's home slot is its value's low bits and its tag its
// top seven, so a case chooses where each probe starts and which tags meet.
struct ValueHash {
  template <std::size_t kWords>
  static std::uint64_t Hash(const std::array<std::uint64_t, kWords>& words) {
    return words[0];
  }
};

// NULL payload bytes may be uninitialized. Neither consume nor lookup may hash them.
constexpr std::int64_t kNullPayload = -8'765'432'109'876'543'210LL;
struct NullPayloadHash {
  template <std::size_t kWords>
  static std::uint64_t Hash(const std::array<std::uint64_t, kWords>& words) {
    EXPECT_NE(words[0], std::bit_cast<std::uint64_t>(kNullPayload));
    return FoldHash::Hash(words);
  }
};

// One hash for every key: every probe meets every group.
struct ConstantHash {
  template <std::size_t kWords>
  static std::uint64_t Hash(const std::array<std::uint64_t, kWords>& /*words*/) {
    return 0x0123456789abcdefULL;
  }
};

arrow::compute::ExecBatch BatchOf(arrow::ArrayVector columns) {
  const std::int64_t length = columns.empty() ? 0 : columns.front()->length();
  return {std::vector<arrow::Datum>(columns.begin(), columns.end()), length};
}

// The ids a key table gives `batch`; empty (with a failure) when the call fails.
std::vector<std::uint32_t> IdsOf(KeyTable& table, const arrow::compute::ExecBatch& batch) {
  const auto ids = table.Consume(batch);
  EXPECT_TRUE(ids.ok()) << ids.status().ToString();
  if (!ids.ok()) {
    return {};
  }
  return {ids->ids.begin(), ids->ids.end()};
}

// The ids of one-key rows numbered in the order their first rows come, NULL a key of its own.
class FirstSeen {
 public:
  std::vector<std::uint32_t> IdsOf(const std::vector<std::optional<std::int64_t>>& keys) {
    std::vector<std::uint32_t> ids;
    ids.reserve(keys.size());
    for (const auto& key : keys) {
      ids.push_back(ids_.try_emplace(key, static_cast<std::uint32_t>(ids_.size())).first->second);
    }
    return ids;
  }
  [[nodiscard]] std::uint32_t groups() const { return static_cast<std::uint32_t>(ids_.size()); }

 private:
  std::map<std::optional<std::int64_t>, std::uint32_t> ids_;
};

// `rows` keys, row r's key a fixed scramble of r modulo `groups`: ids that repeat at random.
std::vector<std::optional<std::int64_t>> RandomKeys(std::uint64_t first_row, std::size_t rows,
                                                    std::uint64_t groups) {
  std::vector<std::optional<std::int64_t>> keys;
  keys.reserve(rows);
  for (std::uint64_t r = first_row; r < first_row + rows; ++r) {
    std::uint64_t z = (r + 1) * 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    keys.emplace_back(static_cast<std::int64_t>((z ^ (z >> 31U)) % groups));
  }
  return keys;
}

// Keys from..to-1 in order.
std::vector<std::optional<std::int64_t>> KeysFrom(std::int64_t from, std::int64_t to) {
  std::vector<std::optional<std::int64_t>> keys;
  keys.reserve(static_cast<std::size_t>(to - from));
  for (std::int64_t key = from; key < to; ++key) {
    keys.emplace_back(key);
  }
  return keys;
}

FixedKeyLayout LayoutOf(const std::vector<arrow::TypeHolder>& types) {
  const std::optional<FixedKeyLayout> layout = FixedKeyLayoutOf(types);
  EXPECT_TRUE(layout.has_value());
  return layout.value_or(FixedKeyLayout{});
}

// The one and two-key shapes with a packed layout, at the key widths step 0 measured.
TEST(FixedKeyLayoutTest, TakesOneOrTwoFixedWidthKeys) {
  struct Case {
    std::vector<arrow::TypeHolder> types;
    std::size_t key_bytes;
  };
  const std::vector<Case> taken = {
      {.types = {arrow::int16()}, .key_bytes = 8},
      {.types = {arrow::uint16()}, .key_bytes = 8},
      {.types = {arrow::int32()}, .key_bytes = 8},
      {.types = {arrow::date32()}, .key_bytes = 8},
      {.types = {arrow::int64()}, .key_bytes = 8},
      {.types = {arrow::timestamp(arrow::TimeUnit::MICRO)}, .key_bytes = 8},
      {.types = {arrow::float64()}, .key_bytes = 8},
      {.types = {arrow::decimal64(18, 2)}, .key_bytes = 8},
      {.types = {arrow::decimal128(38, 0)}, .key_bytes = 16},
      {.types = {arrow::int16(), arrow::int16()}, .key_bytes = 8},  // 2 + 2 + 1
      {.types = {arrow::int16(), arrow::int32()}, .key_bytes = 8},  // 2 + 4 + 1
      {.types = {arrow::int32(), arrow::int32()}, .key_bytes = 16},
      {.types = {arrow::int64(), arrow::int32()}, .key_bytes = 16},  // 8 + 4 + 1, step 0's
      {.types = {arrow::int64(), arrow::int64()}, .key_bytes = 24},
      {.types = {arrow::int16(), arrow::decimal128(38, 0)}, .key_bytes = 24},
      {.types = {arrow::decimal128(20, 3), arrow::int64()}, .key_bytes = 32},
      {.types = {arrow::decimal128(38, 0), arrow::decimal128(38, 0)}, .key_bytes = 40},
  };
  for (const Case& c : taken) {
    const auto layout = FixedKeyLayoutOf(c.types);
    ASSERT_TRUE(layout.has_value()) << c.types.front().type->ToString();
    EXPECT_EQ(layout->key_bytes, c.key_bytes) << c.types.front().type->ToString();
    ASSERT_EQ(layout->columns.size(), c.types.size());
    for (std::size_t k = 0; k < c.types.size(); ++k) {
      EXPECT_TRUE(layout->columns[k].type->Equals(*c.types[k].type));
    }
    if (c.types.size() == 2) {
      EXPECT_EQ(layout->columns[1].offset, layout->columns[0].width);
      EXPECT_EQ(layout->mask_offset, layout->columns[0].width + layout->columns[1].width);
    }
  }
  const std::vector<std::vector<arrow::TypeHolder>> left = {
      {arrow::binary()},
      {arrow::boolean()},
      {arrow::int64(), arrow::binary()},
      {arrow::int64(), arrow::int64(), arrow::int64()},
      {arrow::int8()},
      {arrow::float32()},
      {},
  };
  for (const auto& types : left) {
    EXPECT_FALSE(FixedKeyLayoutOf(types).has_value())
        << (types.empty() ? "no keys" : types.front().type->ToString());
  }
}

// The engine's tables of one BIGINT, and of a BIGINT and an INTEGER (gtest's macros take a type
// name without commas).
using OneKeyTable = FixedKeyTable<8, Growth::kSlotOrder>;
using OneKeyIdOrderTable = FixedKeyTable<8, Growth::kIdOrder>;
using TwoKeyTable = FixedKeyTable<16, Growth::kSlotOrder>;

// Explicit fixed kinds exercise the candidate. Automatic selection retains the approved
// backends until the candidate meets the end-to-end gates; unsupported fixed shapes fall back.
TEST(FixedKeyLayoutTest, ExplicitFixedKindsPreserveTheAutomaticFallback) {
  arrow::compute::ExecContext ctx;
  const auto fixed =
      MakeKeyTable({arrow::int64(), arrow::int32()}, &ctx, {.kind = KeyTableKind::kFixed});
  ASSERT_TRUE(fixed.ok()) << fixed.status().ToString();
  EXPECT_NE(dynamic_cast<TwoKeyTable*>(fixed->get()), nullptr);
  for (const auto& types : std::vector<std::vector<arrow::TypeHolder>>{
           {arrow::int64()}, {arrow::int64(), arrow::int32()}}) {
    const auto automatic = MakeKeyTable(types, &ctx);
    ASSERT_TRUE(automatic.ok()) << automatic.status().ToString();
    EXPECT_EQ(dynamic_cast<OneKeyTable*>(automatic->get()), nullptr);
    EXPECT_EQ(dynamic_cast<TwoKeyTable*>(automatic->get()), nullptr);
  }
  const auto id_order = MakeKeyTable({arrow::int64()}, &ctx, {.kind = KeyTableKind::kFixedIdOrder});
  ASSERT_TRUE(id_order.ok()) << id_order.status().ToString();
  EXPECT_NE(dynamic_cast<OneKeyIdOrderTable*>(id_order->get()), nullptr);
  for (const KeyTableKind kind : {KeyTableKind::kAuto, KeyTableKind::kGrouper}) {
    const auto strings = MakeKeyTable({arrow::binary()}, &ctx, {.kind = kind});
    ASSERT_TRUE(strings.ok()) << strings.status().ToString();
    EXPECT_EQ(dynamic_cast<OneKeyTable*>(strings->get()), nullptr);
  }
  const auto grouper = MakeKeyTable({arrow::int64()}, &ctx, {.kind = KeyTableKind::kGrouper});
  ASSERT_TRUE(grouper.ok()) << grouper.status().ToString();
  EXPECT_EQ(dynamic_cast<OneKeyTable*>(grouper->get()), nullptr);
  for (const KeyTableKind kind : {KeyTableKind::kFixed, KeyTableKind::kFixedIdOrder}) {
    const auto refused = MakeKeyTable({arrow::binary()}, &ctx, {.kind = kind});
    ASSERT_TRUE(refused.ok()) << refused.status().ToString();
    EXPECT_EQ(dynamic_cast<OneKeyTable*>(refused->get()), nullptr);
  }
}

class ColumnKeyTableTest : public testing::ExecTest {};

TEST_F(ColumnKeyTableTest, LocalLaneChargesItsConcreteIndexBeforeConstruction) {
  const std::vector<arrow::TypeHolder> packed{arrow::int64(), arrow::int32()};
  const std::vector<arrow::TypeHolder> column{arrow::int64(), arrow::int32(), arrow::boolean()};
  constexpr std::int64_t kOtherMetadata = 64;
  for (const auto& types : {packed, column}) {
    std::int64_t required = 0;
    {
      MemoryBudget budget(std::nullopt);
      MemoryReservation lane;
      lane.Reset(&budget);
      ASSERT_TRUE(lane.Resize(kOtherMetadata).ok());
      arrow::compute::ExecContext ctx(&budget);
      const auto table =
          MakeKeyTable(types, &ctx, {.kind = KeyTableKind::kFixed, .table_metadata = &lane});
      ASSERT_TRUE(table.ok()) << table.status();
      required = lane.bytes();
      EXPECT_GT(required, kOtherMetadata);
      if (types.size() == packed.size()) {
        using Table = FixedKeyTable<16, Growth::kSlotOrder>;
        EXPECT_EQ(required, kOtherMetadata + static_cast<std::int64_t>(sizeof(Table)));
        EXPECT_EQ(budget.bytes_allocated(),
                  required);  // Both packed column descriptors are inline.
      } else {
        EXPECT_GT(budget.bytes_allocated(), required);  // Generic column descriptors use the pool.
      }
    }
    // The first budget refuses the concrete index; the second allows its object but refuses
    // the separate column descriptors. Either failure must preserve other lane charges.
    for (const auto limit : {required - 1, required}) {
      MemoryBudget budget(limit);
      arrow::compute::ExecContext ctx(&budget);
      MemoryReservation lane;
      lane.Reset(&budget);
      ASSERT_TRUE(lane.Resize(kOtherMetadata).ok());
      const auto table =
          MakeKeyTable(types, &ctx, {.kind = KeyTableKind::kFixed, .table_metadata = &lane});
      if (types.size() == packed.size() && limit == required) {
        EXPECT_TRUE(table.ok()) << table.status();
      } else {
        EXPECT_TRUE(table.status().IsOutOfMemory()) << table.status();
        EXPECT_EQ(lane.bytes(), kOtherMetadata);
        EXPECT_EQ(budget.bytes_allocated(), kOtherMetadata);
      }
    }
  }
}

// Build one routing lane directly from indexed rows. Ordinary lookup uses the private probe
// hash, so a destination that incorrectly probes with the lane hash cannot pass this test.
TEST_F(ColumnKeyTableTest, SelectedRowsOfOneLaneMatchMaterializedKeys) {
  constexpr std::uint32_t kRows = 16384;
  auto numbers = KeysFrom(0, kRows + 3);
  std::vector<std::optional<std::int32_t>> small;
  std::vector<std::optional<bool>> flags;
  for (std::size_t i = 0; i < numbers.size(); ++i) {
    if (i % 29 == 3) {
      numbers[i] = std::nullopt;
    }
    small.emplace_back(i % 5 == 3 ? std::nullopt : std::optional(static_cast<std::int32_t>(i)));
    flags.emplace_back(i % 7 == 3 ? std::nullopt : std::optional(i % 2 == 0));
  }
  const auto integers = Int64s(numbers);
  const auto narrow = Int32s(small);
  for (const auto& input : {BatchOf({integers}), BatchOf({integers, narrow}),
                            BatchOf({integers, narrow, testing::Bools(flags)})}) {
    const auto batch = input.Slice(3, kRows);
    // Start the validity/BOOLEAN bitmaps at bit three. Include the all-NULL first row's lane.
    const auto hashes = KeyHashes(batch).ValueOrDie();
    std::vector<std::uint32_t> rows;
    for (std::uint32_t i = 0; i < kRows; ++i) {
      const auto row = (137 * i) % kRows;
      if ((hashes[row] & 31U) == (hashes.front() & 31U)) {
        rows.push_back(row);
        if (i % 7 == 0) {
          rows.push_back(row);
        }
      }
    }
    ASSERT_GT(rows.size(), 256U);
    arrow::UInt32Builder indices;
    ASSERT_TRUE(indices.AppendValues(rows).ok());
    const auto selected = indices.Finish().ValueOrDie();
    std::vector<arrow::TypeHolder> types;
    arrow::ArrayVector gathered;
    for (const auto& col : batch.values) {
      types.emplace_back(col.type());
      gathered.push_back(arrow::compute::Take(col, selected).ValueOrDie().make_array());
    }
    const auto materialized = BatchOf(std::move(gathered));
    arrow::compute::ExecContext ctx;
    auto target = MakeKeyTable(types, &ctx, {.kind = KeyTableKind::kFixed}).ValueOrDie();
    auto reference = MakeKeyTable(types, &ctx, {.kind = KeyTableKind::kFixed}).ValueOrDie();
    auto* stored = dynamic_cast<StoredKeyTable*>(target.get());
    ASSERT_NE(stored, nullptr);
    const std::array<std::uint32_t, 2> invalid{0, kRows};
    const auto no_callback = [](std::size_t, std::span<const std::uint32_t>, std::uint32_t) {
      ADD_FAILURE() << "empty or invalid selection reached an update";
      return arrow::Status::OK();
    };
    EXPECT_TRUE(stored->ConsumeSelected(batch, {}, no_callback).ok());
    EXPECT_TRUE(stored->ConsumeSelected(batch, invalid, no_callback).IsInvalid());
    EXPECT_EQ(stored->num_groups(), 0U);

    const auto expected = IdsOf(*reference, materialized);
    std::size_t visited = 0;
    const auto status = stored->ConsumeSelected(
        batch, rows,
        [&](std::size_t offset, std::span<const std::uint32_t> ids, std::uint32_t after) {
          EXPECT_EQ(offset, visited);
          EXPECT_EQ(after, stored->num_groups());
          EXPECT_TRUE(std::ranges::equal(ids, std::span(expected).subspan(offset, ids.size())));
          visited += ids.size();
          return arrow::Status::OK();
        });
    ASSERT_TRUE(status.ok()) << status;
    EXPECT_EQ(visited, expected.size());
    const auto keys = reference->Uniques().ValueOrDie();
    EXPECT_EQ(stored->Uniques().ValueOrDie().batch.values, keys.batch.values);
    std::vector<std::uint64_t> stored_hashes(stored->num_groups());
    stored->RoutingHashes(0, stored_hashes);
    EXPECT_EQ(stored_hashes, KeyHashes(keys.batch).ValueOrDie());
    const auto lookup = stored->Lookup(materialized).ValueOrDie();
    ASSERT_EQ(lookup->null_count(), 0);
    EXPECT_TRUE(std::ranges::equal(std::span(lookup->raw_values(), expected.size()), expected));
  }
}

TEST_F(ColumnKeyTableTest, ChunkUpdatesAndStoredMergesMatchMaterializedKeys) {
  std::vector<std::optional<std::int64_t>> ints;
  std::vector<std::optional<std::int32_t>> small;
  std::vector<std::optional<bool>> flags;
  ints.reserve(941);
  small.reserve(941);
  flags.reserve(941);
  for (int i = 0; i < 941; ++i) {
    const int key = i % 301;
    ints.emplace_back(key == 23 ? std::nullopt : std::optional<std::int64_t>(key));
    small.emplace_back(key % 4 == 0 ? std::nullopt : std::optional<std::int32_t>(key % 17));
    flags.emplace_back(key % 3 == 0 ? std::nullopt : std::optional<bool>(key % 3 == 1));
  }
  const auto numbers = Int64s(ints);
  const auto narrow = Int32s(small);
  const auto bits = testing::Bools(flags);
  for (const auto& unsliced :
       {BatchOf({numbers}), BatchOf({narrow, numbers}), BatchOf({numbers, bits, narrow})}) {
    const auto batch = unsliced.Slice(3, 937);
    std::vector<arrow::TypeHolder> types;
    types.reserve(batch.values.size());
    for (const auto& value : batch.values) {
      types.emplace_back(value.type());
    }
    MemoryBudget budget(std::nullopt);
    arrow::compute::ExecContext ctx(&budget);
    auto baseline = MakeKeyTable(types, &ctx, {.kind = KeyTableKind::kFixed});
    auto source = MakeKeyTable(types, &ctx, {.kind = KeyTableKind::kFixed});
    auto target = MakeKeyTable(types, &ctx, {.kind = KeyTableKind::kFixedIdOrder});
    ASSERT_TRUE(baseline.ok()) << baseline.status();
    ASSERT_TRUE(source.ok()) << source.status();
    ASSERT_TRUE(target.ok()) << target.status();
    auto* stored = dynamic_cast<StoredKeyTable*>(source->get());
    auto* destination = dynamic_cast<StoredKeyTable*>(target->get());
    ASSERT_NE(stored, nullptr);
    ASSERT_NE(destination, nullptr);
    const auto expected = IdsOf(**baseline, batch);
    std::size_t visited = 0;
    const auto status = stored->ConsumeChunks(
        batch, [&](std::size_t offset, std::span<const std::uint32_t> ids, std::uint32_t after) {
          EXPECT_EQ(offset, visited);
          EXPECT_EQ(after, stored->num_groups());
          EXPECT_TRUE(std::ranges::equal(ids, std::span(expected).subspan(offset, ids.size())));
          visited += ids.size();
          return arrow::Status::OK();
        });
    ASSERT_TRUE(status.ok()) << status;
    ASSERT_EQ(visited, expected.size());

    const auto unique = (*baseline)->Uniques();
    ASSERT_TRUE(unique.ok()) << unique.status();
    // Both validity and BOOLEAN values start in the middle of a byte.
    const auto range = stored->StoredUniques(3, 41);
    ASSERT_TRUE(range.ok()) << range.status();
    const auto reference = unique->batch.Slice(3, 41);
    for (std::size_t c = 0; c < types.size(); ++c) {
      EXPECT_TRUE(range->batch.values[c].Equals(reference.values[c]));
    }
    const auto arrow_hashes = KeyHashes(reference);
    ASSERT_TRUE(arrow_hashes.ok()) << arrow_hashes.status();
    std::vector<std::uint64_t> hashes(41);
    stored->RoutingHashes(3, hashes);
    EXPECT_EQ(hashes, *arrow_hashes);

    const std::array<std::uint32_t, 2> invalid{0, stored->num_groups()};
    EXPECT_TRUE(stored->GatherStored(invalid).status().IsInvalid());
    EXPECT_TRUE(stored->StoredUniques(stored->num_groups(), 1).status().IsInvalid());
    const auto refused = destination->MergeStored(
        *stored, invalid, [](std::size_t, std::span<const std::uint32_t>, std::uint32_t) {
          ADD_FAILURE() << "invalid source selection reached a state update";
          return arrow::Status::OK();
        });
    EXPECT_TRUE(refused.IsInvalid()) << refused;
    EXPECT_EQ(destination->num_groups(), 0U);

    const std::array<std::uint32_t, 5> picks{20, 0, 4, 20, 13};
    for (const auto length : {0U, 1U, 5U}) {
      const auto gathered = stored->GatherStored(std::span(picks).first(length));
      ASSERT_TRUE(gathered.ok()) << gathered.status();
      ASSERT_EQ(gathered->batch.length, length);
      for (std::size_t c = 0; c < types.size(); ++c) {
        const auto output = gathered->batch.values[c].make_array();
        const auto original = unique->batch.values[c].make_array();
        for (std::uint32_t i = 0; i < length; ++i) {
          EXPECT_TRUE(output->RangeEquals(i, i + 1, picks[i], *original));
        }
      }
    }

    // The destination has groups from before this part. Merge its selected groups in reverse
    // order with duplicates, across chunk boundaries and through a different growth policy.
    constexpr std::size_t kSeedRows = 15;
    const auto seed = IdsOf(**target, batch.Slice(0, kSeedRows));
    std::map<std::uint32_t, std::uint32_t> remap;
    for (std::size_t i = 0; i < seed.size(); ++i) {
      remap.emplace(expected[i], seed[i]);
    }
    std::vector<std::uint32_t> selection;
    selection.reserve(800);
    for (std::uint32_t i = 0; i < 800; ++i) {
      selection.push_back(stored->num_groups() - 1 - (i % stored->num_groups()));
    }
    visited = 0;
    const auto merged = destination->MergeStored(
        *stored, selection,
        [&](std::size_t offset, std::span<const std::uint32_t> ids, std::uint32_t after) {
          EXPECT_EQ(offset, visited);
          for (std::size_t i = 0; i < ids.size(); ++i) {
            const auto entry =
                remap.try_emplace(selection[offset + i], static_cast<std::uint32_t>(remap.size()));
            EXPECT_EQ(ids[i], entry.first->second);
          }
          visited += ids.size();
          EXPECT_EQ(after, remap.size());
          return arrow::Status::OK();
        });
    ASSERT_TRUE(merged.ok()) << merged;
    EXPECT_EQ(visited, selection.size());
    source->reset();
    const auto found = destination->Lookup(batch);
    ASSERT_TRUE(found.ok()) << found.status();
    ASSERT_EQ((*found)->null_count(), 0);
    for (std::size_t i = 0; i < expected.size(); ++i) {
      EXPECT_EQ((*found)->Value(static_cast<std::int64_t>(i)), remap.at(expected[i]));
    }
  }
}

TEST_F(ColumnKeyTableTest, PrefetchedMergeMatchesMaterializedKeysAcrossChunksAndGrowth) {
  constexpr std::uint32_t kGroups = 521;
  std::vector<std::optional<bool>> flags;
  std::vector<std::optional<std::int16_t>> small;
  std::vector<std::optional<std::int32_t>> numbers;
  std::vector<std::optional<std::string>> wide;
  for (std::uint32_t i = 0; i < kGroups + 3; ++i) {
    flags.emplace_back(i % 3 == 0 ? std::nullopt : std::optional(i % 3 == 1));
    small.emplace_back(i % 7 == 0 ? std::nullopt : std::optional(static_cast<std::int16_t>(i)));
    numbers.emplace_back(i % 11 == 0 ? std::nullopt : std::optional(-static_cast<std::int32_t>(i)));
    wide.emplace_back(i % 5 == 0
                          ? std::nullopt
                          : std::optional(std::to_string(10000U + i) + "123456789012345678"));
  }
  const auto bits = testing::Bools(flags);
  const auto short_keys = testing::Int16s(small);
  // Span two 64-column NULL mask blocks. The trailing keys include a high-word DECIMAL,
  // a bit-packed BOOLEAN and an all-valid BIGINT that makes the original rows distinct.
  arrow::ArrayVector columns(63, bits);
  columns.insert(columns.end(), {testing::Decimals(38, 3, wide), short_keys, bits, Int32s(numbers),
                                 Int64s(KeysFrom(0, kGroups + 3))});
  const auto batch = BatchOf(std::move(columns)).Slice(3, kGroups);
  std::vector<arrow::TypeHolder> types;
  types.reserve(batch.values.size());
  for (const auto& value : batch.values) {
    types.emplace_back(value.type());
  }
  arrow::compute::ExecContext ctx;
  auto source = MakeKeyTable(types, &ctx, {.kind = KeyTableKind::kFixed}).ValueOrDie();
  auto* stored = dynamic_cast<StoredKeyTable*>(source.get());
  ASSERT_NE(stored, nullptr);
  ASSERT_TRUE(stored->Consume(batch).ok());
  ASSERT_EQ(stored->num_groups(), kGroups);

  constexpr auto kSelections = (2 * kGroups) + 17;
  std::vector<std::uint32_t> selected;
  selected.reserve(kSelections);
  for (std::uint32_t i = 0; i < kSelections; ++i) {
    selected.push_back((137 * i) % kGroups);
  }
  const auto gathered = stored->GatherStored(selected).ValueOrDie();
  const auto seed = batch.Slice(11, 23);
  auto reference = MakeKeyTable(types, &ctx, {.kind = KeyTableKind::kFixed}).ValueOrDie();
  const auto seed_ids = IdsOf(*reference, seed);
  const auto expected_ids = IdsOf(*reference, gathered.batch);
  const auto expected_keys = reference->Uniques().ValueOrDie();
  bool observed = false;
  std::vector<std::unique_ptr<KeyTable>> targets;
  for (const bool prefetch : {false, true}) {
    SCOPED_TRACE(prefetch);
    observed = !prefetch;
    auto target = MakeKeyTable(types, &ctx,
                               {.kind = KeyTableKind::kFixed,
                                .prefetch_composite_merge = prefetch,
                                .prefetched_composite_merge = &observed})
                      .ValueOrDie();
    auto* destination = dynamic_cast<StoredKeyTable*>(target.get());
    ASSERT_NE(destination, nullptr);
    EXPECT_EQ(IdsOf(*destination, seed), seed_ids);
    std::vector<std::uint32_t> actual;
    auto status = destination->MergeStored(
        *stored, selected,
        [&](std::size_t offset, std::span<const std::uint32_t> ids, std::uint32_t after) {
          EXPECT_EQ(offset, actual.size());
          EXPECT_EQ(after, destination->num_groups());
          actual.insert(actual.end(), ids.begin(), ids.end());
          return arrow::Status::OK();
        });
    ASSERT_TRUE(status.ok()) << status;
    EXPECT_EQ(observed, prefetch);
    EXPECT_EQ(actual, expected_ids);
    targets.push_back(std::move(target));
  }
  EXPECT_EQ(stored->Uniques().ValueOrDie().batch.values, batch.values);
  source.reset();
  for (const auto& target : targets) {
    EXPECT_EQ(target->Uniques().ValueOrDie().batch.values, expected_keys.batch.values);
  }
}

TEST_F(ColumnKeyTableTest, PrefetchedMergeScratchIsBoundedAndCanRetryAfterRefusal) {
  const auto batch = BatchOf({Int64s({1}), Int32s({2}), testing::Bools({std::nullopt})});
  const std::vector<arrow::TypeHolder> types{arrow::int64(), arrow::int32(), arrow::boolean()};
  arrow::compute::ExecContext source_ctx;
  auto source = MakeKeyTable(types, &source_ctx, {.kind = KeyTableKind::kFixed}).ValueOrDie();
  auto* stored = dynamic_cast<StoredKeyTable*>(source.get());
  ASSERT_NE(stored, nullptr);
  ASSERT_TRUE(stored->Consume(batch).ok());
  const std::vector<std::uint32_t> selected(65537, 0);
  constexpr std::int64_t kBudget = 65536;
  MemoryBudget budget(kBudget);
  {
    arrow::compute::ExecContext ctx(&budget);
    auto target =
        MakeKeyTable(types, &ctx, {.kind = KeyTableKind::kFixed, .prefetch_composite_merge = true})
            .ValueOrDie();
    auto* destination = dynamic_cast<StoredKeyTable*>(target.get());
    ASSERT_NE(destination, nullptr);
    // Every selected key is already present, so a merge never needs to grow the table.
    ASSERT_TRUE(destination->Consume(batch).ok());
    std::size_t visited = 0;
    const auto merge = [&] {
      return destination->MergeStored(
          *stored, selected,
          [&](std::size_t offset, std::span<const std::uint32_t> ids, std::uint32_t after) {
            EXPECT_EQ(offset, visited);
            EXPECT_EQ(after, 1U);
            EXPECT_TRUE(std::ranges::all_of(ids, [](std::uint32_t id) { return id == 0; }));
            visited += ids.size();
            return arrow::Status::OK();
          });
    };
    {
      MemoryReservation held;
      held.Reset(&budget);
      // The source descriptors and 256 output IDs fit. The hash and key scratch does not.
      ASSERT_TRUE(held.Resize(kBudget - budget.bytes_allocated() - 2048).ok());
      const auto before = budget.bytes_allocated();
      EXPECT_TRUE(merge().IsOutOfMemory());
      EXPECT_GT(budget.bytes_allocated(), before);  // the reusable output IDs were allocated
      EXPECT_EQ(destination->num_groups(), 1U);
      EXPECT_EQ(visited, 0U);
    }
    // A full array of selected IDs alone would exceed this budget four times over.
    const auto merged = merge();
    ASSERT_TRUE(merged.ok()) << merged;
    EXPECT_EQ(visited, selected.size());
  }
  EXPECT_EQ(budget.bytes_allocated(), 0);
}

TEST_F(ColumnKeyTableTest, BorrowedChunkIdsFitWhenTheFullIdArrayExceedsTheBudget) {
  constexpr std::size_t kRows = 65536;
  const auto numbers = Int64s(std::vector<std::optional<std::int64_t>>(kRows, 1));
  for (const auto& batch : {BatchOf({numbers}), BatchOf({numbers, numbers, numbers})}) {
    const std::vector<arrow::TypeHolder> types(batch.values.size(), arrow::int64());
    MemoryBudget budget(65536);
    arrow::compute::ExecContext ctx(&budget);
    auto table = MakeKeyTable(types, &ctx, {.kind = KeyTableKind::kFixed});
    ASSERT_TRUE(table.ok()) << table.status();
    auto* stored = dynamic_cast<StoredKeyTable*>(table->get());
    ASSERT_NE(stored, nullptr);
    ASSERT_TRUE(stored->Consume(batch).status().IsOutOfMemory());
    std::size_t visited = 0;
    const auto status = stored->ConsumeChunks(
        batch, [&](std::size_t offset, std::span<const std::uint32_t> ids, std::uint32_t after) {
          EXPECT_EQ(offset, visited);
          EXPECT_EQ(after, 1U);
          EXPECT_TRUE(std::ranges::all_of(ids, [](std::uint32_t id) { return id == 0; }));
          visited += ids.size();
          return arrow::Status::OK();
        });
    ASSERT_TRUE(status.ok()) << status;
    EXPECT_EQ(visited, kRows);
    const auto stopped = stored->ConsumeChunks(
        batch, [](std::size_t offset, std::span<const std::uint32_t>, std::uint32_t) {
          EXPECT_EQ(offset, 0U);
          return arrow::Status::Cancelled("stop at the first chunk");
        });
    EXPECT_TRUE(stopped.IsCancelled()) << stopped;
  }
}

TEST_F(ColumnKeyTableTest, SchemaAndRetainedEmptyUniquesUseTheQueryBudget) {
  const std::vector<arrow::TypeHolder> types(66, arrow::boolean());
  MemoryBudget tiny(1);
  arrow::compute::ExecContext tiny_ctx(&tiny);
  const auto refused = MakeKeyTable(types, &tiny_ctx, {.kind = KeyTableKind::kFixed});
  ASSERT_TRUE(refused.status().IsOutOfMemory()) << refused.status();
  EXPECT_TRUE(refused.status().message().contains("--memory-limit"));
  EXPECT_EQ(tiny.bytes_allocated(), 0);

  // Both the packed and column backends return charged metadata through a wrapped pool.
  for (const auto& shape : {std::vector<arrow::TypeHolder>{arrow::int64()}, types}) {
    arrow::ProxyMemoryPool backend(arrow::default_memory_pool());
    MemoryBudget budget(std::nullopt, &backend);
    arrow::ProxyMemoryPool query_pool(&budget);
    arrow::compute::ExecContext ctx(&query_pool);
    std::shared_ptr<arrow::ArrayData> retained;
    {
      auto table = MakeKeyTable(shape, &ctx, {.kind = KeyTableKind::kFixed}, &budget);
      ASSERT_TRUE(table.ok()) << table.status();
      const auto schema_bytes = budget.bytes_allocated();
      auto empty = (*table)->Uniques();
      ASSERT_TRUE(empty.ok()) << empty.status();
      ASSERT_EQ(empty->batch.values.size(), shape.size());
      EXPECT_EQ(empty->batch.length, 0);
      EXPECT_GT(budget.bytes_allocated(), schema_bytes);
      retained = empty->batch.values.back().array();
      empty->batch.values.clear();
      const auto datum_bytes = empty->batch.values.capacity() * sizeof(arrow::Datum);
      const auto handle_bytes =
          retained->buffers.capacity() * sizeof(std::shared_ptr<arrow::Buffer>);
      EXPECT_EQ(budget.bytes_allocated() - backend.bytes_allocated(),
                static_cast<std::int64_t>(datum_bytes + handle_bytes));
    }
    const auto handles = retained->buffers.capacity() * sizeof(std::shared_ptr<arrow::Buffer>);
    EXPECT_EQ(budget.bytes_allocated() - backend.bytes_allocated(),
              static_cast<std::int64_t>(handles));
    EXPECT_EQ(retained->length, 0);
    retained.reset();
    EXPECT_EQ(budget.bytes_allocated(), 0);
  }
}

TEST_F(ColumnKeyTableTest, WideEmptySnapshotsRefuseOneByteBelowTheirMetadataAndAllowRetry) {
  const std::vector<arrow::TypeHolder> types(66, arrow::boolean());
  std::int64_t limit = 0;
  {
    // Count the Arrow allocations separately from the heap storage of the two vector types.
    arrow::ProxyMemoryPool backend(arrow::default_memory_pool());
    MemoryBudget budget(std::nullopt, &backend);
    arrow::compute::ExecContext ctx(&budget);
    auto table = MakeKeyTable(types, &ctx, {.kind = KeyTableKind::kFixed});
    ASSERT_TRUE(table.ok()) << table.status();
    const auto before = backend.bytes_allocated();
    const auto snapshot = (*table)->Uniques();
    ASSERT_TRUE(snapshot.ok()) << snapshot.status();
    const auto pooled = backend.bytes_allocated() - before;
    std::size_t vectors = snapshot->batch.values.capacity() * sizeof(arrow::Datum);
    for (const auto& value : snapshot->batch.values) {
      vectors += value.array()->buffers.capacity() * sizeof(std::shared_ptr<arrow::Buffer>);
    }
    // After one live snapshot there is room for the second one's pooled allocations and all
    // but one byte of its vectors. Omitting either kind of vector would let it succeed.
    limit = budget.bytes_allocated() + pooled + static_cast<std::int64_t>(vectors) - 1;
  }

  MemoryBudget tight(limit);
  arrow::ProxyMemoryPool proxy(&tight);
  arrow::compute::ExecContext ctx(&proxy);
  {
    auto table = MakeKeyTable(types, &ctx, {.kind = KeyTableKind::kFixed}, &tight);
    ASSERT_TRUE(table.ok()) << table.status();
    const auto schema = tight.bytes_allocated();
    {
      const auto first = (*table)->Uniques();
      ASSERT_TRUE(first.ok()) << first.status();
      const auto before = tight.bytes_allocated();
      const auto second = (*table)->Uniques();
      EXPECT_TRUE(second.status().IsOutOfMemory()) << second.status();
      EXPECT_EQ(tight.bytes_allocated(), before);
      EXPECT_EQ(first->batch.values.size(), types.size());
    }
    EXPECT_EQ(tight.bytes_allocated(), schema);
    ASSERT_TRUE((*table)->Uniques().ok());
  }
  EXPECT_EQ(tight.bytes_allocated(), 0);
}

TEST_F(ColumnKeyTableTest, EligibleSchemasAndFactorySelection) {
  const std::vector<arrow::TypeHolder> numbers = {
      arrow::int16(),   arrow::uint16(),         arrow::int32(),
      arrow::int64(),   arrow::date32(),         arrow::timestamp(arrow::TimeUnit::MICRO),
      arrow::float64(), arrow::decimal64(16, 4), arrow::decimal128(38, 3),
      arrow::boolean()};
  EXPECT_TRUE(TakesColumnKeys(numbers));
  EXPECT_TRUE(TakesColumnKeys({arrow::boolean()}));
  EXPECT_TRUE(TakesColumnKeys({arrow::int64(), arrow::boolean()}));
  EXPECT_FALSE(TakesColumnKeys({}));
  for (const auto& unsupported :
       {arrow::binary(), arrow::utf8(), arrow::int8(), arrow::float32()}) {
    auto mixed = numbers;
    mixed.emplace_back(unsupported);
    EXPECT_FALSE(TakesColumnKeys(mixed)) << unsupported->ToString();
  }

  // Only our tables enforce the private group cap. Probe it through the normal factory
  // for each new shape and for a schema which must still fall back to Arrow's Grouper.
  const auto bools = testing::Bools({false, true});
  const auto ints = Int64s({0, 0});
  const auto strings = testing::Strings({"a", "b"});
  arrow::compute::ExecContext ctx;
  for (const auto& batch :
       {BatchOf({bools}), BatchOf({ints, bools}), BatchOf({ints, ints, bools}),
        BatchOf({ints, ints, Int64s({0, 1})}), BatchOf({ints, bools, strings})}) {
    std::vector<arrow::TypeHolder> types;
    types.reserve(batch.values.size());
    for (const auto& column : batch.values) {
      types.emplace_back(column.type());
    }
    for (const auto kind :
         {KeyTableKind::kFixed, KeyTableKind::kFixedIdOrder, KeyTableKind::kAuto}) {
      auto table = MakeKeyTable(types, &ctx, {.kind = kind, .max_groups = 1});
      ASSERT_TRUE(table.ok()) << table.status();
      EXPECT_EQ(IdsOf(**table, batch.Slice(0, 1)), (std::vector<std::uint32_t>{0}));
      const auto result = (*table)->Consume(batch);
      if (kind != KeyTableKind::kAuto && TakesColumnKeys(types)) {
        EXPECT_TRUE(result.status().IsOutOfMemory()) << result.status();
        EXPECT_EQ((*table)->num_groups(), 1U);
        EXPECT_EQ(IdsOf(**table, batch.Slice(0, 1)), (std::vector<std::uint32_t>{0}));
      } else {
        EXPECT_TRUE(result.ok()) << result.status();
        EXPECT_EQ((*table)->num_groups(), 2U);
      }
    }
  }
}

// Mix xors its input word with the previous hash before multiplying. Adjust the second word
// so different first words yield equal two-column hashes. The third field and NULL masks match.
TEST_F(ColumnKeyTableTest, HashCollisionsCompareEveryKey) {
  constexpr std::uint64_t kFirst = 1;
  constexpr std::uint64_t kOther = 2;
  constexpr std::uint64_t kSecond = 7;
  const auto left_hash = FoldHash::Mix(kFirst, FoldHash::kSeed);
  const auto right_hash = FoldHash::Mix(kOther, FoldHash::kSeed);
  const auto colliding_word = kSecond ^ left_hash ^ right_hash;
  ASSERT_EQ(FoldHash::Mix(kSecond, left_hash), FoldHash::Mix(colliding_word, right_hash));
  const auto colliding = std::bit_cast<std::int64_t>(colliding_word);
  const auto keys =
      BatchOf({Int32s({kFirst, kOther, kOther, kFirst}),
               Int64s({kSecond, colliding, colliding, kSecond}), Int32s({1, 1, 1, 1})});
  arrow::compute::ExecContext ctx;
  auto table = MakeKeyTable({arrow::int32(), arrow::int64(), arrow::int32()}, &ctx,
                            {.kind = KeyTableKind::kFixed});
  ASSERT_TRUE(table.ok()) << table.status();
  const std::vector<std::uint32_t> expected{0, 1, 1, 0};
  EXPECT_EQ(IdsOf(**table, keys), expected);
  const auto zeros = Int32s(std::vector<std::optional<std::int32_t>>(300, 0));
  const auto more = BatchOf({zeros, Int64s(KeysFrom(0, 300)), zeros});
  ASSERT_TRUE((*table)->Consume(more).ok());
  const auto found = (*table)->Lookup(keys);
  ASSERT_TRUE(found.ok()) << found.status();
  EXPECT_EQ((*found)->null_count(), 0);
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ((*found)->Value(static_cast<std::int64_t>(i)), expected[i]);
  }
}

TEST_F(ColumnKeyTableTest, GrowthChargesTheOverlapAndRollsBackAtTheLimit) {
  const auto keys = Int64s(KeysFrom(0, 25));
  const auto all = BatchOf({keys, keys, keys});
  // The first index holds 24 groups. Measure the cost of returning one ID with no growth,
  // then restrict growth to exactly the final retained bytes plus that per-call workspace.
  const auto first = all.Slice(0, 24);
  const auto known = all.Slice(0, 1);
  const auto next = all.Slice(24, 1);
  const std::vector<arrow::TypeHolder> types(3, arrow::int64());
  MemoryBudget open(std::nullopt);
  arrow::compute::ExecContext ctx(&open);
  auto plain = MakeKeyTable(types, &ctx, {.kind = KeyTableKind::kFixed});
  ASSERT_TRUE(plain.ok()) << plain.status();
  ASSERT_TRUE((*plain)->Consume(first).ok());
  const auto before = open.bytes_allocated();
  open.ResetPeak();
  ASSERT_TRUE((*plain)->Consume(known).ok());
  const auto workspace = open.max_memory() - before;
  ASSERT_TRUE((*plain)->Consume(next).ok());
  const auto final_bytes = open.bytes_allocated();
  ASSERT_GT(final_bytes, before);
  const auto limit = final_bytes + workspace;
  EXPECT_GT(open.max_memory(), limit);

  MemoryBudget tight(limit);
  arrow::compute::ExecContext tight_ctx(&tight);
  auto limited = MakeKeyTable(types, &tight_ctx, {.kind = KeyTableKind::kFixed});
  ASSERT_TRUE(limited.ok()) << limited.status();
  ASSERT_TRUE((*limited)->Consume(first).ok());
  const auto refused = (*limited)->Consume(next);
  EXPECT_TRUE(refused.status().IsOutOfMemory()) << refused.status();
  EXPECT_EQ(tight.bytes_allocated(), before);
  EXPECT_EQ((*limited)->num_groups(), 24U);
  EXPECT_EQ(IdsOf(**limited, known), (std::vector<std::uint32_t>{0}));
}

template <class T>
class FixedKeyTableTest : public testing::ExecTest {
 protected:
  static constexpr Growth kGrowth = T::value;

  // A one-BIGINT-key table hashing with `Hasher`, on the fixture's budget.
  template <class Hasher = FoldHash>
  std::unique_ptr<FixedKeyTable<8, kGrowth, Hasher>> MakeTable(KeyTableOptions options = {}) {
    return std::make_unique<FixedKeyTable<8, kGrowth, Hasher>>(LayoutOf({arrow::int64()}), &budget_,
                                                               options);
  }

  MemoryBudget budget_{std::nullopt};
};

struct GrowthName {
  template <class T>
  static const char* GetName(int /*index*/) {
    return T::value == Growth::kSlotOrder ? "SlotOrder" : "IdOrder";
  }
};

using Growths = ::testing::Types<std::integral_constant<Growth, Growth::kSlotOrder>,
                                 std::integral_constant<Growth, Growth::kIdOrder>>;
TYPED_TEST_SUITE(FixedKeyTableTest, Growths, GrowthName);

// Seven keys whose home is the last of 16 slots, then each again, in one call of 14 rows (the
// most 16 slots hold at 7/8): every probe's group load runs past the end into the mirrored control
// bytes, the six that overflow land in slots 0 to 5 through them, and each repeat is found there.
// With one tag for all, every full slot matches and the key compare decides. Then keys at homes 9
// to 14 and a NULL, as the slots double twice, and one hash for every key. The ids are the
// first-seen ones throughout.
TYPED_TEST(FixedKeyTableTest, ProbesThatCollideAndWrapThroughTheMirror) {
  std::vector<std::optional<std::int64_t>> last;
  last.reserve(14);
  for (std::int64_t k = 0; k < 7; ++k) {
    last.emplace_back(15 + (16 * k));
  }
  for (std::int64_t k = 6; k >= 0; --k) {
    last.emplace_back(15 + (16 * k));
  }
  FirstSeen expected;
  {
    auto table = this->template MakeTable<ValueHash>();
    EXPECT_EQ(IdsOf(*table, BatchOf({Int64s(last)})), expected.IdsOf(last));
    EXPECT_EQ(table->capacity(), 16U);
    std::vector<std::optional<std::int64_t>> wrapping = {std::nullopt};
    for (std::int64_t home = 9; home <= 14; ++home) {
      wrapping.emplace_back(home);
      wrapping.emplace_back(home + 32);
    }
    wrapping.insert(wrapping.end(), last.begin(), last.end());
    EXPECT_EQ(IdsOf(*table, BatchOf({Int64s(wrapping)})), expected.IdsOf(wrapping));
    const std::vector<std::optional<std::int64_t>> more = KeysFrom(1000, 1100);
    EXPECT_EQ(IdsOf(*table, BatchOf({Int64s(more)})), expected.IdsOf(more));
    EXPECT_GT(table->capacity(), 64U);
    EXPECT_EQ(IdsOf(*table, BatchOf({Int64s(wrapping)})), expected.IdsOf(wrapping));
    EXPECT_EQ(table->num_groups(), expected.groups());
  }
  {
    auto table = this->template MakeTable<ConstantHash>();
    FirstSeen same;
    const std::vector<std::optional<std::int64_t>> keys = KeysFrom(0, 300);
    EXPECT_EQ(IdsOf(*table, BatchOf({Int64s(keys)})), same.IdsOf(keys));
    const std::vector<std::optional<std::int64_t>> mixed = KeysFrom(250, 400);
    EXPECT_EQ(IdsOf(*table, BatchOf({Int64s(mixed)})), same.IdsOf(mixed));
    EXPECT_EQ(table->num_groups(), same.groups());
  }
  EXPECT_EQ(this->budget_.bytes_allocated(), 0);
}

TYPED_TEST(FixedKeyTableTest, NullPayloadsAreNotHashed) {
  auto table = this->template MakeTable<NullPayloadHash>();
  const auto input = Int64s({1, std::nullopt, 2, std::nullopt});
  auto* values = input->data()->GetMutableValues<std::int64_t>(1);
  values[1] = kNullPayload;
  values[3] = kNullPayload;
  const auto batch = BatchOf({input});
  EXPECT_EQ(IdsOf(*table, batch), (std::vector<std::uint32_t>{0, 1, 2, 1}));
  const auto lookup = table->Lookup(batch);
  ASSERT_TRUE(lookup.ok()) << lookup.status().ToString();
  EXPECT_EQ((*lookup)->null_count(), 0);
  EXPECT_EQ((*lookup)->Value(1), 1U);
  EXPECT_EQ((*lookup)->Value(3), 1U);
}

// A call's new groups take the ids before, before + 1, ... in the order of their first rows, the
// NULL group (one key) too; with two keys, in the order of the first row of each pair.
TYPED_TEST(FixedKeyTableTest, NewIdsFollowTheirFirstRows) {
  auto table = this->MakeTable();
  FirstSeen expected;
  for (const auto& keys : std::vector<std::vector<std::optional<std::int64_t>>>{
           {5, std::nullopt, 5, 7}, {std::nullopt, 8, 7, 9}, {9, 1, std::nullopt, 2, 1}}) {
    EXPECT_EQ(IdsOf(*table, BatchOf({Int64s(keys)})), expected.IdsOf(keys));
  }
  // The NULL group's key of zeros is never in the slots: a 0 that first comes after the slots
  // have doubled is a group of its own.
  const std::vector<std::optional<std::int64_t>> later = KeysFrom(100, 200);
  EXPECT_EQ(IdsOf(*table, BatchOf({Int64s(later)})), expected.IdsOf(later));
  EXPECT_EQ(IdsOf(*table, BatchOf({Int64s({0, std::nullopt})})), expected.IdsOf({0, std::nullopt}));
  auto pairs = std::make_unique<FixedKeyTable<16, TestFixture::kGrowth>>(
      LayoutOf({arrow::int64(), arrow::int32()}), &this->budget_, KeyTableOptions{});
  EXPECT_EQ(IdsOf(*pairs, BatchOf({Int64s({1, 1, std::nullopt, 1, std::nullopt}),
                                   Int32s({2, std::nullopt, 2, 2, std::nullopt})})),
            (std::vector<std::uint32_t>{0, 1, 2, 0, 3}));
  EXPECT_EQ(IdsOf(*pairs, BatchOf({Int64s({std::nullopt, 2, 1}), Int32s({2, 1, 3})})),
            (std::vector<std::uint32_t>{2, 4, 5}));
}

// Allocations follow the groups, not the rows: a call's ids, the chunk's scratch once, and a few
// arrays a doubling, never one a chunk or a row (counted by a pool between the table and its
// budget).
TYPED_TEST(FixedKeyTableTest, AllocationsFollowTheGroupsNotTheRows) {
  constexpr std::size_t kBatches = 16;
  constexpr std::size_t kRows = 65536;
  constexpr std::uint64_t kGroups = 100000;
  arrow::ProxyMemoryPool counted(&this->budget_);
  {
    FixedKeyTable<8, TestFixture::kGrowth> table(LayoutOf({arrow::int64()}), &counted, {});
    for (std::size_t b = 0; b < kBatches; ++b) {
      const auto ids = table.Consume(BatchOf({Int64s(RandomKeys(b * kRows, kRows, kGroups))}));
      ASSERT_TRUE(ids.ok()) << ids.status().ToString();
    }
    EXPECT_GT(table.num_groups(), kGroups * 99 / 100);
    const auto doublings = static_cast<std::int64_t>(std::bit_width(table.capacity()));
    EXPECT_LE(counted.num_allocations(), static_cast<std::int64_t>(kBatches) + 4 + (4 * doublings))
        << "doublings " << doublings;
  }
  EXPECT_EQ(this->budget_.bytes_allocated(), 0);
}

// At its group limit a table still numbers the keys it has, however many rows, and fails only a
// new group, the NULL group or a new pair included, with its own OutOfMemory.
TYPED_TEST(FixedKeyTableTest, TheGroupLimitFailsOnlyANewGroup) {
  const auto too_many = [](const arrow::Status& status) {
    return status.IsOutOfMemory() && status.message().contains("a GROUP BY of more than");
  };
  {
    auto table = this->MakeTable({.max_groups = 3});
    EXPECT_EQ(IdsOf(*table, BatchOf({Int64s({1, 2, 1, 3})})),
              (std::vector<std::uint32_t>{0, 1, 0, 2}));
    std::vector<std::optional<std::int64_t>> known;
    known.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
      known.emplace_back((i % 3) + 1);
    }
    EXPECT_EQ(IdsOf(*table, BatchOf({Int64s(known)})).size(), known.size());
    const auto failed = table->Consume(BatchOf({Int64s({1, 4})}));
    EXPECT_TRUE(too_many(failed.status())) << failed.status().ToString();
    EXPECT_EQ(table->num_groups(), 3U);
  }
  {
    auto table = this->MakeTable({.max_groups = 2});
    EXPECT_EQ(IdsOf(*table, BatchOf({Int64s({1, 2})})), (std::vector<std::uint32_t>{0, 1}));
    const auto failed = table->Consume(BatchOf({Int64s({2, std::nullopt})}));
    EXPECT_TRUE(too_many(failed.status())) << failed.status().ToString();
  }
  {
    FixedKeyTable<16, TestFixture::kGrowth> pairs(LayoutOf({arrow::int64(), arrow::int32()}),
                                                  &this->budget_, {.max_groups = 2});
    EXPECT_EQ(IdsOf(pairs, BatchOf({Int64s({1, 1}), Int32s({1, 2})})),
              (std::vector<std::uint32_t>{0, 1}));
    const auto failed = pairs.Consume(BatchOf({Int64s({2}), Int32s({1})}));
    EXPECT_TRUE(too_many(failed.status())) << failed.status().ToString();
  }
  EXPECT_EQ(this->budget_.bytes_allocated(), 0);
}

// The estimate (E1) and a table without it (E5, as lanes are made): after the second call of
// random keys over 200,000 groups the estimating table holds slots for about all of them, the other
// only for the groups it has seen; neither changes before then, and both give the same ids.
TYPED_TEST(FixedKeyTableTest, TheEstimateSizesTheSlotsAfterTheSecondCall) {
  constexpr std::size_t kRows = 65536;
  constexpr std::uint64_t kGroups = 200000;
  auto estimating = this->MakeTable();
  auto plain = this->MakeTable({.estimate = false});
  const auto first = BatchOf({Int64s(RandomKeys(0, kRows, kGroups))});
  EXPECT_EQ(IdsOf(*estimating, first), IdsOf(*plain, first));
  EXPECT_EQ(estimating->capacity(), plain->capacity());
  const auto second = BatchOf({Int64s(RandomKeys(kRows, kRows, kGroups))});
  EXPECT_EQ(IdsOf(*estimating, second), IdsOf(*plain, second));
  EXPECT_GT(estimating->capacity(), plain->capacity());
  EXPECT_GE(estimating->capacity() * 7 / 8, kGroups * 9 / 10);
  // Without the estimate, the slots the groups need and room for one chunk of new ones.
  EXPECT_LT(plain->capacity() * 7 / 16, plain->num_groups() + 256);
  const auto third = BatchOf({Int64s(RandomKeys(2 * kRows, kRows, kGroups))});
  const std::size_t sized = estimating->capacity();
  EXPECT_EQ(IdsOf(*estimating, third), IdsOf(*plain, third));
  EXPECT_EQ(estimating->capacity(), sized);  // sized once
}

// It fails small: 63 hits on the first call's groups reserve nothing (E2), 64 reserve, but never
// past 8 times the groups seen (E3): 39,488 of an estimate of 70,312.
TYPED_TEST(FixedKeyTableTest, TheEstimateNeedsHitsAndIsCapped) {
  for (const std::int64_t hits : {63, 64}) {
    SCOPED_TRACE(std::to_string(hits) + " hits");
    auto estimating = this->MakeTable();
    auto plain = this->MakeTable({.estimate = false});
    const auto first = BatchOf({Int64s(KeysFrom(0, 1000))});
    IdsOf(*estimating, first);
    IdsOf(*plain, first);
    std::vector<std::optional<std::int64_t>> keys = KeysFrom(0, hits);
    const std::vector<std::optional<std::int64_t>> fresh = KeysFrom(1000, 1000 + 4000 - hits);
    keys.insert(keys.end(), fresh.begin(), fresh.end());
    const auto second = BatchOf({Int64s(keys)});
    EXPECT_EQ(IdsOf(*estimating, second), IdsOf(*plain, second));
    if (hits < 64) {
      EXPECT_EQ(estimating->capacity(), plain->capacity());
    } else {
      // 8 x 4936 groups = 39,488, which 65,536 slots hold at 7/8 and 32,768 do not.
      EXPECT_EQ(estimating->capacity(), 65536U);
    }
  }
}

// A budget that refuses the reservation (E4) does not fail the table: it gives the same ids and
// holds what the table without the estimate holds.
TYPED_TEST(FixedKeyTableTest, ARefusedEstimateLeavesTheTableAsItWas) {
  constexpr std::size_t kRows = 65536;
  constexpr std::uint64_t kGroups = 200000;
  const auto first = BatchOf({Int64s(RandomKeys(0, kRows, kGroups))});
  const auto second = BatchOf({Int64s(RandomKeys(kRows, kRows, kGroups))});
  MemoryBudget open(std::nullopt);
  FixedKeyTable<8, TestFixture::kGrowth> plain(LayoutOf({arrow::int64()}), &open,
                                               {.estimate = false});
  const std::vector<std::uint32_t> first_ids = IdsOf(plain, first);
  const std::vector<std::uint32_t> second_ids = IdsOf(plain, second);
  MemoryBudget tight(open.max_memory());
  {
    FixedKeyTable<8, TestFixture::kGrowth> estimating(LayoutOf({arrow::int64()}), &tight, {});
    EXPECT_EQ(IdsOf(estimating, first), first_ids);
    EXPECT_EQ(IdsOf(estimating, second), second_ids);
    EXPECT_EQ(estimating.capacity(), plain.capacity());
    EXPECT_EQ(tight.bytes_allocated(), open.bytes_allocated());
  }
  EXPECT_EQ(tight.bytes_allocated(), 0);
}

// With 64 repeats, the estimate wants 39,488 keys. The budget can pay for some of the
// replacement arrays. A later batch of known keys still needs the budget for its output IDs.
TYPED_TEST(FixedKeyTableTest, ARefusedEstimateReleasesEverySpeculativeArray) {
  MemoryBudget tight(470'000);
  FixedKeyTable<8, TestFixture::kGrowth> estimating(LayoutOf({arrow::int64()}), &tight, {});
  auto plain = this->MakeTable({.estimate = false});
  const auto first = BatchOf({Int64s(KeysFrom(0, 1000))});
  const auto second = BatchOf({Int64s(KeysFrom(936, 4936))});
  const auto known = BatchOf({Int64s(std::vector<std::optional<std::int64_t>>(50'000, 42))});
  for (const auto* batch : {&first, &second, &known}) {
    EXPECT_EQ(IdsOf(estimating, *batch), IdsOf(*plain, *batch));
    EXPECT_EQ(estimating.capacity(), plain->capacity());
    EXPECT_EQ(tight.bytes_allocated(), this->budget_.bytes_allocated());
  }
}

// After the first 256 keys, the slots have room for another key, but the key array must double.
// A budget that fits the final buffers exactly must refuse the copy while the old keys are live.
TYPED_TEST(FixedKeyTableTest, KeyOnlyGrowthChargesBothArrays) {
  const auto first = BatchOf({Int64s(KeysFrom(0, 256))});
  const auto next = BatchOf({Int64s({256})});
  auto table = this->MakeTable({.estimate = false});
  ASSERT_EQ(IdsOf(*table, first).size(), 256U);
  const auto slots = table->capacity();
  this->budget_.ResetPeak();
  const auto grown = table->Consume(next);
  ASSERT_TRUE(grown.ok()) << grown.status();
  ASSERT_EQ(table->capacity(), slots);
  const auto final_bytes = this->budget_.bytes_allocated();  // includes the output ID
  EXPECT_GE(this->budget_.max_memory(), final_bytes + (256 * 8));

  MemoryBudget tight(final_bytes);
  FixedKeyTable<8, TestFixture::kGrowth> limited(LayoutOf({arrow::int64()}), &tight,
                                                 {.estimate = false});
  ASSERT_EQ(IdsOf(limited, first).size(), 256U);
  const auto before = tight.bytes_allocated();
  const auto refused = limited.Consume(next);
  EXPECT_TRUE(refused.status().IsOutOfMemory()) << refused.status();
  EXPECT_EQ(limited.num_groups(), 256U);
  EXPECT_EQ(tight.bytes_allocated(), before);
}

// A single refused pool operation, including scratch and growth; all others still use the budget.
class RefusingPool final : public arrow::ProxyMemoryPool {
 public:
  explicit RefusingPool(arrow::MemoryPool* backend) : arrow::ProxyMemoryPool(backend) {}
  void RefuseAfter(std::size_t operations) { remaining_ = operations; }
  void Allow() { remaining_.reset(); }

  arrow::Status Allocate(std::int64_t size, std::int64_t alignment, std::uint8_t** out) override {
    if (Refuse(size)) {
      return arrow::Status::OutOfMemory("test pool refusal");
    }
    return arrow::ProxyMemoryPool::Allocate(size, alignment, out);
  }
  arrow::Status Reallocate(std::int64_t old_size, std::int64_t new_size, std::int64_t alignment,
                           std::uint8_t** ptr) override {
    if (Refuse(new_size)) {
      return arrow::Status::OutOfMemory("test pool refusal");
    }
    return arrow::ProxyMemoryPool::Reallocate(old_size, new_size, alignment, ptr);
  }

 private:
  bool Refuse(std::int64_t size) {
    if (size == 0 || !remaining_.has_value()) {
      return false;
    }
    if (*remaining_ == 0) {
      remaining_.reset();
      return true;
    }
    --*remaining_;
    return false;
  }
  std::optional<std::size_t> remaining_;
};

TEST_F(ColumnKeyTableTest, FailedGrowthCanBeRetriedAndReturnedArraysOutliveTheTable) {
  const auto keys = [](int from, int to) {
    const auto ids = KeysFrom(from, to);
    std::vector<std::optional<bool>> flags;
    flags.reserve(ids.size());
    for (int i = from; i < to; ++i) {
      flags.emplace_back(i % 3 == 0 ? std::nullopt : std::optional<bool>(i % 2 == 0));
    }
    return BatchOf({Int64s(ids), testing::Bools(flags), Int64s(ids)});
  };
  MemoryBudget budget(std::nullopt);
  RefusingPool pool(&budget);
  arrow::compute::ExecContext ctx(&pool);
  const auto more = keys(16, 180);
  std::size_t failures = 0;
  bool finished = false;
  for (std::size_t fail = 0; !finished && fail < 128; ++fail) {
    SCOPED_TRACE(fail);
    auto made = MakeKeyTable({arrow::int64(), arrow::boolean(), arrow::int64()}, &ctx,
                             {.kind = KeyTableKind::kFixed, .estimate = false});
    ASSERT_TRUE(made.ok()) << made.status();
    auto table = std::move(*made);
    // The input goes out of scope before the first read of the stored columns.
    const auto old_ids = table->Consume(keys(0, 16));
    ASSERT_TRUE(old_ids.ok()) << old_ids.status();
    const auto old_uniques = table->Uniques();
    ASSERT_TRUE(old_uniques.ok()) << old_uniques.status();
    const auto old_lookup = table->Lookup(keys(15, 17));
    ASSERT_TRUE(old_lookup.ok()) << old_lookup.status();
    pool.RefuseAfter(fail);
    const auto attempted = table->Consume(more);
    pool.Allow();
    finished = attempted.ok();
    if (!finished) {
      ++failures;
      EXPECT_TRUE(attempted.status().IsOutOfMemory()) << attempted.status();
      const auto original = table->Lookup(keys(0, 16));
      ASSERT_TRUE(original.ok()) << original.status();
      ASSERT_EQ((*original)->null_count(), 0);
      for (std::uint32_t i = 0; i < 16; ++i) {
        EXPECT_EQ((*original)->Value(i), i);
      }
    }
    // Some rows may have been inserted before the failed allocation. All keep their original
    // ids when the full batch is submitted again.
    const auto retried = table->Consume(more);
    ASSERT_TRUE(retried.ok()) << retried.status();
    ASSERT_EQ(retried->ids.size(), 164U);
    for (std::size_t i = 0; i < retried->ids.size(); ++i) {
      EXPECT_EQ(retried->ids[i], 16U + i);
    }
    EXPECT_EQ(table->num_groups(), 180U);
    table.reset();
    ASSERT_EQ(old_ids->ids.size(), 16U);
    for (std::size_t i = 0; i < old_ids->ids.size(); ++i) {
      EXPECT_EQ(old_ids->ids[i], i);
    }
    EXPECT_EQ((*old_lookup)->Value(0), 15U);
    EXPECT_TRUE((*old_lookup)->IsNull(1));
    const auto first = keys(0, 16);
    ASSERT_EQ(old_uniques->batch.values.size(), first.values.size());
    for (std::size_t i = 0; i < first.values.size(); ++i) {
      EXPECT_TRUE(old_uniques->batch.values[i].Equals(first.values[i]));
    }
  }
  EXPECT_TRUE(finished);
  EXPECT_GT(failures, 1U);
  EXPECT_EQ(pool.bytes_allocated(), 0);
}

TYPED_TEST(FixedKeyTableTest, LookupNeverInsertsAcrossCollisionsGrowthAndNulls) {
  auto table = this->template MakeTable<ConstantHash>();
  const auto wanted = BatchOf({Int64s({std::nullopt, 0, 1, 400})});
  auto empty = table->Lookup(wanted);
  ASSERT_TRUE(empty.ok()) << empty.status().ToString();
  EXPECT_EQ((*empty)->null_count(), 4);
  EXPECT_EQ(table->num_groups(), 0U);
  EXPECT_EQ(table->capacity(), 0U);
  const auto keys = KeysFrom(0, 400);
  ASSERT_EQ(IdsOf(*table, BatchOf({Int64s(keys)})).size(), 400U);
  const auto capacity = table->capacity();
  const auto found = table->Lookup(wanted);
  ASSERT_TRUE(found.ok()) << found.status().ToString();
  EXPECT_TRUE((*found)->IsNull(0));
  EXPECT_EQ((*found)->Value(1), 0U);
  EXPECT_EQ((*found)->Value(2), 1U);
  EXPECT_TRUE((*found)->IsNull(3));
  EXPECT_EQ(table->num_groups(), 400U);
  EXPECT_EQ(table->capacity(), capacity);
  EXPECT_EQ(IdsOf(*table, BatchOf({Int64s({std::nullopt})})), (std::vector<std::uint32_t>{400}));
  auto null_group = table->Lookup(wanted);
  ASSERT_TRUE(null_group.ok()) << null_group.status().ToString();
  EXPECT_EQ((*null_group)->Value(0), 400U);
  EXPECT_EQ((*null_group)->null_count(), 1);
  // A returned lookup array owns its IDs and bitmap independently of future scratch and the table.
  const auto retained = std::move(*null_group);
  table.reset();
  EXPECT_EQ(retained->Value(0), 400U);
  EXPECT_TRUE(retained->IsNull(3));
  EXPECT_EQ((*found)->null_count(), 2);
}

TYPED_TEST(FixedKeyTableTest, EmptyLookupOnlyNeedsMemoryForItsResult) {
  // Enough for four IDs and their validity bitmap, even with separate 64-byte-aligned buffers.
  MemoryBudget tight(128);
  std::shared_ptr<arrow::UInt32Array> misses;
  {
    FixedKeyTable<8, TestFixture::kGrowth> table(LayoutOf({arrow::int64()}), &tight, {});
    const auto probe = BatchOf({Int64s({std::nullopt, 2, 0, 2})});
    auto result = table.Lookup(probe);
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    misses = std::move(*result);
    EXPECT_EQ(table.num_groups(), 0U);
  }
  EXPECT_GT(tight.bytes_allocated(), 0);
  ASSERT_EQ(misses->length(), 4);
  EXPECT_EQ(misses->null_count(), 4);
  for (std::int64_t row = 0; row < misses->length(); ++row) {
    EXPECT_TRUE(misses->IsNull(row));
  }
  misses.reset();
  EXPECT_EQ(tight.bytes_allocated(), 0);
}

TYPED_TEST(FixedKeyTableTest, FailedScratchAndGrowthAllocationsAllowRetry) {
  const auto batch = BatchOf({Int64s({std::nullopt, 1, 2, 1})});
  std::size_t failures = 0;
  for (std::size_t fail = 0; fail < 12; ++fail) {
    SCOPED_TRACE(fail);
    RefusingPool pool(&this->budget_);
    {
      FixedKeyTable<8, TestFixture::kGrowth> table(LayoutOf({arrow::int64()}), &pool, {});
      pool.RefuseAfter(fail);
      const auto first = table.Consume(batch);
      if (!first.ok()) {
        EXPECT_TRUE(first.status().IsOutOfMemory()) << first.status().ToString();
        EXPECT_EQ(table.num_groups(), 0U);
        ++failures;
      }
      pool.Allow();
      EXPECT_EQ(IdsOf(table, batch), (std::vector<std::uint32_t>{0, 1, 2, 1}));
      const auto probe = BatchOf({Int64s({3, std::nullopt, 2})});
      pool.RefuseAfter(fail);
      const auto lookup = table.Lookup(probe);
      if (!lookup.ok()) {
        EXPECT_TRUE(lookup.status().IsOutOfMemory()) << lookup.status().ToString();
      }
      EXPECT_EQ(table.num_groups(), 3U);
      pool.Allow();
      const auto retried = table.Lookup(probe);
      ASSERT_TRUE(retried.ok()) << retried.status().ToString();
      EXPECT_TRUE((*retried)->IsNull(0));
      EXPECT_EQ((*retried)->Value(1), 0U);
      EXPECT_EQ((*retried)->Value(2), 2U);
    }
    EXPECT_EQ(pool.bytes_allocated(), 0);
  }
  // Include at least the ID, three scratch, control, slots and key storage refusals.
  EXPECT_GE(failures, 7U);
}

TYPED_TEST(FixedKeyTableTest, FailedGrowthKeepsExistingIdsAndOwnedUniques) {
  const auto first = BatchOf({Int64s(KeysFrom(0, 300))});
  const auto more = BatchOf({Int64s(KeysFrom(300, 900))});
  std::size_t failures = 0;
  for (std::size_t fail = 0; fail < 8; ++fail) {
    SCOPED_TRACE(fail);
    RefusingPool pool(&this->budget_);
    {
      FixedKeyTable<8, TestFixture::kGrowth> table(LayoutOf({arrow::int64()}), &pool, {});
      ASSERT_EQ(IdsOf(table, first).size(), 300U);
      const auto early = table.Uniques();
      ASSERT_TRUE(early.ok()) << early.status().ToString();
      pool.RefuseAfter(fail);
      const auto grown = table.Consume(more);
      if (!grown.ok()) {
        EXPECT_TRUE(grown.status().IsOutOfMemory()) << grown.status().ToString();
        ++failures;
      }
      pool.Allow();
      const auto original = table.Lookup(first);
      ASSERT_TRUE(original.ok()) << original.status().ToString();
      EXPECT_EQ((*original)->null_count(), 0);
      for (std::uint32_t id = 0; id < 300; ++id) {
        EXPECT_EQ((*original)->Value(id), id);
      }
      const auto retried = table.Consume(more);
      ASSERT_TRUE(retried.ok()) << retried.status().ToString();
      for (std::size_t row = 0; row < retried->ids.size(); ++row) {
        EXPECT_EQ(retried->ids[row], 300U + row);
      }
      EXPECT_EQ(table.num_groups(), 900U);
      const auto retained =
          std::static_pointer_cast<arrow::Int64Array>(early->batch.values.front().make_array());
      EXPECT_EQ(retained->length(), 300);
      EXPECT_EQ(retained->Value(0), 0);
      EXPECT_EQ(retained->Value(299), 299);
      EXPECT_EQ(IdsOf(table, BatchOf({Int64s({std::nullopt})})), (std::vector<std::uint32_t>{900}));
      // Refused output values or validity allocation cannot mutate the index or leak buffers.
      for (std::size_t output_fail = 0; output_fail < 2; ++output_fail) {
        const auto before = pool.bytes_allocated();
        pool.RefuseAfter(output_fail);
        {
          const auto refused = table.Uniques();
          EXPECT_TRUE(refused.status().IsOutOfMemory()) << refused.status().ToString();
        }
        pool.Allow();
        EXPECT_EQ(pool.bytes_allocated(), before);
        EXPECT_EQ(table.num_groups(), 901U);
        const auto null_id = table.Lookup(BatchOf({Int64s({std::nullopt})}));
        ASSERT_TRUE(null_id.ok()) << null_id.status().ToString();
        EXPECT_EQ((*null_id)->Value(0), 900U);
      }
    }
    EXPECT_EQ(pool.bytes_allocated(), 0);
  }
  EXPECT_GE(failures, 4U);
}

TYPED_TEST(FixedKeyTableTest, EmptyLookupWithOutputFailureCanBeRetriedBeforeConsume) {
  const auto batch = BatchOf({Int64s({std::nullopt, 1, 2})});
  RefusingPool pool(&this->budget_);
  FixedKeyTable<8, TestFixture::kGrowth> table(LayoutOf({arrow::int64()}), &pool, {});
  pool.RefuseAfter(0);
  const auto first = table.Lookup(batch);
  EXPECT_TRUE(first.status().IsOutOfMemory()) << first.status().ToString();
  EXPECT_EQ(table.num_groups(), 0U);
  EXPECT_EQ(pool.bytes_allocated(), 0);
  pool.Allow();
  const auto retried = table.Lookup(batch);
  ASSERT_TRUE(retried.ok()) << retried.status().ToString();
  EXPECT_EQ((*retried)->null_count(), 3);
  EXPECT_EQ(IdsOf(table, batch), (std::vector<std::uint32_t>{0, 1, 2}));
}

}  // namespace
}  // namespace antb1::exec
