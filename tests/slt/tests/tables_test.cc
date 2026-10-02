// Tables files (runner/tables.h): files relative to the fixtures directory, globs, and the
// clickbench and redact options.

#include "tables.h"

#include <expected>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

namespace antb1::slt {
namespace {

namespace fs = std::filesystem;

class Tables : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::path(::testing::TempDir()) / "antb1_tables" /
           ::testing::UnitTest::GetInstance()->current_test_info()->name();
    std::error_code ec;
    fs::remove_all(dir_, ec);
    fs::create_directories(dir_ / "fixtures");
    for (const std::string_view name : {"a.parquet", "b1.parquet", "b2.parquet"}) {
      std::ofstream(dir_ / "fixtures" / name) << "";
    }
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  [[nodiscard]] std::expected<std::vector<TableDef>, std::string> Load(
      std::string_view text) const {
    std::ofstream(dir_ / "tables.txt") << text;
    return LoadTables(dir_ / "tables.txt", dir_ / "fixtures");
  }

  [[nodiscard]] std::string Fixture(std::string_view name) const {
    return (dir_ / "fixtures" / name).string();
  }

  fs::path dir_;
};

TEST_F(Tables, FilesGlobsAndOptions) {
  const auto tables = Load(
      "# comment\n"
      "\n"
      "plain  a.parquet\n"
      "wide   b*.parquet  clickbench\n"
      "secret a.parquet,b1.parquet  redact\n"
      "both   a.parquet  redact clickbench\n");
  ASSERT_TRUE(tables.has_value()) << tables.error();
  ASSERT_EQ(tables->size(), 4U);
  const TableDef& plain = (*tables)[0];
  EXPECT_EQ(plain.name, "plain");
  EXPECT_EQ(plain.files, std::vector<std::string>{Fixture("a.parquet")});
  EXPECT_FALSE(plain.clickbench);
  EXPECT_FALSE(plain.redact);
  const TableDef& wide = (*tables)[1];
  EXPECT_EQ(wide.files, (std::vector<std::string>{Fixture("b1.parquet"), Fixture("b2.parquet")}));
  EXPECT_EQ(wide.patterns, std::vector<std::string>{Fixture("b*.parquet")});
  EXPECT_TRUE(wide.clickbench);
  EXPECT_FALSE(wide.redact);
  const TableDef& secret = (*tables)[2];
  EXPECT_EQ(secret.files.size(), 2U);
  EXPECT_FALSE(secret.clickbench);
  EXPECT_TRUE(secret.redact);
  EXPECT_TRUE((*tables)[3].clickbench);
  EXPECT_TRUE((*tables)[3].redact);
}

TEST_F(Tables, Errors) {
  EXPECT_TRUE(Load("t a.parquet hidden\n")
                  .error()
                  .ends_with(":1: unknown option 'hidden' (known: clickbench, redact)"));
  EXPECT_TRUE(Load("t a.parquet\nT b1.parquet\n").error().ends_with(":2: duplicate table 'T'"));
  EXPECT_TRUE(
      Load("t\n").error().ends_with(":1: expected '<name> <file>[,<file>...] [option...]'"));
  // A missing fixture names the tests that write them.
  const std::string missing = Load("t tpch/sf/x.parquet redact\n").error();
  EXPECT_TRUE(missing.contains("fixture '" + Fixture("tpch/sf/x.parquet") + "' not found"))
      << missing;
  EXPECT_TRUE(missing.contains("fixtures.generate")) << missing;
  EXPECT_TRUE(missing.contains("fixtures.tpch")) << missing;
}

}  // namespace
}  // namespace antb1::slt
