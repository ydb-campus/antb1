#include "../group_hash_table.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <arrow/api.h>
#include <arrow/compute/api.h>
#include <gtest/gtest.h>

#include "antb1/exec/memory_budget.h"

#include "exec_test_util.h"

// The table of ADR 0019 against arrow::compute::Grouper, which it replaces: the gate is that the
// results are identical, so every case here asks both for the same input and compares. A test that
// only checked the table against hand-written answers would pass while differing from the grouper
// in a way the engine's own GROUP BY would notice.

namespace antb1::exec {
namespace {

using antb1::exec::testing::Int32s;
using antb1::exec::testing::Int64s;

// The group ids and the uniques of `batches` through the grouper and through the table, so a case
// compares the two. The table is given no expected size, so it starts small and grows.
struct Both {
  std::vector<std::vector<std::uint32_t>> grouper_ids;
  std::vector<std::vector<std::uint32_t>> table_ids;
  std::string grouper_uniques;
  std::string table_uniques;
  std::uint32_t grouper_groups = 0;
  std::uint32_t table_groups = 0;
};

std::vector<std::uint32_t> IdsOf(const arrow::Array& ids) {
  const auto& typed = static_cast<const arrow::UInt32Array&>(ids);
  std::vector<std::uint32_t> out;
  out.reserve(static_cast<std::size_t>(typed.length()));
  for (int64_t i = 0; i < typed.length(); ++i) {
    out.push_back(typed.Value(i));
  }
  return out;
}

std::string UniquesOf(const arrow::compute::ExecBatch& uniques) {
  std::string out;
  for (const arrow::Datum& value : uniques.values) {
    out += value.make_array()->ToString();
    out += ";";
  }
  return out;
}

arrow::Result<Both> Run(const std::vector<arrow::ArrayVector>& batches, int64_t expected = 0) {
  Both both;
  std::vector<arrow::TypeHolder> types;
  for (const auto& column : batches.front()) {
    types.emplace_back(column->type());
  }
  ARROW_ASSIGN_OR_RAISE(auto grouper, arrow::compute::Grouper::Make(types));
  ARROW_ASSIGN_OR_RAISE(
      auto table, GroupHashTable::Make(types, arrow::default_memory_pool(), nullptr, expected));
  for (const arrow::ArrayVector& columns : batches) {
    std::vector<arrow::Datum> values(columns.begin(), columns.end());
    const arrow::compute::ExecBatch keys(std::move(values), columns.front()->length());
    ARROW_ASSIGN_OR_RAISE(const arrow::Datum ids, grouper->Consume(arrow::compute::ExecSpan(keys)));
    both.grouper_ids.push_back(IdsOf(*ids.make_array()));
    ARROW_ASSIGN_OR_RAISE(const auto mine, table->Consume(keys));
    both.table_ids.push_back(IdsOf(*mine));
  }
  both.grouper_groups = grouper->num_groups();
  both.table_groups = table->num_groups();
  ARROW_ASSIGN_OR_RAISE(const auto grouper_uniques, grouper->GetUniques());
  ARROW_ASSIGN_OR_RAISE(const auto table_uniques, table->GetUniques());
  both.grouper_uniques = UniquesOf(grouper_uniques);
  both.table_uniques = UniquesOf(table_uniques);
  return both;
}

void ExpectSame(const std::vector<arrow::ArrayVector>& batches, int64_t expected = 0) {
  auto both = Run(batches, expected);
  ASSERT_TRUE(both.ok()) << both.status().ToString();
  EXPECT_EQ(both->table_groups, both->grouper_groups);
  EXPECT_EQ(both->table_ids, both->grouper_ids);
  EXPECT_EQ(both->table_uniques, both->grouper_uniques);
}

// One BIGINT key: repeats join their group, new keys are numbered in first-seen order, and a NULL
// is a group of its own that every other NULL joins.
TEST(GroupHashTableTest, OneKeyWithRepeatsAndNulls) {
  ExpectSame({{Int64s({7, 7, -1, std::nullopt, 7, std::nullopt, 0})}});
}

// Two keys of different widths: a row is its whole key, so rows agreeing in one key and not the
// other are different groups, and a NULL in either is part of the key.
TEST(GroupHashTableTest, TwoKeysOfDifferentWidths) {
  ExpectSame({{Int64s({1, 1, 2, 1, std::nullopt, std::nullopt, 1}),
               Int32s({5, 6, 5, 5, 5, std::nullopt, std::nullopt})}});
}

// Several batches: the ids of the second continue the first's groups, which is how a part's batches
// reach one table.
TEST(GroupHashTableTest, GroupsCarryAcrossBatches) {
  ExpectSame({{Int64s({1, 2, 3})}, {Int64s({3, 4, 1})}, {Int64s({5, 5, 2})}});
}

// Past the load the table grows, which moves every slot: the groups it already holds have to come
// back in their own order, so the ids and the uniques stay the grouper's. 4000 distinct keys from a
// table that starts at 64 slots is six doublings.
TEST(GroupHashTableTest, GrowingKeepsEveryGroup) {
  std::vector<std::optional<int64_t>> keys;
  keys.reserve(8000);
  for (int64_t i = 0; i < 4000; ++i) {
    keys.emplace_back(i * 7);  // distinct, and not consecutive
  }
  for (int64_t i = 0; i < 4000; ++i) {
    keys.emplace_back((3999 - i) * 7);  // every one again, in reverse: all hits
  }
  ExpectSame({{Int64s(keys)}});
}

// Told how many groups to expect, the table does not grow at all; the answer is the same either
// way.
TEST(GroupHashTableTest, SizedUpFrontAnswersTheSame) {
  std::vector<std::optional<int64_t>> keys;
  keys.reserve(2000);
  for (int64_t i = 0; i < 2000; ++i) {
    keys.emplace_back(i % 500);
  }
  ExpectSame({{Int64s(keys)}}, /*expected=*/500);
}

// DOUBLE keys arrive normalized (NormalizeDoubleKey, as the grouper's caller does), so the table
// only has to hold them: -0.0 and 0.0 are one group by then, and so are two NaNs.
TEST(GroupHashTableTest, NormalizedDoubleKeys) {
  const auto doubles = testing::ArrayOf<arrow::DoubleBuilder>(
      arrow::float64(), std::vector<std::optional<double>>{0.0, 0.0, 1.5, std::nullopt, 1.5});
  ExpectSame({{doubles}});
}

// A key type of a fixed width the engine uses beside the integers: DATE is four bytes, TIMESTAMP
// eight, DECIMAL(38,0) sixteen, so the packing holds each at its own width.
TEST(GroupHashTableTest, OtherFixedWidthKeys) {
  const auto dates = testing::ArrayOf<arrow::Date32Builder>(
      arrow::date32(), std::vector<std::optional<int32_t>>{19000, 19000, 1, std::nullopt});
  ExpectSame({{dates}});
  arrow::Decimal128Builder decimals(arrow::decimal128(38, 0));
  for (const char* value : {"1", "1", "-170141183460469231731687303715884105728"}) {
    auto parsed = arrow::Decimal128::FromString(value);
    ASSERT_TRUE(parsed.ok());
    ASSERT_TRUE(decimals.Append(*parsed).ok());
  }
  ASSERT_TRUE(decimals.AppendNull().ok());
  auto built = decimals.Finish();
  ASSERT_TRUE(built.ok());
  ExpectSame({{*built}});
}

// What the table does not hold goes back to the grouper rather than failing late: a VARCHAR key
// (until ADR 0027's S2 layout), a BOOLEAN one (a bit, which the packing has no width for), and no
// keys at all (one group, which GroupTable answers itself).
TEST(GroupHashTableTest, SaysWhatItCannotHold) {
  EXPECT_FALSE(GroupHashTable::Supports({arrow::binary()}));
  EXPECT_FALSE(GroupHashTable::Supports({arrow::int64(), arrow::binary()}));
  EXPECT_FALSE(GroupHashTable::Supports({arrow::boolean()}));
  EXPECT_FALSE(GroupHashTable::Supports({}));
  EXPECT_TRUE(GroupHashTable::Supports({arrow::int64()}));
  EXPECT_TRUE(GroupHashTable::Supports({arrow::int64(), arrow::int32()}));
  auto refused = GroupHashTable::Make({arrow::binary()}, arrow::default_memory_pool(), nullptr);
  EXPECT_TRUE(refused.status().IsInvalid()) << refused.status().ToString();
}

// Every byte the table holds is charged to the budget, so a limit stops it rather than the host
// running out: OutOfMemory, and nothing of the table's own allocations escapes the reservation.
TEST(GroupHashTableTest, ChargesItsMemoryToTheBudget) {
  MemoryBudget budget(16 * 1024);
  auto table =
      GroupHashTable::Make({arrow::int64()}, arrow::default_memory_pool(), &budget, /*expected=*/0);
  ASSERT_TRUE(table.ok()) << table.status().ToString();
  EXPECT_GT(budget.bytes_allocated(), 0);
  std::vector<std::optional<int64_t>> keys;
  keys.reserve(100000);
  for (int64_t i = 0; i < 100000; ++i) {
    keys.emplace_back(i);
  }
  const auto column = Int64s(keys);
  std::vector<arrow::Datum> values = {column};
  const arrow::compute::ExecBatch batch(std::move(values), column->length());
  const auto ids = (*table)->Consume(batch);
  ASSERT_FALSE(ids.ok());
  EXPECT_TRUE(ids.status().IsOutOfMemory()) << ids.status().ToString();
  table->reset();
  EXPECT_EQ(budget.bytes_allocated(), 0);
}

}  // namespace
}  // namespace antb1::exec
