// Tables files (runner/tables.h): files relative to the fixtures directory, globs, and the
// clickbench, redact and ref= options.

#include "tables.h"

#include <expected>
#include <filesystem>
#include <format>
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
                  .ends_with(":1: unknown option 'hidden' (known: clickbench, redact, ref=)"));
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

bool SameKey(const ForeignKey& key, const std::vector<std::string>& columns, std::string_view table,
             const std::vector<std::string>& ref_columns) {
  return key.columns == columns && key.table == table && key.ref_columns == ref_columns;
}

TEST_F(Tables, RefOptions) {
  // A forward reference (t to u), a self-reference (u.p), a two-column key, tables found in any
  // case and stored as their lines spell them, columns kept as written; refs stay in order.
  const auto tables = Load(
      "t   a.parquet  ref=Uid:U.id  redact  ref=uid+day:u.id+Day\n"
      "u   b1.parquet ref=p:U.id ref=id:T.tid\n"
      "v   b2.parquet\n");
  ASSERT_TRUE(tables.has_value()) << tables.error();
  ASSERT_EQ(tables->size(), 3U);
  const TableDef& t = (*tables)[0];
  EXPECT_TRUE(t.redact);
  ASSERT_EQ(t.refs.size(), 2U);
  EXPECT_TRUE(SameKey(t.refs[0], {"Uid"}, "u", {"id"}));
  EXPECT_TRUE(SameKey(t.refs[1], {"uid", "day"}, "u", {"id", "Day"}));
  const TableDef& u = (*tables)[1];
  ASSERT_EQ(u.refs.size(), 2U);
  EXPECT_TRUE(SameKey(u.refs[0], {"p"}, "u", {"id"}));
  EXPECT_TRUE(SameKey(u.refs[1], {"id"}, "t", {"tid"}));
  EXPECT_TRUE((*tables)[2].refs.empty());
}

TEST_F(Tables, RefErrors) {
  const std::string syntax =
      "': expected ref=<column>[+<column>...]:<table>.<column>[+<column>...]";
  for (const std::string_view option :
       {"ref=", "ref=x", "ref=x:u", "ref=x.u:y", "ref=:u.y", "ref=x:.y", "ref=x:u.", "ref=x+:u.y",
        "ref=x:u.y+", "ref=x++z:u.y+w", "ref=1x:u.y", "ref=x:u-v.y", "ref=x:u.y.z", "ref=x:u.y:z",
        "ref=\"x\":u.y"}) {
    const auto loaded = Load(std::format("u b1.parquet\nt a.parquet {}\n", option));
    ASSERT_FALSE(loaded.has_value()) << option;
    EXPECT_TRUE(loaded.error().ends_with(std::format(":2: ref option '{}{}", option, syntax)))
        << loaded.error();
  }
  EXPECT_TRUE(Load("t a.parquet ref=x+z:t.y\n")
                  .error()
                  .ends_with(":1: ref option 'ref=x+z:t.y': 2 column(s) reference 1"));
  EXPECT_TRUE(Load("t a.parquet ref=x:t.y+z\n")
                  .error()
                  .ends_with(":1: ref option 'ref=x:t.y+z': 1 column(s) reference 2"));
  EXPECT_TRUE(Load("t a.parquet ref=x+X:t.y+z\n")
                  .error()
                  .ends_with(":1: ref option 'ref=x+X:t.y+z': column 'X' repeats"));
  EXPECT_TRUE(Load("t a.parquet ref=x+z:t.y+Y\n")
                  .error()
                  .ends_with(":1: ref option 'ref=x+z:t.y+Y': column 'Y' repeats"));
  // The same key, in other cases: a duplicate (another target is not).
  EXPECT_TRUE(Load("t a.parquet ref=x:t.y ref=x:u.y ref=X:T.Y\n")
                  .error()
                  .ends_with(":1: duplicate ref option 'ref=X:T.Y'"));
  // An unknown table is reported at its option once every line is read.
  EXPECT_TRUE(Load("t a.parquet\nu b1.parquet ref=y:t.x ref=y:w.x\nv b2.parquet\n")
                  .error()
                  .ends_with(":2: ref option 'ref=y:w.x': no table 'w' in tables.txt"));
  EXPECT_TRUE(Load("t a.parquet ref=y:tt.x\n")
                  .error()
                  .ends_with(":1: ref option 'ref=y:tt.x': no table 'tt' in tables.txt"));
}

}  // namespace
}  // namespace antb1::slt
