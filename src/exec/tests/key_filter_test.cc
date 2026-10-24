#include "antb1/exec/key_filter.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "antb1/common/int128.h"
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

// Key values, seeded for the same reason, spread over `range` so that they leave gaps between them.
std::vector<Int128> Keys(std::size_t count, std::uint64_t seed, std::uint64_t range) {
  std::mt19937_64 random(seed);
  std::vector<Int128> out(count);
  for (Int128& key : out) {
    key = static_cast<Int128>(random() % range);
  }
  return out;
}

// The one property that cannot bend: a hash that was added is never reported absent. A false
// positive costs a probe row's work; a false negative would drop a row the join must keep, and no
// benchmark would notice.
TEST(KeyFilterTest, NeverMissesAHashItHolds) {
  // The last size is the largest build kMaxBytes admits, sized for its key count but filled with a
  // sample: it is the only case that exercises the widest block mask the cap now allows, which no
  // size below it reaches. Filling it fully would cost a 32 MiB write for no extra property.
  for (const int64_t keys : {int64_t{1}, int64_t{2}, int64_t{7}, int64_t{1024}, int64_t{200000},
                             int64_t{16} * 1024 * 1024}) {
    auto filter = KeyFilter::Make(keys, nullptr);
    ASSERT_TRUE(filter.ok()) << filter.status().ToString();
    ASSERT_NE(*filter, nullptr) << keys;
    const std::vector<std::uint64_t> hashes =
        Hashes(static_cast<std::size_t>(std::min<int64_t>(keys, 200000)), 20261018);
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

// The largest build kMaxBytes admits, and the first it does not. `bytes` rounds up to a
// power-of-two block count, so the cap does not interpolate: it admits a whole size class and
// refuses the next. That quantisation is what the cap's own comment relies on to stay short of the
// filter size ADR 0029 measured at +17%, so it is pinned here rather than left as arithmetic in a
// comment.
TEST(KeyFilterTest, TheCapAdmitsAWholeSizeClassAndRefusesTheNext) {
  struct Case {
    int64_t keys;
    bool made;
    int64_t bytes;  // 0 where it is refused
    std::string_view why;
  };
  for (const Case& c : {
           Case{.keys = int64_t{8} * 1024 * 1024,
                .made = true,
                .bytes = int64_t{16} * 1024 * 1024,
                .why = "the size class below"},
           Case{.keys = int64_t{16} * 1024 * 1024,
                .made = true,
                .bytes = int64_t{32} * 1024 * 1024,
                .why = "the largest build the cap admits"},
           Case{.keys = (int64_t{16} * 1024 * 1024) + 1,
                .made = false,
                .bytes = 0,
                .why = "one key more doubles the filter to 64 MiB"},
           // The build of the query ADR 0029 measured at +17% with a 45 MB filter. It sizes to
           // 64 MiB and must stay refused: that it does is why raising the cap to 32 MiB needed no
           // new gate of its own.
           Case{.keys = 22'760'819, .made = false, .bytes = 0, .why = "the +17% shape"},
       }) {
    auto filter = KeyFilter::Make(c.keys, nullptr);
    ASSERT_TRUE(filter.ok()) << c.why << ": " << filter.status().ToString();
    EXPECT_EQ(*filter != nullptr, c.made) << c.why << ", keys=" << c.keys;
    if (c.made && *filter != nullptr) {
      EXPECT_EQ((*filter)->bytes(), c.bytes) << c.why;
      EXPECT_LE((*filter)->bytes(), KeyFilter::kMaxBytes) << c.why;
    }
    // The same verdict under a budget that could afford the filter (1 GiB, so `limit / 8` is 128
    // MiB and even the refused 64 MiB class fits): what refuses these is the cap and not the
    // budget, and a refusal charges nothing. Worth pinning because the budget case below stopped
    // covering the cap once its key count came under it -- this is the half that is about
    // kMaxBytes. It does not pin the ORDER of the two checks: the reservation is scoped, so
    // reversing them would unwind and leave this assertion green.
    MemoryBudget budget(int64_t{1} * 1024 * 1024 * 1024);
    const int64_t before = budget.bytes_allocated();
    auto charged = KeyFilter::Make(c.keys, &budget);
    ASSERT_TRUE(charged.ok()) << c.why << ": " << charged.status().ToString();
    EXPECT_EQ(*charged != nullptr, c.made) << c.why << " with a budget that affords it";
    if (!c.made) {
      EXPECT_EQ(budget.bytes_allocated(), before)
          << c.why << ": a refused filter charged the budget";
    }
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
  // More than the BUDGET holds, and a key count kMaxBytes itself permits (2 MiB of filter against a
  // 1 MiB budget): no filter, no error, nothing charged. Chosen so that raising or lowering
  // kMaxBytes cannot change what this case exercises -- it declined on the cap until the cap moved,
  // which made it a test of the cap that read as a test of the budget.
  auto too_big = KeyFilter::Make(1'000'000, &budget);
  ASSERT_TRUE(too_big.ok()) << too_big.status().ToString();
  EXPECT_EQ(*too_big, nullptr);
  EXPECT_EQ(budget.bytes_allocated(), before);
}

// The part test's own version of the property above: a range that holds a published key is never
// reported empty. A wrong "no" here skips a part of the probe that had matching rows in it, which
// loses answers silently -- the one failure no benchmark would show.
TEST(KeyFilterTest, NeverRulesOutARangeHoldingAKey) {
  auto filter = KeyFilter::Make(500, nullptr);
  ASSERT_TRUE(filter.ok()) << filter.status().ToString();
  ASSERT_NE(*filter, nullptr);
  std::vector<Int128> keys = Keys(500, 20261019, 1000000);
  (*filter)->PublishKeys(keys, *std::ranges::min_element(keys), *std::ranges::max_element(keys));
  ASSERT_TRUE((*filter)->prunes_parts());
  // Every single key, and every range around one, is held.
  for (const Int128& key : keys) {
    EXPECT_TRUE((*filter)->MayHoldAKeyIn(key, key)) << static_cast<int64_t>(key);
    EXPECT_TRUE((*filter)->MayHoldAKeyIn(key - 10, key + 10)) << static_cast<int64_t>(key);
  }
  // And every range the keys do leave empty is ruled out, which is the point of publishing them.
  std::ranges::sort(keys);
  int64_t ruled_out = 0;
  for (std::size_t i = 1; i < keys.size(); ++i) {
    if (keys[i] - keys[i - 1] < 3) {
      continue;  // no gap between them to test
    }
    EXPECT_FALSE((*filter)->MayHoldAKeyIn(keys[i - 1] + 1, keys[i] - 1))
        << static_cast<int64_t>(keys[i - 1]) << ".." << static_cast<int64_t>(keys[i]);
    ++ruled_out;
  }
  EXPECT_GT(ruled_out, 100) << "a key set this sparse should leave gaps to find";
  // Outside the interval altogether, on either side.
  EXPECT_FALSE((*filter)->MayHoldAKeyIn(keys.front() - 100, keys.front() - 1));
  EXPECT_FALSE((*filter)->MayHoldAKeyIn(keys.back() + 1, keys.back() + 100));
}

// Above kMaxPublishedKeys the values are not collected, so only the interval is published. It rules
// out less -- a gap inside it is invisible -- and must still never rule out a range holding a key.
TEST(KeyFilterTest, FallsBackToTheIntervalWithoutTheValues) {
  auto filter = KeyFilter::Make(100, nullptr);
  ASSERT_TRUE(filter.ok()) << filter.status().ToString();
  ASSERT_NE(*filter, nullptr);
  (*filter)->PublishKeys({}, 100, 200);
  EXPECT_TRUE((*filter)->prunes_parts());
  EXPECT_FALSE((*filter)->MayHoldAKeyIn(0, 99));
  EXPECT_FALSE((*filter)->MayHoldAKeyIn(201, 1000));
  EXPECT_TRUE((*filter)->MayHoldAKeyIn(100, 100));
  EXPECT_TRUE((*filter)->MayHoldAKeyIn(200, 200));
  EXPECT_TRUE((*filter)->MayHoldAKeyIn(0, 100));
  EXPECT_TRUE((*filter)->MayHoldAKeyIn(200, 1000));
  EXPECT_TRUE((*filter)->MayHoldAKeyIn(0, 1000));
  EXPECT_TRUE((*filter)->MayHoldAKeyIn(140, 160)) << "the interval cannot see a gap inside itself";
}

// A filter nothing was published to rules out no part at all, which is how a build whose keys are
// not integers in the engine's domain leaves the probe exactly as it is.
TEST(KeyFilterTest, RulesOutNoPartWithoutPublishedKeys) {
  auto filter = KeyFilter::Make(100, nullptr);
  ASSERT_TRUE(filter.ok()) << filter.status().ToString();
  ASSERT_NE(*filter, nullptr);
  EXPECT_FALSE((*filter)->prunes_parts());
  EXPECT_TRUE((*filter)->MayHoldAKeyIn(0, 0));
  EXPECT_TRUE((*filter)->MayHoldAKeyIn(kInt128Min, kInt128Max));
}

// Both ways of not affording something, because both must give up quietly rather than fail a
// query: a filter is an optimisation, and one that is not made leaves the probe exactly as it was.
TEST(KeyFilterTest, DeclinesTheBitsAndThenTheValuesWhenTheBudgetIsTight) {
  // A thousand keys need 2,048 bytes of bits and 16,000 of values.
  {
    MemoryBudget tiny(1024);  // less than the bits alone: no filter at all, and no error
    auto filter = KeyFilter::Make(1000, &tiny);
    ASSERT_TRUE(filter.ok()) << filter.status().ToString();
    EXPECT_EQ(*filter, nullptr);
    EXPECT_EQ(tiny.bytes_allocated(), 0);
  }
  // Room for the bits but not for the values on top of them: the interval is published alone, which
  // rules out fewer parts and no wrong ones.
  MemoryBudget middling(17000);
  auto filter = KeyFilter::Make(1000, &middling);
  ASSERT_TRUE(filter.ok()) << filter.status().ToString();
  ASSERT_NE(*filter, nullptr);
  const int64_t bits = (*filter)->bytes();
  // The build asks before it collects, so this is the refusal it actually sees, and the values are
  // never held at all rather than charged once they are.
  EXPECT_FALSE((*filter)->ReserveKeys(1000));
  EXPECT_EQ((*filter)->bytes(), bits) << "a refused reservation must charge nothing";
  std::vector<Int128> keys(1000);
  for (std::size_t i = 0; i < keys.size(); ++i) {
    keys[i] = static_cast<Int128>(i) * 2;  // 0, 2, 4, ... so there are gaps to miss
  }
  (*filter)->PublishKeys(keys, 0, 1998);
  EXPECT_EQ((*filter)->bytes(), bits) << "the values were not charged, so they were not kept";
  EXPECT_TRUE((*filter)->prunes_parts());
  EXPECT_FALSE((*filter)->MayHoldAKeyIn(1999, 5000)) << "the interval still rules out a part";
  EXPECT_TRUE((*filter)->MayHoldAKeyIn(1001, 1001))
      << "and without the values it cannot see that this odd number is not a key";
}

// The values are charged on top of the bits, and given back with them.
TEST(KeyFilterTest, ChargesThePublishedValues) {
  MemoryBudget budget(1024 * 1024);
  const int64_t before = budget.bytes_allocated();
  auto filter = KeyFilter::Make(1000, &budget);
  ASSERT_TRUE(filter.ok()) << filter.status().ToString();
  ASSERT_NE(*filter, nullptr);
  const int64_t bits = (*filter)->bytes();
  std::vector<Int128> keys(1000);
  for (std::size_t i = 0; i < keys.size(); ++i) {
    keys[i] = static_cast<Int128>(i);
  }
  // Reserved first, as the build does, and then published: the charge is absolute, so the two
  // together cost what one of them does.
  EXPECT_TRUE((*filter)->ReserveKeys(1000));
  EXPECT_EQ((*filter)->bytes(), bits + (1000 * static_cast<int64_t>(sizeof(Int128))));
  (*filter)->PublishKeys(keys, 0, 999);
  EXPECT_EQ((*filter)->bytes(), bits + (1000 * static_cast<int64_t>(sizeof(Int128))))
      << "reserving and then publishing must not charge twice";
  EXPECT_GE(budget.bytes_allocated(), before + (*filter)->bytes());
  (*filter).reset();
  EXPECT_EQ(budget.bytes_allocated(), before) << "the reservation outlived the filter";
}

}  // namespace
}  // namespace antb1::exec
