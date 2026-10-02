#include "supported_features.h"

#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <utility>

#include <gtest/gtest.h>

namespace antb1::slt {
namespace {

// A feature enum with more values than a 64-bit mask holds, as Feature will once new grammar adds
// its features: like std::byte it names no value, and every std::uint8_t is one.
enum class WideFeature : std::uint8_t {};
constexpr std::size_t kWideCount = 70;
using WideSet = BasicFeatureSet<WideFeature, kWideCount>;

constexpr WideFeature Wide(int value) { return static_cast<WideFeature>(value); }

// BasicFeatureSet::Names() finds it by argument-dependent lookup.
std::string FeatureName(WideFeature f) { return std::format("w{}", std::to_underlying(f)); }

TEST(FeatureSet, HoldsMoreThan64Features) {
  // Built at compile time, like kSupportedFeatures, on both sides of the 64-bit boundary.
  constexpr WideSet kEdges = {Wide(0), Wide(63), Wide(64), Wide(69)};
  static_assert(kEdges.size() == 4 && kEdges.Has(Wide(64)) && !kEdges.Has(Wide(65)));
  EXPECT_EQ(kEdges.size(), 4U);
  EXPECT_EQ(kEdges.Names(), "w0, w63, w64, w69");

  // A 64-bit mask would wrap feature 65 onto feature 1 (or overflow its shift).
  const WideSet high = {Wide(65)};
  EXPECT_TRUE(high.Has(Wide(65)));
  EXPECT_FALSE(high.Has(Wide(1)));
  EXPECT_NE(high, WideSet{Wide(1)});
  EXPECT_FALSE(kEdges.Contains(high));
  EXPECT_EQ(high.Names(), "w65");

  const WideSet all = WideSet::All();
  EXPECT_EQ(all.size(), kWideCount);
  EXPECT_TRUE(all.Has(Wide(69)));
  EXPECT_TRUE(all.Contains(kEdges));
  EXPECT_FALSE(kEdges.Contains(all));
  const WideSet rest = all.Minus(kEdges);
  EXPECT_EQ(rest.size(), kWideCount - 4);
  EXPECT_FALSE(rest.Has(Wide(64)));
  EXPECT_TRUE(rest.Has(Wide(65)));
  WideSet back = rest;
  back.Add(kEdges);
  EXPECT_EQ(back, all);
  WideSet one;
  EXPECT_TRUE(one.empty());
  one.Add(Wide(66));
  EXPECT_FALSE(one.empty());
  EXPECT_TRUE(all.Minus(all).empty());
  EXPECT_EQ(all.Minus(all).Names(), "none");
}

TEST(FeatureSet, NamesFollowDeclarationOrder) {
  EXPECT_EQ(FeatureSet::All().size(), kFeatureCount);
  EXPECT_EQ((FeatureSet{Feature::kTableName, Feature::kCountStar}).Names(),
            "count_star, table_name");
  EXPECT_EQ(FeatureSet{}.Names(), "none");
}

}  // namespace
}  // namespace antb1::slt
