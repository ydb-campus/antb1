#include "antb1/io/glob.h"

#include <gtest/gtest.h>

namespace antb1::io {
namespace {

TEST(GlobTest, LiteralPathIsReturnedAsIs) {
  auto files = ExpandGlob("/definitely/not/here.parquet");
  ASSERT_TRUE(files.ok());
  ASSERT_EQ(files->size(), 1U);
  EXPECT_EQ((*files)[0], "/definitely/not/here.parquet");
}

TEST(GlobTest, NoMatchIsIOError) {
  EXPECT_TRUE(ExpandGlob("/definitely/not/here/*.parquet").status().IsIOError());
}

}  // namespace
}  // namespace antb1::io

namespace antb1::io {
namespace {

TEST(GlobTest, WildcardInDirectoryIsRejected) {
  EXPECT_TRUE(ExpandGlob("/tmp/*/x.parquet").status().IsInvalid());
}

}  // namespace
}  // namespace antb1::io
