// The shared comparator (canonical.h) that slt, oracle and diff tests rely on: tolerance, sort
// modes, NULL and (empty) cells, hash thresholds.

#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "canonical.h"
#include "engine.h"
#include "sha256.h"

namespace antb1::slt {
namespace {

using Row = std::vector<std::optional<std::string>>;

ResultSet Result(std::vector<ColumnClass> classes, std::vector<Row> rows) {
  std::vector<std::string> names(classes.size(), "T");
  return ResultSet{
      .classes = std::move(classes), .type_names = std::move(names), .rows = std::move(rows)};
}

bool Equal(const std::vector<std::string>& e, const std::vector<std::string>& a,
           std::string_view types, SortMode sort = SortMode::kNoSort,
           double tolerance = kDefaultRelTolerance) {
  return !CompareBlocks(e, a, types, sort, tolerance).has_value();
}

TEST(Comparator, RelativeAndAbsoluteToleranceOfRealColumns) {
  EXPECT_TRUE(Equal({"1000000000"}, {"1000000001"}, "R")) << "|d| <= 1e-12 + 1e-9 * 1e9";
  EXPECT_FALSE(Equal({"1000000000"}, {"1000000002"}, "R"));
  EXPECT_TRUE(Equal({"-2.5"}, {"-2.5000000000001"}, "R"));
  EXPECT_TRUE(Equal({"0"}, {"1e-12"}, "R")) << "the absolute part covers values near zero";
  EXPECT_FALSE(Equal({"0"}, {"3e-12"}, "R"));
  EXPECT_TRUE(Equal({"-0"}, {"0"}, "R"));
  EXPECT_FALSE(Equal({"1"}, {"-1"}, "R"));
  EXPECT_TRUE(Equal({"1.5"}, {"1.6"}, "R", SortMode::kNoSort, 0.1)) << "# tol overrides";
}

TEST(Comparator, SpecialAndNonNumericRealsCompareExactly) {
  EXPECT_TRUE(Equal({"nan"}, {"nan"}, "R"));
  EXPECT_TRUE(Equal({"inf"}, {"inf"}, "R"));
  EXPECT_FALSE(Equal({"inf"}, {"1e308"}, "R"));
  EXPECT_FALSE(Equal({"-inf"}, {"inf"}, "R"));
  EXPECT_FALSE(Equal({"nan"}, {"0"}, "R"));
  EXPECT_FALSE(Equal({"abc"}, {"xyz"}, "R"));
  EXPECT_FALSE(Equal({"NULL"}, {"(empty)"}, "R"));
}

TEST(Comparator, IntegerAndTextColumnsNeverUseTheTolerance) {
  EXPECT_FALSE(Equal({"1000000000"}, {"1000000001"}, "I"));
  EXPECT_FALSE(Equal({"1000000000"}, {"1000000001"}, "T"));
  EXPECT_TRUE(Equal({"7\t1000000000"}, {"7\t1000000001"}, "IR"));
  EXPECT_FALSE(Equal({"7\t1000000000"}, {"8\t1000000000"}, "IR"));
  EXPECT_FALSE(Equal({"1000000000\t7"}, {"1000000001\t7"}, "IR"));
  EXPECT_FALSE(Equal({"1\t2"}, {"1"}, "IR")) << "a missing cell";
}

TEST(Comparator, SortModes) {
  const auto a = Result({ColumnClass::kInteger, ColumnClass::kText},
                        {{"2", "b"}, {"1", std::nullopt}, {"10", ""}});
  const auto b = Result({ColumnClass::kInteger, ColumnClass::kText},
                        {{"10", ""}, {"2", "b"}, {"1", std::nullopt}});
  EXPECT_FALSE(Equal(RenderBlock(a, SortMode::kNoSort, 0), RenderBlock(b, SortMode::kNoSort, 0),
                     "IT", SortMode::kNoSort));
  EXPECT_TRUE(Equal(RenderBlock(a, SortMode::kRowSort, 0), RenderBlock(b, SortMode::kRowSort, 0),
                    "IT", SortMode::kRowSort));
  // valuesort ignores which row and column a value came from.
  const auto c = Result({ColumnClass::kInteger, ColumnClass::kText},
                        {{"2", ""}, {"10", "b"}, {"1", std::nullopt}});
  EXPECT_FALSE(Equal(RenderBlock(a, SortMode::kRowSort, 0), RenderBlock(c, SortMode::kRowSort, 0),
                     "IT", SortMode::kRowSort));
  EXPECT_TRUE(Equal(RenderBlock(a, SortMode::kValueSort, 0),
                    RenderBlock(c, SortMode::kValueSort, 0), "IT", SortMode::kValueSort));
  // valuesort with an R column compares every value numerically.
  EXPECT_TRUE(Equal({"0.30000000000000004", "1"}, {"0.3", "1"}, "IR", SortMode::kValueSort));
  const auto count = CompareBlocks({"1", "2"}, {"1"}, "I", SortMode::kValueSort, 0);
  ASSERT_TRUE(count.has_value());
  EXPECT_EQ(count.value_or(BlockDiff{}).reason, "expected 2 values, got 1");
}

TEST(Comparator, NullEmptyAndLookalikeStringsAreDistinct) {
  const std::vector<std::optional<std::string>> cells = {std::nullopt, "",  "NULL",
                                                         "(empty)",    " ", "\\x20"};
  for (std::size_t i = 0; i < cells.size(); ++i) {
    for (std::size_t j = 0; j < cells.size(); ++j) {
      EXPECT_EQ(SltCell(cells[i]) == SltCell(cells[j]), i == j) << i << " vs " << j;
    }
  }
  const auto a = Result({ColumnClass::kText}, {{std::nullopt}});
  const auto b = Result({ColumnClass::kText}, {{""}});
  EXPECT_FALSE(
      Equal(RenderBlock(a, SortMode::kNoSort, 0), RenderBlock(b, SortMode::kNoSort, 0), "T"));
}

TEST(Comparator, HashThreshold) {
  const auto r = Result({ColumnClass::kText}, {{"a"}, {std::nullopt}, {""}, {"b"}});
  EXPECT_EQ(RenderBlock(r, SortMode::kNoSort, 4).size(), 4U) << "at the threshold: not hashed";
  EXPECT_EQ(RenderBlock(r, SortMode::kNoSort, 0).size(), 4U) << "0: never hashed";
  const auto hashed = RenderBlock(r, SortMode::kNoSort, 3);
  ASSERT_EQ(hashed.size(), 1U);
  EXPECT_EQ(hashed[0], "4 values hashing to " + Sha256Hex("a\nNULL\n(empty)\nb\n"));
  // The hash sees NULL and (empty), the row order (nosort) and the count.
  const auto other = Result({ColumnClass::kText}, {{"a"}, {""}, {std::nullopt}, {"b"}});
  EXPECT_FALSE(Equal(hashed, RenderBlock(other, SortMode::kNoSort, 3), "T"));
  EXPECT_TRUE(Equal(RenderBlock(r, SortMode::kRowSort, 3),
                    RenderBlock(other, SortMode::kRowSort, 3), "T", SortMode::kRowSort));
  const std::string same_hash_other_count = "5" + hashed[0].substr(1);
  EXPECT_FALSE(Equal(hashed, {same_hash_other_count}, "T"));
  const auto mixed = CompareBlocks(hashed, RenderBlock(r, SortMode::kNoSort, 0), "T",
                                   SortMode::kNoSort, kDefaultRelTolerance);
  ASSERT_TRUE(mixed.has_value());
  EXPECT_EQ(mixed.value_or(BlockDiff{}).reason, "hashed results differ");
}

}  // namespace
}  // namespace antb1::slt
