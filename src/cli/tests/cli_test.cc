#include "antb1/cli/cli.h"

#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <arrow/util/config.h>
#include <gtest/gtest.h>
#include <parquet/arrow/writer.h>

#include "antb1/common/version.h"
#include "antb1/plan/sql_status.h"

namespace antb1::cli {
namespace {

namespace fs = std::filesystem;

// `antb1 bench --drop-caches` drops the Linux page cache (sudo); elsewhere it is a usage error.
#ifdef __linux__
constexpr bool kCanDropCaches = true;
#else
constexpr bool kCanDropCaches = false;
#endif

struct Run {
  int code = -1;
  std::string out;
  std::string err;
};

Run Invoke(std::vector<std::string> args, const std::string& stdin_text = "",
           const CliHooks& hooks = {}) {
  args.insert(args.begin(), "antb1");
  std::istringstream in(stdin_text);
  std::ostringstream out;
  std::ostringstream err;
  Run r;
  r.code = RunCli(args, in, out, err, hooks);
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

  std::string WriteFile(const std::string& name, const std::string& text) const {
    const auto path = (dir_ / name).string();
    std::ofstream(path, std::ios::binary) << text;
    return path;
  }

  fs::path dir_;
  std::string path_;
};

std::string ReadFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// A clock that advances 0.25 s per reading, a fixed date, and a page-cache drop that is counted.
CliHooks FakeHooks(const std::shared_ptr<int>& drops,
                   const arrow::Status& drop_result = arrow::Status::OK()) {
  auto now = std::make_shared<double>(0);
  return CliHooks{.seconds =
                      [now] {
                        *now += 0.25;
                        return *now;
                      },
                  .today = [] { return std::string("2026-01-02"); },
                  .drop_caches =
                      [drops, drop_result] {
                        ++*drops;
                        return drop_result;
                      }};
}

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

// The error object stays valid UTF-8 JSON when the SQL holds ill-formed UTF-8 (here an overlong
// NUL in a quoted identifier), escaped like --format json values.
TEST_F(CliTest, JsonErrorObjectEscapesIllFormedUtf8) {
  const std::string sql =
      "SELECT \"a\xC0\x80"
      "b\" FROM t";
  auto r = Invoke({"query", "-c", sql, "--table", "t=" + path_, "--format", "json"});
  EXPECT_EQ(r.code, kExitQueryError);
  EXPECT_NE(r.err.find(R"("message":"column 'a\\xc0\\x80b' does not exist")"), std::string::npos)
      << r.err;
  EXPECT_EQ(r.err.find('\xC0'), std::string::npos) << r.err;
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

TEST_F(CliTest, QueriesRunEveryShape) {
  auto r = Invoke({"query", "--clickbench", "-c",
                   "SELECT COUNT(*), MIN(EventDate), SUM(EventDate + 1) FROM t", "--table",
                   "t=" + path_});
  EXPECT_EQ(r.code, kExitUnsupported) << "arithmetic is outside the subset: " << r.err;
  r = Invoke({"query", "--clickbench", "-c",
              "SELECT COUNT(*) AS n, MIN(EventDate) FROM t WHERE EventDate >= '2022-01-08'",
              "--table", "t=" + path_, "--format", "csv"});
  EXPECT_EQ(r.code, kExitOk) << r.err;
  EXPECT_EQ(r.out, "n,min(EventDate)\n3,2022-01-08\n");
  r = Invoke({"query", "-c", "SELECT EventDate FROM t LIMIT 2", "--table", "t=" + path_, "--format",
              "csv"});
  EXPECT_EQ(r.code, kExitOk) << r.err;
  EXPECT_EQ(r.out, "EventDate\n19000\n19000\n");
}

TEST_F(CliTest, BenchWritesClickBenchJson) {
  const auto queries =
      WriteFile("q.sql",
                "SELECT COUNT(*) FROM t;\n\n  SELECT MIN(EventDate) FROM t WHERE EventDate > DATE "
                "'2000-01-01'\r\nSELECT COUNT(*) FROM t GROUP BY EventDate;\n");
  const auto out = (dir_ / "result.json").string();
  const auto drops = std::make_shared<int>(0);
  std::vector<std::string> args{"bench",      "--clickbench", "--queries", queries, "--table",
                                "t=" + path_, "--tries",      "2",         "--out", out,
                                "--machine",  "test machine", "--git-sha", "abc123"};
  if (kCanDropCaches) {
    args.emplace_back("--drop-caches");
  }
  const auto r = Invoke(args, "", FakeHooks(drops));
  ASSERT_EQ(r.code, kExitOk) << r.err;
  EXPECT_EQ(*drops, kCanDropCaches ? 3 : 0) << "once before the first try of every query";
  EXPECT_EQ(r.out, "");
  EXPECT_EQ(
      r.err,
      "Q0: 0.250000 0.250000\nQ1: 0.250000 0.250000\nQ2: unsupported error\n"
      "bench: 3 queries, 2 answered, 1 failed; wrote " +
          out +
          (kCanDropCaches ? " (cold: page cache dropped before each query)\n" : " (lukewarm)\n"));
  const auto expected = std::format(
      R"({{
  "system": "antb1",
  "date": "2026-01-02",
  "machine": "test machine",
  "cluster_size": 1,
  "proprietary": "no",
  "hardware": "cpu",
  "tuned": "no",
  "tags": ["C++", "column-oriented", "embedded", "stateless"],
  "load_time": 0.250000,
  "data_size": {},
  "concurrent_qps": null,
  "concurrent_error_ratio": null,
  "result": [
    [0.250000, 0.250000],
    [0.250000, 0.250000],
    [null, null]
  ],
  "antb1": {{
    "version": "{}",
    "git_sha": "abc123",
    "compiler": "{}",
    "arrow": "{}",
    "build_type": "{}",
    "cache": "{}",
    "tries": 2,
    "batch_size": 65536,
    "failed": [{{"query": 2, "kind": "unsupported"}}]
  }}
}}
)",
      fs::file_size(path_), Version(), CompilerVersion(), ARROW_VERSION_STRING, BuildType(),
      kCanDropCaches ? "cold" : "lukewarm");
  EXPECT_EQ(ReadFile(out), expected);
}

TEST_F(CliTest, BenchFailuresAndExitCodes) {
  const auto drops = std::make_shared<int>(0);
  // No queries: an empty result list.
  auto r = Invoke({"bench", "--queries", WriteFile("empty.sql", "\n \n"), "--table", "t=" + path_,
                   "--out", "-"},
                  "", FakeHooks(drops));
  EXPECT_EQ(r.code, kExitOk) << r.err;
  EXPECT_NE(r.out.find("\"result\": [],\n"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("\"git_sha\": null"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("\"cache\": \"lukewarm\""), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("\"failed\": []"), std::string::npos) << r.out;
  EXPECT_EQ(*drops, 0);
  // An I/O error in a query: the JSON is written, the exit code reports it.
  const auto io_queries =
      WriteFile("io.sql", "SELECT COUNT(*) FROM t\nSELECT COUNT(*) FROM '" +
                              (dir_ / "none.parquet").string() + "'\nSELECT nope FROM t\n");
  const auto out = (dir_ / "io.json").string();
  r = Invoke({"bench", "--queries", io_queries, "--table", "t=" + path_, "--out", out}, "",
             FakeHooks(drops));
  EXPECT_EQ(r.code, kExitIo) << r.err;
  EXPECT_NE(r.err.find("Q1: io error\nQ2: bind error\n"), std::string::npos) << r.err;
  EXPECT_NE(ReadFile(out).find("[{\"query\": 1, \"kind\": \"io\"}, {\"query\": 2, "
                               "\"kind\": \"bind\"}]"),
            std::string::npos);
  // A page cache that cannot be dropped stops the run; off Linux the option is a usage error.
  r = Invoke(
      {"bench", "--queries", io_queries, "--table", "t=" + path_, "--out", out, "--drop-caches"},
      "", FakeHooks(drops, arrow::Status::IOError("sudo: a password is required")));
  if (kCanDropCaches) {
    EXPECT_EQ(r.code, kExitIo);
    EXPECT_NE(r.err.find("cannot drop the page cache: sudo: a password is required"),
              std::string::npos)
        << r.err;
  } else {
    EXPECT_EQ(r.code, kExitUsage);
    EXPECT_NE(r.err.find("--drop-caches is only supported on Linux"), std::string::npos) << r.err;
  }
  // Unreadable query files, unwritable results, bad tables and options.
  r = Invoke(
      {"bench", "--queries", (dir_ / "none.sql").string(), "--table", "t=" + path_, "--out", "-"});
  EXPECT_EQ(r.code, kExitIo);
  r = Invoke({"bench", "--queries", io_queries, "--table", "t=" + path_, "--out",
              (dir_ / "no" / "such" / "dir.json").string()},
             "", FakeHooks(drops));
  EXPECT_EQ(r.code, kExitIo);
  EXPECT_NE(r.err.find("cannot write"), std::string::npos) << r.err;
  EXPECT_EQ(
      Invoke({"bench", "--queries", io_queries, "--table", "t=/none.parquet", "--out", "-"}).code,
      kExitIo);
  EXPECT_EQ(Invoke({"bench", "--queries", io_queries, "--out", "-", "--tries", "0"}).code,
            kExitUsage);
  EXPECT_EQ(Invoke({"bench", "--queries", io_queries}).code, kExitUsage) << "--out is required";
}

TEST(RunCommandTest, ExitStatusAndErrors) {
  EXPECT_TRUE(RunCommand({"true"}).ok());
  const auto failed = RunCommand({"false"});
  EXPECT_TRUE(failed.IsIOError());
  EXPECT_NE(failed.message().find("exit code 1"), std::string::npos) << failed.message();
  EXPECT_TRUE(RunCommand({"sh", "-c", "exit 3"}).IsIOError());
  const auto killed = RunCommand({"sh", "-c", "kill -9 $$"});
  EXPECT_NE(killed.message().find("was killed"), std::string::npos) << killed.ToString();
  EXPECT_TRUE(RunCommand({"antb1-no-such-program-anywhere"}).IsIOError());
  EXPECT_TRUE(RunCommand({}).IsInvalid());
}

}  // namespace
}  // namespace antb1::cli
