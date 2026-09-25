#include "antb1/common/source_span.h"

#include <gtest/gtest.h>

namespace antb1 {
namespace {

TEST(SourceSpanTest, LineColumnCountsNewlines) {
  constexpr std::string_view kText = "SELECT\n  x\nFROM t";
  const auto lc = ToLineColumn(kText, SourceSpan{.offset = 9, .length = 1});
  EXPECT_EQ(lc.line, 2U);
  EXPECT_EQ(lc.column, 3U);
}

TEST(SourceSpanTest, OffsetPastEndIsClamped) {
  const auto lc = ToLineColumn("ab", SourceSpan{.offset = 99, .length = 0});
  EXPECT_EQ(lc.line, 1U);
  EXPECT_EQ(lc.column, 3U);
}

}  // namespace
}  // namespace antb1
