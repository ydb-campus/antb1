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
#include <arrow/compute/exec.h>
#include <gtest/gtest.h>

#include "antb1/exec/memory_budget.h"

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

// The one and two-key shapes it takes, at the key widths step 0 measured, and the shapes left to
// arrow's Grouper.
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

// The engine's choice is a function of the key types: our table for the shapes it takes, the
// Grouper for the others; a test that names our table for a shape it does not take is Invalid.
TEST(FixedKeyLayoutTest, TheEngineTakesOurTableForTheShapesItTakes) {
  arrow::compute::ExecContext ctx;
  const auto fixed = MakeKeyTable({arrow::int64(), arrow::int32()}, &ctx);
  ASSERT_TRUE(fixed.ok()) << fixed.status().ToString();
  EXPECT_NE(dynamic_cast<TwoKeyTable*>(fixed->get()), nullptr);
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
    EXPECT_TRUE(refused.status().IsInvalid()) << refused.status().ToString();
  }
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
  static std::string GetName(int /*index*/) {
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
    return status.IsOutOfMemory() &&
           status.message().find("a GROUP BY of more than") != std::string::npos;
  };
  {
    auto table = this->MakeTable({.max_groups = 3});
    EXPECT_EQ(IdsOf(*table, BatchOf({Int64s({1, 2, 1, 3})})),
              (std::vector<std::uint32_t>{0, 1, 0, 2}));
    std::vector<std::optional<std::int64_t>> known;
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

}  // namespace
}  // namespace antb1::exec
