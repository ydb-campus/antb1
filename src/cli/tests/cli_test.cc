#include "antb1/cli/cli.h"

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <gtest/gtest.h>
#include <parquet/arrow/writer.h>

#include "antb1/plan/sql_status.h"

namespace antb1::cli {
namespace {

namespace fs = std::filesystem;

struct Run {
  int code = -1;
  std::string out;
  std::string err;
};

Run Invoke(std::vector<std::string> args, const std::string& stdin_text = "") {
  args.insert(args.begin(), "antb1");
  std::istringstream in(stdin_text);
  std::ostringstream out;
  std::ostringstream err;
  Run r;
  r.code = RunCli(args, in, out, err);
  r.out = out.str();
  r.err = err.str();
  return r;
}

class CliTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::path(::testing::TempDir()) / "antb1_cli" /
           ::testing::UnitTest::GetInstance()->current_test_info()->name();
    fs::remove_all(dir_);
    fs::create_directories(dir_);
    path_ = (dir_ / "t.parquet").string();
    arrow::UInt16Builder d;
    for (int i = 0; i < 3; ++i) {
      ASSERT_TRUE(d.Append(19000).ok());
    }
    auto table = arrow::Table::Make(arrow::schema({arrow::field("EventDate", arrow::uint16())}),
                                    {d.Finish().ValueOrDie()});
    auto out = arrow::io::FileOutputStream::Open(path_).ValueOrDie();
    ASSERT_TRUE(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), out, 2).ok());
    ASSERT_TRUE(out->Close().ok());
  }
  void TearDown() override { fs::remove_all(dir_); }

  fs::path dir_;
  std::string path_;
};

TEST(ExitCodeTest, Mapping) {
  EXPECT_EQ(ExitCodeFor(arrow::Status::OK()), kExitOk);
  EXPECT_EQ(ExitCodeFor(plan::UnsupportedError("x", {})), kExitUnsupported);
  EXPECT_EQ(ExitCodeFor(plan::BindError("x", {})), kExitQueryError);
  EXPECT_EQ(ExitCodeFor(arrow::Status::IOError("x")), kExitIo);
  EXPECT_EQ(ExitCodeFor(arrow::Status::ExecutionError("overflow")), kExitQueryError);
  // A bare NotImplemented (e.g. a missing kernel) is a bug, not an "unsupported query".
  EXPECT_EQ(ExitCodeFor(arrow::Status::NotImplemented("kernel")), kExitInternal);
  EXPECT_EQ(ExitCodeFor(arrow::Status::TypeError("x")), kExitInternal);
}

TEST_F(CliTest, QueryCountStarJson) {
  auto r = Invoke(
      {"query", "-c", "SELECT COUNT(*) FROM t", "--table", "t=" + path_, "--format", "json"});
  EXPECT_EQ(r.code, kExitOk) << r.err;
  EXPECT_EQ(r.out, "[\n {\"count_star()\": 3}\n]\n");
}

TEST_F(CliTest, SqlFromStdinAndFile) {
  auto r = Invoke({"query", "-c", "-", "--table", "t=" + path_, "--format", "csv"},
                  "SELECT COUNT(*) FROM t");
  EXPECT_EQ(r.code, kExitOk) << r.err;
  EXPECT_EQ(r.out, "count_star()\n3\n");
  const auto sql_file = (dir_ / "q.sql").string();
  std::ofstream(sql_file) << "SELECT COUNT(*) FROM '" << path_ << "';\n";
  r = Invoke({"query", "-f", sql_file, "--format", "csv"});
  EXPECT_EQ(r.code, kExitOk) << r.err;
  EXPECT_EQ(r.out, "count_star()\n3\n");
}

TEST_F(CliTest, UnsupportedQueryExits4WithCaret) {
  auto r = Invoke({"query", "-c", "SELECT COUNT(*) FROM t GROUP BY x", "--table", "t=" + path_});
  EXPECT_EQ(r.code, kExitUnsupported);
  EXPECT_NE(r.err.find("unsupported error"), std::string::npos) << r.err;
  EXPECT_NE(r.err.find("^^^^^"), std::string::npos) << r.err;
}

TEST_F(CliTest, JsonErrorObject) {
  auto r = Invoke({"query", "-c", "SELECT COUNT(*) FROM nope", "--format", "json"});
  EXPECT_EQ(r.code, kExitQueryError);
  EXPECT_EQ(r.err,
            "{\"error\":{\"kind\":\"bind\",\"message\":\"table 'nope' does not exist\","
            "\"offset\":21,\"length\":4,\"line\":1,\"column\":22}}\n");
}

TEST_F(CliTest, UsageAndIoErrors) {
  auto missing_sql = Invoke({"query", "--format", "json"});
  EXPECT_EQ(missing_sql.code, kExitUsage);
  EXPECT_NE(missing_sql.err.find(R"("kind":"usage")"), std::string::npos) << missing_sql.err;
  EXPECT_NE(Invoke({"query", "-c", "SELECT 1", "--table", "broken"}).err.find("usage error"),
            std::string::npos);
  EXPECT_EQ(Invoke({"bogus"}).code, kExitUsage);
  EXPECT_EQ(Invoke({"query", "-c", "SELECT 1", "--table", "broken"}).code, kExitUsage);
  EXPECT_EQ(Invoke({"query", "-c", "SELECT COUNT(*) FROM t", "--table", "t=/nope.parquet"}).code,
            kExitIo);
  EXPECT_EQ(Invoke({"query", "-f", "/nope.sql"}).code, kExitIo);
}

TEST_F(CliTest, TimingIsLastStderrLineInFixedPoint) {
  auto r = Invoke({"query", "-c", "SELECT COUNT(*) FROM t", "--table", "t=" + path_, "--timing"});
  ASSERT_EQ(r.code, kExitOk);
  EXPECT_TRUE(std::regex_match(r.err, std::regex("^[0-9]+\\.[0-9]{6}\n$"))) << r.err;
}

TEST_F(CliTest, SchemaWithClickBenchOverride) {
  auto r = Invoke({"schema", "--table", "events=" + path_, "--clickbench"});
  EXPECT_EQ(r.code, kExitOk) << r.err;
  EXPECT_EQ(r.out, "events\n  EventDate\tDATE\n");
}

TEST_F(CliTest, ExplainAndVersion) {
  auto r = Invoke({"explain", "-c", "SELECT COUNT(*) FROM t", "--table", "t=" + path_});
  EXPECT_EQ(r.code, kExitOk) << r.err;
  EXPECT_NE(r.out.find("RowCount table=t"), std::string::npos);
  r = Invoke({"version"});
  EXPECT_EQ(r.code, kExitOk);
  EXPECT_NE(r.out.find("Apache Arrow 25."), std::string::npos) << r.out;
}

}  // namespace
}  // namespace antb1::cli
