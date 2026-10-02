// `antb1-slt answers` (runner/answers.h): numbered query and answer files, answer text, the
// comparison by column class and the reports. Made-up queries, answers and sentinels only.

#include "answers.h"

#include <algorithm>
#include <cctype>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "canonical.h"
#include "engine.h"
#include "result_diff.h"
#include "sha256.h"

namespace antb1::slt {
namespace {

namespace fs = std::filesystem;

using Row = std::vector<std::optional<std::string>>;

class ScriptedEngine final : public Engine {
 public:
  explicit ScriptedEngine(std::function<ExecResult(const std::string&)> answer)
      : answer_(std::move(answer)) {}

  [[nodiscard]] std::string_view name() const override { return "duckdb"; }
  ExecResult Execute(const std::string& sql) override { return answer_(sql); }

 private:
  std::function<ExecResult(const std::string&)> answer_;
};

ResultSet Ints(const std::vector<std::string>& values) {
  ResultSet r{.classes = {ColumnClass::kInteger}, .type_names = {"BIGINT"}, .rows = {}};
  for (const auto& v : values) {
    r.rows.push_back({v});
  }
  return r;
}

AnswerTable Answer(std::string_view text) {
  auto table = ParseAnswerText(text);
  EXPECT_TRUE(table.has_value()) << table.error();
  return table.value_or(AnswerTable{});
}

std::vector<AnswerQuery> Queries(const std::vector<std::pair<std::string, std::string>>& items) {
  std::vector<AnswerQuery> queries;
  for (const auto& [sql, answer] : items) {
    const int n = static_cast<int>(queries.size()) + 1;
    queries.push_back(AnswerQuery{.number = n,
                                  .query_path = std::format("q{:02}.sql", n),
                                  .answer_path = std::format("q{:02}.csv", n),
                                  .statement = {.line = 1, .sql = sql, .features = std::nullopt},
                                  .answer = answer});
  }
  return queries;
}

std::string Lower(std::string text) {
  std::ranges::transform(text, text.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

TEST(ParseAnswerText, HeaderRowsAndNulls) {
  const auto t = ParseAnswerText("a|b\n1|x\n|\n2|y z\n");
  ASSERT_TRUE(t.has_value()) << t.error();
  EXPECT_EQ(t->columns, 2U);
  ASSERT_EQ(t->rows.size(), 3U);
  EXPECT_EQ(t->rows[0], (Row{"1", "x"}));
  EXPECT_EQ(t->rows[1], (Row{std::nullopt, std::nullopt}));
  EXPECT_EQ(t->rows[2], (Row{"2", "y z"}));
}

TEST(ParseAnswerText, OnlyTheLastLineBreakEndsTheText) {
  EXPECT_TRUE(Answer("n\n").rows.empty());
  EXPECT_TRUE(Answer("n").rows.empty());
  const AnswerTable null_row = Answer("n\n\n");  // a one-column row: NULL
  ASSERT_EQ(null_row.rows.size(), 1U);
  EXPECT_EQ(null_row.rows[0], (Row{std::nullopt}));
  EXPECT_EQ(Answer("n\n1").rows.size(), 1U);
}

TEST(ParseAnswerText, NothingIsTrimmed) {
  const AnswerTable t = Answer("a|b\n 1 |x\r\n");
  ASSERT_EQ(t.rows.size(), 1U);
  EXPECT_EQ(t.rows[0], (Row{" 1 ", "x\r"}));
}

TEST(ParseAnswerText, ErrorsGiveLineNumbersAndCountsOnly) {
  EXPECT_FALSE(ParseAnswerText("").has_value());
  const auto t = ParseAnswerText("a|b\n1|2\nantb1_canary_cell\n");
  ASSERT_FALSE(t.has_value());
  EXPECT_EQ(t.error(), "line 3: 1 field(s), the header has 2");
  EXPECT_FALSE(ParseAnswerText("a\n1|2\n").has_value());
}

TEST(NormalizeDecimal, OneSpellingPerValue) {
  const std::vector<std::pair<std::string_view, std::string_view>> cases = {
      {"17.00", "17"},    {"17", "17"},
      {"-0.25", "-0.25"}, {"-.25", "-0.25"},
      {".500", "0.5"},    {"0.500", "0.5"},
      {"007.10", "7.1"},  {"0", "0"},
      {"0.00", "0"},      {"-0.00", "0"},
      {"-0", "0"},        {"1.", "1"},
      {"-120", "-120"},   {"98765432109876543210.0123", "98765432109876543210.0123"},
  };
  for (const auto& [text, normal] : cases) {
    EXPECT_EQ(NormalizeDecimal(text), std::optional<std::string>(normal)) << text;
  }
  for (const std::string_view other :
       {"", "-", ".", "-.", "+1", "--1", "1e5", " 1", "1 ", "1.2.3", "1,5", "NULL", "0x1"}) {
    EXPECT_EQ(NormalizeDecimal(other), std::nullopt) << other;
  }
}

// The oracle's classes decide: I and T exactly, D by value, R within the tolerance; NULL is an
// empty field, and an empty string on the oracle's side counts as NULL.
TEST(CompareWithAnswer, CellsCompareByTheOraclesColumnClass) {
  const ResultSet oracle{.classes = {ColumnClass::kInteger, ColumnClass::kDecimal,
                                     ColumnClass::kReal, ColumnClass::kText},
                         .type_names = {"BIGINT", "DECIMAL(15,2)", "DOUBLE", "VARCHAR"},
                         .rows = {{"12", "17.00", "0.30000000000000004", "a b"},
                                  {"-3", "-0.25", "1e+20", "2001-02-03"},
                                  {std::nullopt, std::nullopt, std::nullopt, ""}}};
  const std::string header = "i|d|r|t\n";
  const std::string rest = "-3|-.25|100000000000000000000|2001-02-03\n|||\n";
  const auto right =
      CompareWithAnswer(oracle, Answer(std::format("{}12|17|0.3|a b\n{}", header, rest)));
  ASSERT_TRUE(right.has_value()) << right.error().what;
  EXPECT_FALSE(right->reordered);
  for (const std::string_view first_row : {
           "012|17|0.3|a b\n",    // I is exact
           "12|17.01|0.3|a b\n",  // D by value
           "12|17|0.3001|a b\n",  // R within 1e-9 only
           "12|17|0.3|a  b\n",    // T is exact
           "12|17|0.3|a b \n",    // and never trimmed
           "12|17|0.3|\n",        // NULL is not a value
       }) {
    const auto wrong =
        CompareWithAnswer(oracle, Answer(std::format("{}{}{}", header, first_row, rest)));
    ASSERT_FALSE(wrong.has_value()) << first_row;
    EXPECT_EQ(wrong.error().what, "result mismatch: values differ") << first_row;
    EXPECT_EQ(wrong.error().first_row, 0U) << first_row;
  }
}

TEST(CompareWithAnswer, ColumnCountsRowCountsAndRowOrder) {
  const ResultSet oracle = Ints({"1", "2", "2"});
  const auto columns = CompareWithAnswer(oracle, Answer("a|b\n1|1\n2|2\n2|2\n"));
  ASSERT_FALSE(columns.has_value());
  EXPECT_EQ(columns.error().what, "column counts differ: answer 2, DuckDB 1");
  const auto rows = CompareWithAnswer(oracle, Answer("a\n1\n2\n"));
  ASSERT_FALSE(rows.has_value());
  EXPECT_EQ(rows.error().what, "result mismatch: expected 2 rows, got 3");
  const auto reordered = CompareWithAnswer(oracle, Answer("a\n2\n1\n2\n"));
  ASSERT_TRUE(reordered.has_value()) << reordered.error().what;
  EXPECT_TRUE(reordered->reordered);
  const auto multiset = CompareWithAnswer(oracle, Answer("a\n1\n1\n2\n"));
  ASSERT_FALSE(multiset.has_value());
  EXPECT_EQ(multiset.error().first_row, 1U);  // the first row that differs in order
}

TEST(RunAnswers, PassesAndCountsRowsInAnotherOrder) {
  ScriptedEngine oracle([](const std::string& sql) {
    return ExecResult(sql == "SELECT 1" ? Ints({"1", "2"}) : Ints({"7"}));
  });
  const auto queries = Queries({{"SELECT 1", "x\n2\n1\n"}, {"SELECT 7", "y\n7\n"}});
  std::string out;
  const AnswersStats stats =
      RunAnswers(queries, oracle, {.redact = true, .only = std::nullopt, .command = "c"}, out);
  EXPECT_EQ(stats.queries, 2);
  EXPECT_EQ(stats.passed, 2);
  EXPECT_EQ(stats.reordered, 1);
  EXPECT_EQ(stats.failed, 0);
  EXPECT_EQ(out,
            "Q1: pass (rows in another order)\nQ2: pass\n"
            "ANSWERS: PASS queries=2 passed=2 reordered=1 failed=0\n");
}

// A redacted report prints no SQL, no value of either side and no error message: only query
// numbers, error kinds, counts, row indexes and hashes.
TEST(RunAnswers, RedactedFailuresPrintNoSqlValuesOrMessages) {
  ScriptedEngine oracle([](const std::string& sql) -> ExecResult {
    if (sql.contains("antb1_canary_error")) {
      return std::unexpected(
          EngineError{.kind = "Binder Error", .message = "Binder Error: antb1_canary_message"});
    }
    if (sql.contains("antb1_canary_text")) {
      return ResultSet{.classes = {ColumnClass::kText},
                       .type_names = {"VARCHAR"},
                       .rows = {{"ANTB1_CANARY_ACTUAL"}}};
    }
    return Ints({"1"});
  });
  const auto queries = Queries({
      {"SELECT 'antb1_canary_text'", "t\nANTB1_CANARY_ANSWER\n"},
      {"SELECT antb1_canary_error", "t\n1\n"},
      {"SELECT 1 AS antb1_canary_malformed", "a|b\nantb1_canary_cell\n"},
      {"SELECT 1 AS antb1_canary_columns", "a|b\n1|2\n"},
  });
  std::string out;
  const AnswersStats stats = RunAnswers(
      queries, oracle, {.redact = true, .only = std::nullopt, .command = "antb1-slt answers"}, out);
  EXPECT_EQ(stats.failed, 4);
  for (const std::string_view line : {
           "FAIL Q1: result mismatch: values differ\n"
           "  rows: answer 1, DuckDB 1; first differing row: 0\n",
           "  repro (unredacted, prints values and query text; run it locally):\n"
           "    antb1-slt answers --only 1\n",
           "FAIL Q2: DuckDB fails on the query\n  error kind: Binder Error\n",
           "FAIL Q3: cannot read the answer: line 2: 1 field(s), the header has 2\n",
           "FAIL Q4: column counts differ: answer 2, DuckDB 1\n",
           "ANSWERS: FAIL queries=4 passed=0 reordered=0 failed=4\n",
       }) {
    EXPECT_NE(out.find(line), std::string::npos) << line << " in\n" << out;
  }
  EXPECT_NE(
      out.find(std::format("  sha256: answer {}\n          DuckDB {}\n",
                           Sha256Hex("ANTB1_CANARY_ANSWER\n"), Sha256Hex("ANTB1_CANARY_ACTUAL\n"))),
      std::string::npos)
      << out;
  EXPECT_FALSE(Lower(out).contains("canary")) << out;
  EXPECT_FALSE(Lower(out).contains("select")) << out;
}

TEST(RunAnswers, UnredactedFailuresShowTheSqlAndTheRows) {
  ScriptedEngine oracle([](const std::string&) { return ExecResult(Ints({"13"})); });
  std::string out;
  RunAnswers(Queries({{"SELECT 13", "n\n12\n"}}), oracle,
             {.redact = false, .only = std::nullopt, .command = "antb1-slt answers"}, out);
  EXPECT_EQ(out,
            "FAIL Q1: result mismatch: values differ\n"
            "  SQL:\n"
            "    SELECT 13\n"
            "  answer: 1 row(s), DuckDB: 1 row(s); differing rows (at most 5):\n"
            "    row 0: answer 12\n"
            "           DuckDB 13\n"
            "  repro:\n"
            "    antb1-slt answers --only 1\n"
            "ANSWERS: FAIL queries=1 passed=0 reordered=0 failed=1\n");
}

TEST(RunAnswers, OnlyRunsOneQuery) {
  ScriptedEngine oracle([](const std::string&) { return ExecResult(Ints({"7"})); });
  const auto queries = Queries({{"SELECT 1", "x\n1\n"}, {"SELECT 7", "y\n7\n"}});
  std::string out;
  EXPECT_EQ(RunAnswers(queries, oracle, {.redact = true, .only = 2, .command = "c"}, out).failed,
            0);
  EXPECT_EQ(out, "Q2: pass\nANSWERS: PASS queries=1 passed=1 reordered=0 failed=0\n");
  out.clear();
  EXPECT_EQ(RunAnswers(queries, oracle, {.redact = true, .only = 9, .command = "c"}, out).failed,
            1);
  EXPECT_EQ(out,
            "FAIL Q9: no such query (there are 2)\n"
            "ANSWERS: FAIL queries=0 passed=0 reordered=0 failed=1\n");
}

// The other subcommands keep their reports byte for byte: DuckDB is the expected side, antb1 the
// actual one.
TEST(AppendDiscrepancy, DefaultSideLabels) {
  const Discrepancy d{.what = "result mismatch: values differ",
                      .mismatch = true,
                      .types = "I",
                      .expected = {"12"},
                      .actual = {"13"},
                      .first_row = 0};
  std::string out;
  AppendDiscrepancy(d, "SELECT 13", SortMode::kNoSort, /*redact=*/true, out);
  EXPECT_EQ(out, std::format("  rows: DuckDB 1, antb1 1; first differing row: 0\n"
                             "  sha256: DuckDB {}\n"
                             "          antb1  {}\n",
                             Sha256Hex("12\n"), Sha256Hex("13\n")));
  out.clear();
  AppendDiscrepancy(d, "SELECT 13", SortMode::kNoSort, /*redact=*/false, out);
  EXPECT_EQ(out,
            "  SQL:\n"
            "    SELECT 13\n"
            "  DuckDB: 1 row(s), antb1: 1 row(s); differing rows (at most 5):\n"
            "    row 0: DuckDB 12\n"
            "           antb1  13\n");
}

class AnswerFiles : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::path(::testing::TempDir()) / "antb1_answers" /
           ::testing::UnitTest::GetInstance()->current_test_info()->name();
    std::error_code ec;
    fs::remove_all(dir_, ec);
    fs::create_directories(dir_ / "queries");
    fs::create_directories(dir_ / "answers");
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  void Write(const std::string& relative, std::string_view text) const {
    std::ofstream out(dir_ / relative, std::ios::binary);
    out << text;
  }

  // Writes q<nn>.sql and q<nn>.csv for every number.
  void WritePairs(std::initializer_list<int> numbers) const {
    for (const int n : numbers) {
      Write(std::format("queries/q{:02}.sql", n), std::format("SELECT {};\n", n));
      Write(std::format("answers/q{:02}.csv", n), std::format("n\n{}\n", n));
    }
  }

  [[nodiscard]] std::expected<std::vector<AnswerQuery>, std::string> Load(
      bool redact = true) const {
    return LoadAnswerQueries(dir_ / "queries", dir_ / "answers", redact);
  }

  // The error of Load(), or "" if it succeeds.
  [[nodiscard]] std::string LoadError(bool redact = true) const {
    const auto loaded = Load(redact);
    return loaded.has_value() ? std::string() : loaded.error();
  }

  fs::path dir_;
};

TEST_F(AnswerFiles, NumberedQueriesWithTheirAnswers) {
  Write("queries/q01.sql", "-- the first\nSELECT 1\nFROM t;\n");
  Write("answers/q01.csv", "a\n1\n");
  WritePairs({2});
  Write("queries/notes.txt", "other files are ignored");
  Write("answers/q02.txt", "other files are ignored");
  const auto queries = Load();
  ASSERT_TRUE(queries.has_value()) << queries.error();
  ASSERT_EQ(queries->size(), 2U);
  EXPECT_EQ((*queries)[0].number, 1);
  EXPECT_EQ((*queries)[0].statement.sql, "SELECT 1\nFROM t");
  EXPECT_EQ((*queries)[0].answer, "a\n1\n");
  EXPECT_EQ((*queries)[1].number, 2);
  EXPECT_EQ((*queries)[1].query_path, (dir_ / "queries" / "q02.sql").string());
  EXPECT_EQ((*queries)[1].answer_path, (dir_ / "answers" / "q02.csv").string());
}

TEST_F(AnswerFiles, NoQueries) {
  EXPECT_TRUE(LoadError().starts_with("no queries (q01.sql, ...) in ")) << LoadError();
  EXPECT_TRUE(
      LoadAnswerQueries(dir_ / "none", dir_ / "answers", true).error().starts_with("cannot list "));
}

TEST_F(AnswerFiles, NumberedFromOneWithoutGaps) {
  WritePairs({1, 3});
  EXPECT_EQ(LoadError(),
            std::format("'{}' is missing: the queries are numbered from 1 without gaps",
                        (dir_ / "queries" / "q02.sql").string()));
}

TEST_F(AnswerFiles, NamesHaveTwoDigitsAtLeast) {
  WritePairs({1});
  Write("queries/q2.sql", "SELECT 2;\n");
  EXPECT_TRUE(
      LoadError().contains("q2.sql': unexpected file name (the files are q01.sql, "
                           "q02.sql, ...)"))
      << LoadError();
}

TEST_F(AnswerFiles, EveryQueryHasAnAnswerAndEveryAnswerAQuery) {
  WritePairs({1, 2});
  fs::remove(dir_ / "answers" / "q02.csv");
  EXPECT_TRUE(LoadError().starts_with("no answer ")) << LoadError();
  WritePairs({2});
  Write("answers/q03.csv", "n\n3\n");
  EXPECT_TRUE(LoadError().starts_with("the answer ")) << LoadError();
  EXPECT_TRUE(LoadError().ends_with("q03.sql'")) << LoadError();
}

TEST_F(AnswerFiles, OneStatementPerQuery) {
  WritePairs({1});
  Write("queries/q01.sql", "SELECT 1;\nSELECT 2;\n");
  EXPECT_TRUE(LoadError().ends_with("q01.sql': 2 statements, expected one")) << LoadError();
}

// A parse error can quote the file: only unredacted.
TEST_F(AnswerFiles, ParseErrorsQuoteTheFileOnlyUnredacted) {
  WritePairs({1});
  Write("queries/q01.sql", "-- features: antb1_canary_feature\nSELECT 1;\n");
  const std::string redacted = LoadError(/*redact=*/true);
  EXPECT_TRUE(redacted.contains("q01.sql': not a query file")) << redacted;
  EXPECT_FALSE(Lower(redacted).contains("canary")) << redacted;
  EXPECT_TRUE(LoadError(/*redact=*/false).contains("antb1_canary_feature"));
}

}  // namespace
}  // namespace antb1::slt
