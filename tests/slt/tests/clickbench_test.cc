#include "clickbench.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "engine.h"
#include "query_file.h"

namespace antb1::slt {
namespace {

class ScriptedEngine final : public Engine {
 public:
  ScriptedEngine(std::string name, std::function<ExecResult(const std::string&)> answer)
      : name_(std::move(name)), answer_(std::move(answer)) {}

  [[nodiscard]] std::string_view name() const override { return name_; }
  ExecResult Execute(const std::string& sql) override {
    executed.push_back(sql);
    return answer_(sql);
  }

  std::vector<std::string> executed;

 private:
  std::string name_;
  std::function<ExecResult(const std::string&)> answer_;
};

ResultSet Int(std::string value) {
  return ResultSet{
      .classes = {ColumnClass::kInteger}, .type_names = {"BIGINT"}, .rows = {{std::move(value)}}};
}

ExecResult Error(const std::string& kind, bool unsupported = false, bool internal = false) {
  return std::unexpected(EngineError{.kind = kind,
                                     .message = kind + ": SECRET_MESSAGE",
                                     .unsupported = unsupported,
                                     .internal = internal});
}

TEST(ParseClickBenchStatus, ValidFiles) {
  const auto status =
      ParseClickBenchStatus("{\n  \"clickbench_commit\": \"abc123\",\n  \"pass\": [0, 1, 6]\n}\n");
  ASSERT_TRUE(status.has_value()) << status.error();
  EXPECT_EQ(status->commit, "abc123");
  EXPECT_EQ(status->pass, (std::vector<int64_t>{0, 1, 6}));
  const auto empty = ParseClickBenchStatus(R"({"pass": [], "clickbench_commit": "x"})");
  ASSERT_TRUE(empty.has_value()) << empty.error();
  EXPECT_TRUE(empty->pass.empty());
}

TEST(ParseClickBenchStatus, Errors) {
  EXPECT_FALSE(ParseClickBenchStatus("").has_value());
  EXPECT_FALSE(ParseClickBenchStatus(R"({"pass": [0]})").has_value()) << "no commit";
  EXPECT_FALSE(ParseClickBenchStatus(R"({"clickbench_commit": "x"})").has_value()) << "no pass";
  EXPECT_FALSE(ParseClickBenchStatus(R"({"clickbench_commit": "x", "pass": [1, 0]})").has_value())
      << "unsorted";
  EXPECT_FALSE(ParseClickBenchStatus(R"({"clickbench_commit": "x", "pass": [1, 1]})").has_value())
      << "duplicate";
  EXPECT_FALSE(ParseClickBenchStatus(R"({"clickbench_commit": "x", "pass": [-1]})").has_value());
  EXPECT_FALSE(
      ParseClickBenchStatus(R"({"clickbench_commit": "x", "pass": [0], "other": 1})").has_value());
  EXPECT_FALSE(ParseClickBenchStatus(R"({"clickbench_commit": "x", "pass": [0]} x)").has_value());
}

// Q0 is answered by both engines; Q1 is Unsupported; Q2 is a parse error in antb1.
struct Fixture {
  std::vector<Statement> queries = {{.line = 1, .sql = "SELECT SECRET_Q0", .features = {}},
                                    {.line = 2, .sql = "SELECT SECRET_Q1", .features = {}},
                                    {.line = 3, .sql = "SELECT SECRET_Q2", .features = {}}};
  std::map<std::string, ExecResult> antb1_answers = {
      {"SELECT SECRET_Q0", Int("42")},
      {"SELECT SECRET_Q1", Error("unsupported", true)},
      {"SELECT SECRET_Q2", Error("parse")}};
  std::map<std::string, ExecResult> oracle_answers = {{"SELECT SECRET_Q0", Int("42")}};

  struct Result {
    ClickBenchStats stats;
    std::string out;
    std::vector<std::string> oracle_ran;
  };

  Result Run(std::vector<int64_t> pass, bool redact = false,
             std::optional<uint64_t> only = std::nullopt) {
    ScriptedEngine a("antb1", [this](const std::string& sql) { return antb1_answers.at(sql); });
    ScriptedEngine o("duckdb", [this](const std::string& sql) {
      const auto it = oracle_answers.find(sql);
      return it != oracle_answers.end() ? it->second : Error("Binder Error");
    });
    Result r;
    r.stats = RunClickBench(
        queries, {.commit = "c", .pass = std::move(pass)}, a, o,
        {.redact = redact, .only = only, .status_path = "status.json", .command = "cmd"}, r.out);
    r.oracle_ran = o.executed;
    return r;
  }
};

TEST(RunClickBench, PassesWhenTheRatchetMatches) {
  Fixture f;
  const auto r = f.Run({0});
  EXPECT_EQ(r.stats.failed, 0) << r.out;
  EXPECT_EQ(r.stats.passed, (std::vector<int64_t>{0}));
  EXPECT_EQ(r.stats.unsupported, 1);
  EXPECT_EQ(r.stats.rejected, 1);
  EXPECT_TRUE(r.out.contains("Q0: pass\nQ1: unsupported\nQ2: parse error\n")) << r.out;
  EXPECT_TRUE(r.out.contains("CLICKBENCH: PASS queries=3 pass=[0]")) << r.out;
  EXPECT_EQ(r.oracle_ran, (std::vector<std::string>{"SELECT SECRET_Q0"}))
      << "DuckDB runs only the queries antb1 answers";
}

TEST(RunClickBench, UnexpectedPassAndFail) {
  Fixture f;
  const auto pass = f.Run({});
  EXPECT_EQ(pass.stats.failed, 1);
  EXPECT_TRUE(pass.out.contains("FAIL Q0: unexpected pass")) << pass.out;
  const auto fail = f.Run({0, 1});
  EXPECT_EQ(fail.stats.failed, 1);
  EXPECT_TRUE(fail.out.contains("FAIL Q1: unexpected fail")) << fail.out;
  const auto out_of_range = f.Run({0, 7});
  EXPECT_TRUE(out_of_range.out.contains("status.json lists Q7, but the query file has 3 queries"))
      << out_of_range.out;
}

TEST(RunClickBench, WrongAnswersAndUncleanFailuresFail) {
  Fixture f;
  f.oracle_answers["SELECT SECRET_Q0"] = Int("43");
  const auto wrong = f.Run({0});
  EXPECT_EQ(wrong.stats.failed, 1) << wrong.out;
  EXPECT_TRUE(wrong.out.contains("FAIL Q0: result mismatch")) << wrong.out;

  Fixture g;
  g.antb1_answers["SELECT SECRET_Q1"] = Error("execution");
  const auto unclean = g.Run({0});
  EXPECT_EQ(unclean.stats.failed, 1) << unclean.out;
  EXPECT_TRUE(unclean.out.contains("FAIL Q1: antb1 fails with an execution error")) << unclean.out;
  EXPECT_TRUE(unclean.out.contains("cmd --only 1")) << unclean.out;
}

TEST(RunClickBench, RedactedReportsHoldNoQueryTextValuesOrMessages) {
  Fixture f;
  f.antb1_answers["SELECT SECRET_Q0"] = Int("SECRET_VALUE");
  f.antb1_answers["SELECT SECRET_Q1"] = Error("io");
  const auto r = f.Run({0}, /*redact=*/true);
  EXPECT_EQ(r.stats.failed, 2) << r.out;
  EXPECT_TRUE(r.out.contains("sha256: DuckDB")) << r.out;
  EXPECT_TRUE(r.out.contains("error kind: io")) << r.out;
  EXPECT_FALSE(r.out.contains("SECRET")) << r.out;
}

TEST(RunClickBench, OnlyRunsOneQuery) {
  Fixture f;
  const auto r = f.Run({0}, false, 1);
  EXPECT_EQ(r.stats.queries, 1);
  EXPECT_EQ(r.stats.failed, 0) << r.out;
  EXPECT_TRUE(r.oracle_ran.empty());
  const auto bad = f.Run({0}, false, 9);
  EXPECT_EQ(bad.stats.failed, 1);
}

}  // namespace
}  // namespace antb1::slt
