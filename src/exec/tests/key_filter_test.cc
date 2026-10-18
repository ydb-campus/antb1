#include "antb1/exec/key_filter.h"

#include <cstdint>
#include <memory>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "antb1/exec/memory_budget.h"

namespace antb1::exec {
namespace {

// Seeded, so a failure is reproducible: the suite is hermetic (AGENTS.md).
std::vector<std::uint64_t> Hashes(std::size_t count, std::uint64_t seed) {
  std::mt19937_64 random(seed);
  std::vector<std::uint64_t> out(count);
  for (std::uint64_t& h : out) {
    h = random();
  }
  return out;
}

// The one property that cannot bend: a hash that was added is never reported absent. A false
// positive costs a probe row's work; a false negative would drop a row the join must keep, and no
// benchmark would notice.
TEST(KeyFilterTest, NeverMissesAHashItHolds) {
  for (const int64_t keys : {int64_t{1}, int64_t{2}, int64_t{7}, int64_t{1024}, int64_t{200000}}) {
    auto filter = KeyFilter::Make(keys, nullptr);
    ASSERT_TRUE(filter.ok()) << filter.status().ToString();
    ASSERT_NE(*filter, nullptr) << keys;
    const std::vector<std::uint64_t> hashes = Hashes(static_cast<std::size_t>(keys), 20261018);
    (*filter)->Add(hashes);
    for (const std::uint64_t h : hashes) {
      EXPECT_TRUE((*filter)->MayContain(h)) << keys << ": " << h;
    }
    // And adding the same hash again changes nothing it answers.
    (*filter)->Add(hashes.front());
    EXPECT_TRUE((*filter)->MayContain(hashes.front()));
  }
}

// Sized at kBitsPerKey, the filter's false-positive rate is a fraction of a percent. The bound is
// loose on purpose -- the point is that the rate is small and that the filter is not saturated,
// not that it hits one number.
TEST(KeyFilterTest, RejectsAlmostEveryHashItDoesNotHold) {
  constexpr int64_t kKeys = 100000;
  auto filter = KeyFilter::Make(kKeys, nullptr);
  ASSERT_TRUE(filter.ok()) << filter.status().ToString();
  ASSERT_NE(*filter, nullptr);
  (*filter)->Add(Hashes(static_cast<std::size_t>(kKeys), 1));
  const std::vector<std::uint64_t> absent = Hashes(1000000, 2);  // a different seed: disjoint
  int64_t positives = 0;
  for (const std::uint64_t h : absent) {
    positives += (*filter)->MayContain(h) ? 1 : 0;
  }
  const double rate = static_cast<double>(positives) / static_cast<double>(absent.size());
  EXPECT_LT(rate, 0.02) << positives << " of " << absent.size();
  EXPECT_GT(rate, 0.0) << "a filter that rejects everything would be a bug, not a win";
}

// An empty filter is the degenerate case the probe must still read correctly.
TEST(KeyFilterTest, HoldsNothingBeforeAnythingIsAdded) {
  auto filter = KeyFilter::Make(64, nullptr);
  ASSERT_TRUE(filter.ok()) << filter.status().ToString();
  ASSERT_NE(*filter, nullptr);
  for (const std::uint64_t h : Hashes(1000, 3)) {
    EXPECT_FALSE((*filter)->MayContain(h));
  }
}

TEST(KeyFilterTest, NoFilterWithoutKeys) {
  for (const int64_t keys : {int64_t{0}, int64_t{-1}}) {
    auto filter = KeyFilter::Make(keys, nullptr);
    ASSERT_TRUE(filter.ok()) << filter.status().ToString();
    EXPECT_EQ(*filter, nullptr) << keys;
  }
}

// The bytes are charged to the budget and given back, and a filter that would not fit is simply
// not made: the probe then runs as it does without one, which is why this is no error.
TEST(KeyFilterTest, ChargesTheBudgetAndDeclinesWhatItCannotAfford) {
  MemoryBudget budget(1024 * 1024);
  const int64_t before = budget.bytes_allocated();
  {
    auto filter = KeyFilter::Make(1000, &budget);
    ASSERT_TRUE(filter.ok()) << filter.status().ToString();
    ASSERT_NE(*filter, nullptr);
    EXPECT_GT((*filter)->bytes(), 0);
    EXPECT_EQ((*filter)->keys(), 1000);
    EXPECT_GE(budget.bytes_allocated(), before + (*filter)->bytes());
  }
  EXPECT_EQ(budget.bytes_allocated(), before) << "the reservation outlived the filter";
  // Far more keys than the limit holds: no filter, no error, and nothing charged.
  auto too_big = KeyFilter::Make(int64_t{16} * 1024 * 1024, &budget);
  ASSERT_TRUE(too_big.ok()) << too_big.status().ToString();
  EXPECT_EQ(*too_big, nullptr);
  EXPECT_EQ(budget.bytes_allocated(), before);
}

}  // namespace
}  // namespace antb1::exec
