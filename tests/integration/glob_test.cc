// Tables over several files and globs (label integration): the split fixture
// (hits_like_split/part-{0..3}.parquet: 1000, 3000, 2500 and 3500 rows) through every form.

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "antb1/engine/session.h"
#include "antb1/io/parquet_table.h"

#include "integration_util.h"

namespace antb1::integration {
namespace {

std::string Part(const std::string& name) { return Fixture("hits_like_split/" + name); }

TEST(Glob, PatternsOverTheSplitTable) {
  const std::vector<std::pair<std::string, int64_t>> cases = {
      {"part-*.parquet", 10'000},   {"part-?.parquet", 10'000},   {"*.parquet", 10'000},
      {"part-[01].parquet", 4'000}, {"part-[23].parquet", 6'000}, {"part-[!0].parquet", 9'000},
      {"part-3.parquet", 3'500},
  };
  for (const auto& [pattern, rows] : cases) {
    auto session = NewSession();
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(session->RegisterParquet("t", {Part(pattern)}).ok()) << pattern;
    auto registered = Count(*session, "SELECT COUNT(*) FROM t");
    ASSERT_TRUE(registered.ok()) << pattern << ": " << registered.status().ToString();
    EXPECT_EQ(*registered, rows) << pattern;
    auto by_path = Count(*session, FromPath(Part(pattern)));
    ASSERT_TRUE(by_path.ok()) << pattern << ": " << by_path.status().ToString();
    EXPECT_EQ(*by_path, rows) << pattern;
  }
}

TEST(Glob, ListsOfFilesAndGlobs) {
  const std::vector<std::pair<std::vector<std::string>, int64_t>> cases = {
      {{Part("part-0.parquet"), Part("part-2.parquet")}, 3'500},
      {{Part("part-0.parquet"), Part("part-[12].parquet")}, 6'500},
      {{Part("part-[01].parquet"), Part("part-[23].parquet")}, 10'000},
  };
  for (const auto& [paths, rows] : cases) {
    auto session = NewSession();
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(session->RegisterParquet("t", paths).ok());
    auto count = Count(*session, "SELECT COUNT(*) FROM t");
    ASSERT_TRUE(count.ok()) << count.status().ToString();
    EXPECT_EQ(*count, rows);
  }
}

TEST(Glob, ExpandsToSortedFiles) {
  auto session = NewSession();
  ASSERT_NE(session, nullptr);
  ASSERT_TRUE(session->RegisterParquet("t", {Part("part-*.parquet")}).ok());
  const auto table = std::dynamic_pointer_cast<io::ParquetTable>(session->catalog().Find("t"));
  ASSERT_NE(table, nullptr);
  EXPECT_EQ(table->files(),
            (std::vector<std::string>{Part("part-0.parquet"), Part("part-1.parquet"),
                                      Part("part-2.parquet"), Part("part-3.parquet")}));
  EXPECT_EQ(table->exact_row_count(), 10'000);
}

TEST(Glob, WildcardsOnlyInTheFileName) {
  auto session = NewSession();
  ASSERT_NE(session, nullptr);
  const auto status = session->RegisterParquet("t", {Fixture("hits_like_*/part-0.parquet")});
  EXPECT_FALSE(status.ok());
  EXPECT_NE(status.message().find("wildcards are only supported in the file name"),
            std::string::npos)
      << status.ToString();
}

}  // namespace
}  // namespace antb1::integration
