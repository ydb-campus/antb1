// PoolVector (pool_vector.h): an array in a buffer of an Arrow memory pool, so that a MemoryBudget
// used as the pool charges it before every allocation.

#include "../pool_vector.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "antb1/exec/memory_budget.h"

namespace antb1::exec {
namespace {

// Arrow rounds a buffer's capacity up to a multiple of 64 bytes, and the budget (the pool) is
// charged exactly that capacity, which grows only through Reserve (or a Resize past it) and never
// shrinks until Reset.
TEST(PoolVectorTest, GrowsChargedAndNeverShrinks) {
  MemoryBudget budget(std::nullopt);
  {
    PoolVector<std::uint32_t> values(&budget);
    EXPECT_EQ(values.capacity(), 0U);
    EXPECT_TRUE(values.empty());
    EXPECT_EQ(budget.bytes_allocated(), 0);

    ASSERT_TRUE(values.Reserve(1).ok());
    EXPECT_EQ(values.capacity(), 16U);  // 64 bytes of 4-byte values
    EXPECT_EQ(values.memory_usage(), 64);
    EXPECT_EQ(budget.bytes_allocated(), 64);
    for (std::uint32_t i = 0; i < 16; ++i) {
      values.push_back(i * 3);
    }
    EXPECT_EQ(values.size(), 16U);

    ASSERT_TRUE(values.Reserve(100).ok());  // a reallocation keeps the values
    EXPECT_GE(values.capacity(), 100U);
    for (std::uint32_t i = 0; i < 16; ++i) {
      EXPECT_EQ(values[i], i * 3);
    }
    const std::int64_t grown = budget.bytes_allocated();
    EXPECT_EQ(grown, values.memory_usage());
    EXPECT_EQ(grown % 64, 0);

    ASSERT_TRUE(values.Resize(2).ok());  // a smaller size keeps the capacity
    EXPECT_EQ(values.size(), 2U);
    ASSERT_TRUE(values.Reserve(10).ok());  // and so does a smaller reservation
    EXPECT_EQ(budget.bytes_allocated(), grown);
    EXPECT_EQ(values[1], 3U);

    values.Reset();
    EXPECT_EQ(values.capacity(), 0U);
    EXPECT_EQ(values.size(), 0U);
    EXPECT_EQ(budget.bytes_allocated(), 0);

    ASSERT_TRUE(values.Resize(5).ok());  // usable again after a Reset
    EXPECT_EQ(values.size(), 5U);
    EXPECT_EQ(budget.bytes_allocated(), 64);
  }
  EXPECT_EQ(budget.bytes_allocated(), 0);  // the destructor gives the buffer back
}

TEST(PoolVectorTest, AssignFillsEveryElement) {
  MemoryBudget budget(std::nullopt);
  PoolVector<std::uint8_t> bytes(&budget);
  ASSERT_TRUE(bytes.Assign(73, 0x80).ok());
  ASSERT_EQ(bytes.size(), 73U);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    EXPECT_EQ(bytes[i], 0x80) << i;
  }
  ASSERT_TRUE(bytes.Assign(5, 1).ok());
  ASSERT_EQ(bytes.size(), 5U);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    EXPECT_EQ(bytes[i], 1) << i;
  }
}

// A refused allocation is the budget's own OutOfMemory and leaves the vector, its values and its
// charge as they were.
TEST(PoolVectorTest, ARefusedAllocationLeavesTheVectorAsItWas) {
  MemoryBudget budget(1024);
  {
    PoolVector<std::uint64_t> values(&budget);
    ASSERT_TRUE(values.Assign(8, 7).ok());  // 64 bytes
    const std::size_t capacity = values.capacity();

    const arrow::Status refused = values.Reserve(std::size_t{1} << 20U);
    EXPECT_TRUE(refused.IsOutOfMemory()) << refused.ToString();
    EXPECT_NE(refused.message().find("memory limit"), std::string::npos) << refused.ToString();
    EXPECT_EQ(values.capacity(), capacity);
    ASSERT_EQ(values.size(), 8U);
    for (std::size_t i = 0; i < values.size(); ++i) {
      EXPECT_EQ(values[i], 7U);
    }
    EXPECT_EQ(budget.bytes_allocated(), 64);

    const arrow::Status resized = values.Resize(std::size_t{1} << 20U);
    EXPECT_TRUE(resized.IsOutOfMemory()) << resized.ToString();
    EXPECT_EQ(values.size(), 8U);  // a refused Resize keeps the size too

    // One element more than an int64 count of bytes can hold fails in the vector itself, before
    // the pool is asked: its own message, not the budget's.
    const std::size_t too_many =
        (static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()) /
         sizeof(std::uint64_t)) +
        1;
    const arrow::Status overflow = values.Reserve(too_many);
    EXPECT_TRUE(overflow.IsOutOfMemory()) << overflow.ToString();
    EXPECT_NE(overflow.message().find("an array of"), std::string::npos) << overflow.ToString();
    EXPECT_EQ(budget.bytes_allocated(), 64);
  }
  EXPECT_EQ(budget.bytes_allocated(), 0);
}

// A moved-from vector is empty, holds no buffer and grows again; the buffer, its values and its
// charge go with the move.
// NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move): the moved-from state is what
// this test checks.
TEST(PoolVectorTest, AMovedFromVectorIsEmpty) {
  MemoryBudget budget(std::nullopt);
  PoolVector<std::uint32_t> from(&budget);
  ASSERT_TRUE(from.Assign(10, 4).ok());
  PoolVector<std::uint32_t> to(std::move(from));
  EXPECT_EQ(to.size(), 10U);
  EXPECT_EQ(to[9], 4U);
  EXPECT_EQ(from.size(), 0U);
  EXPECT_EQ(from.capacity(), 0U);
  EXPECT_EQ(budget.bytes_allocated(), 64);

  PoolVector<std::uint32_t> other(&budget);
  ASSERT_TRUE(other.Assign(20, 5).ok());
  other = std::move(to);  // gives other's buffer back
  EXPECT_EQ(other.size(), 10U);
  EXPECT_EQ(to.size(), 0U);
  EXPECT_EQ(budget.bytes_allocated(), 64);

  ASSERT_TRUE(from.Resize(3).ok());  // a moved-from vector is usable again
  EXPECT_EQ(from.size(), 3U);
  EXPECT_EQ(budget.bytes_allocated(), 128);
}
// NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)

}  // namespace
}  // namespace antb1::exec
