#include "ratchet.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
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

// The ratchet core with the settings of the queries derived from TPC-H (antb1-slt tpch): numbered
// from 1, only Unsupported a clean failure (C18), "running" lines and optional times. The
// ClickBench settings are covered through clickbench_test.cc.

namespace antb1::slt {
namespace {

constexpr Workload kNumberedFromOne{
    .summary = "TPCH",
    .commit_key = "",
    .first = 1,
    .rejections_clean = false,
    .unclean_rule = "only Unsupported is clean here",
    .table = "the test table",
    .running_lines = true,
};

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

ExecResult Error(const std::string& kind, bool unsupported = false) {
  return std::unexpected(
      EngineError{.kind = kind, .message = kind + ": SECRET_MESSAGE", .unsupported = unsupported});
}

TEST(ParseRatchetStatus, KeysPerWorkload) {
  const auto plain = ParseRatchetStatus(R"({"pass": [1, 6]})", kNumberedFromOne);
  ASSERT_TRUE(plain.has_value()) << plain.error();
  EXPECT_EQ(plain->pass, (std::vector<int64_t>{1, 6}));
  EXPECT_TRUE(plain->commit.empty());
  ASSERT_TRUE(ParseRatchetStatus("{\n  \"pass\": []\n}\n", kNumberedFromOne).has_value());

  EXPECT_FALSE(ParseRatchetStatus("{}", kNumberedFromOne).has_value());
  EXPECT_FALSE(ParseRatchetStatus("{", kNumberedFromOne).has_value());
  const auto commit =
      ParseRatchetStatus(R"({"clickbench_commit": "x", "pass": []})", kNumberedFromOne);
  ASSERT_FALSE(commit.has_value()) << "a key the workload does not have";
  EXPECT_TRUE(commit.error().contains("unexpected or repeated key \"clickbench_commit\""));
  EXPECT_FALSE(ParseRatchetStatus(R"({"pass": [2, 1]})", kNumberedFromOne).has_value());
}

// Q1 is answered by both engines; Q2 is Unsupported; Q3 is Unsupported too.
struct Fixture {
  std::vector<Statement> queries = {{.line = 1, .sql = "SELECT SECRET_Q1", .features = {}},
                                    {.line = 1, .sql = "SELECT SECRET_Q2", .features = {}},
                                    {.line = 1, .sql = "SELECT SECRET_Q3", .features = {}}};
  std::map<std::string, ExecResult> antb1_answers = {
      {"SELECT SECRET_Q1", Int("1")},
      {"SELECT SECRET_Q2", Error("unsupported", /*unsupported=*/true)},
      {"SELECT SECRET_Q3", Error("unsupported", /*unsupported=*/true)}};
  std::map<std::string, ExecResult> oracle_answers = {{"SELECT SECRET_Q1", Int("1")}};
  std::function<double()> clock;

  struct Result {
    RatchetStats stats;
    std::string out;
    std::vector<std::string> writes;
  };

  Result Run(std::vector<int64_t> pass, bool redact = false,
             std::optional<uint64_t> only = std::nullopt) {
    ScriptedEngine a("antb1", [this](const std::string& sql) { return antb1_answers.at(sql); });
    ScriptedEngine o("duckdb", [this](const std::string& sql) { return oracle_answers.at(sql); });
    Result r;
    r.stats = RunRatchet(queries, {.commit = {}, .pass = std::move(pass)}, kNumberedFromOne, a, o,
                         {.redact = redact,
                          .only = only,
                          .status_path = "status.json",
                          .command = "cmd",
                          .clock = clock},
                         [&r](std::string_view text) {
                           r.out += text;
                           r.writes.emplace_back(text);
                         });
    return r;
  }
};

TEST(RunRatchet, NumbersFromTheFirstQueryAndLogsEachBeforeItRuns) {
  Fixture f;
  const auto r = f.Run({1});
  EXPECT_EQ(r.stats.failed, 0) << r.out;
  EXPECT_EQ(r.stats.passed, (std::vector<int64_t>{1}));
  EXPECT_EQ(r.stats.unsupported, 2);
  EXPECT_EQ(r.out,
            "Q1: running\nQ1: pass\nQ2: running\nQ2: unsupported\nQ3: running\nQ3: unsupported\n"
            "TPCH: PASS queries=3 pass=[1] unsupported=2 rejected=0 failed=0 ratchet=[1]\n");
  EXPECT_EQ(r.writes.front(), "Q1: running\n") << "written before the query runs";
}

// Only Unsupported is clean: a parse or bind error fails, and so does an execution error.
TEST(RunRatchet, ParseAndBindErrorsAreUnclean) {
  for (const char* kind : {"parse", "bind", "execution", "io"}) {
    Fixture f;
    f.antb1_answers["SELECT SECRET_Q2"] = Error(kind);
    const auto r = f.Run({1});
    EXPECT_EQ(r.stats.failed, 1) << kind << "\n" << r.out;
    EXPECT_EQ(r.stats.rejected, 0);
    const std::string article = std::string_view("aeiou").contains(kind[0]) ? "an" : "a";
    EXPECT_TRUE(
        r.out.contains(std::format("FAIL Q2: antb1 fails with {} {} error; only "
                                   "Unsupported is clean here",
                                   article, kind)))
        << r.out;
    EXPECT_TRUE(r.out.contains("cmd --only 2\n")) << r.out;
  }
}

TEST(RunRatchet, UnexpectedPassFailAndNumbersOutsideTheQueries) {
  Fixture f;
  const auto pass = f.Run({});
  EXPECT_EQ(pass.stats.failed, 1);
  EXPECT_TRUE(
      pass.out.contains("FAIL Q1: unexpected pass: antb1 answers Q1 and equals DuckDB, but "
                        "the ratchet status.json does not list it. Add 1 to \"pass\" there "
                        "and mark Q1 `pass` in the test table of docs/sql-subset.md"))
      << pass.out;
  const auto fail = f.Run({1, 2});
  EXPECT_EQ(fail.stats.failed, 1);
  EXPECT_TRUE(
      fail.out.contains("FAIL Q2: unexpected fail: the ratchet status.json lists Q2, but "
                        "it no longer passes (unsupported)"))
      << fail.out;
  const auto outside = f.Run({0, 1, 4});
  EXPECT_EQ(outside.stats.failed, 2) << outside.out;
  EXPECT_TRUE(outside.out.contains("FAIL Q0: status.json lists Q0, but the query file has 3"));
  EXPECT_TRUE(outside.out.contains("FAIL Q4: status.json lists Q4, but the query file has 3"));
}

TEST(RunRatchet, OnlyTakesAQueryNumber) {
  Fixture f;
  const auto r = f.Run({1}, false, 3);
  EXPECT_EQ(r.stats.queries, 1);
  EXPECT_EQ(r.stats.failed, 0) << r.out;
  EXPECT_TRUE(r.out.starts_with("Q3: running\nQ3: unsupported\n")) << r.out;
  for (const uint64_t bad : {uint64_t{0}, uint64_t{4}}) {
    const auto none = f.Run({1}, false, bad);
    EXPECT_EQ(none.stats.queries, 0);
    EXPECT_TRUE(none.out.contains(std::format("FAIL Q{}: no such query", bad))) << none.out;
  }
}

TEST(RunRatchet, RedactedWrongAnswersShowNoQueryTextOrValues) {
  Fixture f;
  f.antb1_answers["SELECT SECRET_Q1"] = Int("SECRET_VALUE");
  f.antb1_answers["SELECT SECRET_Q3"] = Error("bind");
  const auto r = f.Run({1}, /*redact=*/true);
  EXPECT_EQ(r.stats.failed, 2) << r.out;
  EXPECT_TRUE(r.out.contains("Q1: wrong answer\n")) << r.out;
  EXPECT_TRUE(r.out.contains("sha256: DuckDB")) << r.out;
  EXPECT_TRUE(r.out.contains("error kind: bind")) << r.out;
  EXPECT_TRUE(r.out.contains("repro (unredacted, prints values and query text; run it locally)"));
  EXPECT_FALSE(r.out.contains("SECRET")) << r.out;
}

// With a clock (ANTB1_TPCH_TIMES=1): each outcome line has the query's seconds, and the summary
// the geometric mean of the passing queries'.
TEST(RunRatchet, TimesFromTheClock) {
  Fixture f;
  f.antb1_answers["SELECT SECRET_Q2"] = Int("2");
  f.oracle_answers["SELECT SECRET_Q2"] = Int("2");
  int calls = 0;
  // Q1 takes 2 s, Q2 8 s, Q3 0 s (shown as the floor of 1 microsecond).
  const std::vector<double> steps = {0, 2, 2, 10, 10, 10};
  f.clock = [&] { return steps.at(static_cast<std::size_t>(calls++)); };
  const auto r = f.Run({1, 2});
  EXPECT_EQ(r.stats.failed, 0) << r.out;
  EXPECT_TRUE(r.out.contains("Q1: pass (2.000 s)\n")) << r.out;
  EXPECT_TRUE(r.out.contains("Q2: pass (8.000 s)\n")) << r.out;
  EXPECT_TRUE(r.out.contains("Q3: unsupported (0.000 s)\n")) << r.out;
  EXPECT_TRUE(r.out.contains(" ratchet=[1, 2] geomean=4.000s\n")) << r.out;

  Fixture none;
  none.clock = [] { return 0.0; };
  const auto r2 = none.Run({}, false, 2);
  EXPECT_TRUE(r2.out.contains(" geomean=n/a\n")) << r2.out;
}

}  // namespace
}  // namespace antb1::slt
