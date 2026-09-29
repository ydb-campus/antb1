#include "../heavy_hitters.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>

#include <gtest/gtest.h>

namespace antb1::exec {
namespace {

// A stream with a few frequent values among many rare ones, in a fixed scrambled order.
std::vector<std::uint64_t> Stream() {
  std::vector<std::uint64_t> values;
  for (std::uint64_t i = 0; i < 20000; ++i) {
    std::uint64_t value = 1000 + ((i * 7919) % 5000);  // rare: 4 times each
    if (i % 5 == 0) {
      value = 1;  // 20%
    } else if (i % 50 == 1) {
      value = 2;  // 2%
    } else if (i % 400 == 3) {
      value = 3;  // 0.25%
    }
    values.push_back(value);
  }
  return values;
}

// Every value above the share is found when the summary is large enough for the share; nothing
// found is far below it (the Misra-Gries bounds), for small and larger summaries.
TEST(HeavyHittersTest, FindsEveryValueAboveTheShare) {
  const std::vector<std::uint64_t> values = Stream();
  std::map<std::uint64_t, std::int64_t> counts;
  for (const std::uint64_t value : values) {
    ++counts[value];
  }
  for (const std::size_t capacity : {std::size_t{8}, std::size_t{64}, std::size_t{512}}) {
    HeavyHitters summary(capacity);
    for (const std::uint64_t value : values) {
      summary.Add(value);
    }
    EXPECT_EQ(summary.count(), 20000);
    for (const double share : {0.1, 0.01, 1.0 / 128}) {
      const std::vector<std::uint64_t> heavy = summary.Above(share);
      const auto n = static_cast<double>(values.size());
      for (const auto& [value, count] : counts) {
        const bool found = std::ranges::binary_search(heavy, value);
        if (static_cast<double>(count) > share * n &&
            share * static_cast<double>(capacity + 1) >= 1) {
          EXPECT_TRUE(found) << value << " capacity " << capacity << " share " << share;
        }
        if (found) {
          EXPECT_GT(static_cast<double>(count),
                    (share * n) - (n / static_cast<double>(capacity + 1)))
              << value;
        }
      }
      EXPECT_TRUE(std::ranges::is_sorted(heavy));
    }
  }
  HeavyHitters summary(512);
  for (const std::uint64_t value : values) {
    summary.Add(value);
  }
  EXPECT_EQ(summary.Above(0.1), (std::vector<std::uint64_t>{1}));
  EXPECT_EQ(summary.Above(0.01), (std::vector<std::uint64_t>{1, 2}));
}

// The summary is a function of the values and their order.
TEST(HeavyHittersTest, SameStreamSameSummary) {
  HeavyHitters a(16);
  HeavyHitters b(16);
  for (const std::uint64_t value : Stream()) {
    a.Add(value);
    b.Add(value);
  }
  EXPECT_EQ(a.Above(1.0 / 64), b.Above(1.0 / 64));
  EXPECT_TRUE(HeavyHitters(4).Above(0.5).empty());
}

}  // namespace
}  // namespace antb1::exec
