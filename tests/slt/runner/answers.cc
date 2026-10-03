#include "answers.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "canonical.h"
#include "engine.h"
#include "query_file.h"
#include "result_diff.h"

namespace antb1::slt {
namespace {

namespace fs = std::filesystem;

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

std::string FileName(int number, std::string_view extension) {
  return std::format("q{:02}{}", number, extension);
}

// The numbers n of the files q<nn><extension> in `dir`, sorted. Other files are ignored, but a
// q<digits><extension> not spelled as FileName() spells it is an error.
std::expected<std::vector<int>, std::string> ListNumbered(const fs::path& dir,
                                                          std::string_view extension) {
  std::vector<int> numbers;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (!name.starts_with('q') || !name.ends_with(extension)) {
      continue;
    }
    const std::string_view digits =
        std::string_view(name).substr(1, name.size() - 1 - extension.size());
    if (digits.empty() || !std::ranges::all_of(digits, IsDigit)) {
      continue;
    }
    int n = 0;
    const auto [end_of_number, error] =
        std::from_chars(digits.data(), digits.data() + digits.size(), n);
    if (error != std::errc{} || n < 1 || FileName(n, extension) != name) {
      return std::unexpected(std::format("'{}': unexpected file name (the files are {}, {}, ...)",
                                         (dir / name).string(), FileName(1, extension),
                                         FileName(2, extension)));
    }
    numbers.push_back(n);
  }
  if (ec) {
    return std::unexpected(std::format("cannot list '{}': {}", dir.string(), ec.message()));
  }
  std::ranges::sort(numbers);
  return numbers;
}

std::expected<std::string, std::string> ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::unexpected(std::format("cannot read '{}'", path));
  }
  std::string text(std::istreambuf_iterator<char>(in), {});
  if (in.bad()) {
    return std::unexpected(std::format("cannot read '{}'", path));
  }
  return text;
}

// An error unless `numbers` (of the q<nn>.sql files of `dir`) are 1, 2, 3, ... without gaps.
std::expected<void, std::string> CheckNumbering(const fs::path& dir,
                                                const std::vector<int>& numbers) {
  if (numbers.empty()) {
    return std::unexpected(
        std::format("no queries ({}, ...) in '{}'", FileName(1, ".sql"), dir.string()));
  }
  int expected = 0;
  for (const int n : numbers) {
    if (n != ++expected) {
      return std::unexpected(
          std::format("'{}' is missing: the queries are numbered from 1 without gaps",
                      (dir / FileName(expected, ".sql")).string()));
    }
  }
  return {};
}

// The one statement of a query file. A parse error may quote the file (an unknown feature
// name): with `redact` the error names the file only.
std::expected<Statement, std::string> ReadQuery(const std::string& path, bool redact) {
  auto text = ReadFile(path);
  if (!text.has_value()) {
    return std::unexpected(text.error());
  }
  auto statements = ParseSqlFile(path, *text);
  if (!statements.has_value()) {
    return std::unexpected(redact ? std::format("'{}': not a query file (runner/query_file.h); "
                                                "run with --show-values locally to see why",
                                                path)
                                  : statements.error());
  }
  if (statements->size() != 1) {
    return std::unexpected(
        std::format("'{}': {} statements, expected one", path, statements->size()));
  }
  return std::move(statements->front());
}

// The fields of one line of answer text: '|' separated, an empty field is NULL.
std::vector<std::optional<std::string>> Fields(std::string_view line) {
  std::vector<std::optional<std::string>> fields;
  std::size_t pos = 0;
  while (true) {
    const std::size_t end = std::min(line.find('|', pos), line.size());
    const std::string_view field = line.substr(pos, end - pos);
    fields.push_back(field.empty() ? std::nullopt : std::optional<std::string>(field));
    if (end == line.size()) {
      return fields;
    }
    pos = end + 1;
  }
}

// The cells as CompareWithAnswer compares them: an empty string is NULL, a D value is spelled by
// NormalizeDecimal (text that is not a decimal stays as it is, and differs).
void Normalize(ResultSet& result) {
  for (auto& row : result.rows) {
    for (std::size_t c = 0; c < row.size() && c < result.classes.size(); ++c) {
      std::optional<std::string>& cell = row[c];
      if (cell.has_value() && cell->empty()) {
        cell.reset();
      } else if (cell.has_value() && result.classes[c] == ColumnClass::kDecimal) {
        if (auto normal = NormalizeDecimal(*cell)) {
          cell = *std::move(normal);
        }
      }
    }
  }
}

std::expected<AnswerMatch, Discrepancy> Check(const AnswerQuery& q, Engine& oracle) {
  const auto answer = ParseAnswerText(q.answer);
  if (!answer.has_value()) {
    return std::unexpected(
        Discrepancy{.what = std::format("cannot read the answer: {}", answer.error())});
  }
  const ExecResult result = oracle.Execute(q.statement.sql);
  if (!result.has_value()) {
    return std::unexpected(ErrorDiscrepancy("DuckDB fails on the query", result.error()));
  }
  return CompareWithAnswer(*result, *answer);
}

}  // namespace

std::expected<std::vector<AnswerQuery>, std::string> LoadAnswerQueries(const fs::path& queries_dir,
                                                                       const fs::path& answers_dir,
                                                                       bool redact) {
  const auto queries = ListNumbered(queries_dir, ".sql");
  if (!queries.has_value()) {
    return std::unexpected(queries.error());
  }
  const auto answers = ListNumbered(answers_dir, ".csv");
  if (!answers.has_value()) {
    return std::unexpected(answers.error());
  }
  if (auto numbered = CheckNumbering(queries_dir, *queries); !numbered.has_value()) {
    return std::unexpected(numbered.error());
  }
  for (const int n : *answers) {
    if (!std::ranges::binary_search(*queries, n)) {
      return std::unexpected(std::format("the answer '{}' has no query '{}'",
                                         (answers_dir / FileName(n, ".csv")).string(),
                                         (queries_dir / FileName(n, ".sql")).string()));
    }
  }
  std::vector<AnswerQuery> out;
  for (const int n : *queries) {
    AnswerQuery q{.number = n,
                  .query_path = (queries_dir / FileName(n, ".sql")).string(),
                  .answer_path = (answers_dir / FileName(n, ".csv")).string(),
                  .statement = {},
                  .answer = {}};
    if (!std::ranges::binary_search(*answers, n)) {
      return std::unexpected(
          std::format("no answer '{}' for the query '{}'", q.answer_path, q.query_path));
    }
    auto statement = ReadQuery(q.query_path, redact);
    if (!statement.has_value()) {
      return std::unexpected(statement.error());
    }
    q.statement = *std::move(statement);
    auto answer = ReadFile(q.answer_path);
    if (!answer.has_value()) {
      return std::unexpected(answer.error());
    }
    q.answer = *std::move(answer);
    out.push_back(std::move(q));
  }
  return out;
}

std::expected<std::vector<Statement>, std::string> LoadNumberedQueries(const fs::path& dir,
                                                                       bool redact) {
  const auto numbers = ListNumbered(dir, ".sql");
  if (!numbers.has_value()) {
    return std::unexpected(numbers.error());
  }
  if (auto numbered = CheckNumbering(dir, *numbers); !numbered.has_value()) {
    return std::unexpected(numbered.error());
  }
  std::vector<Statement> out;
  for (const int n : *numbers) {
    auto statement = ReadQuery((dir / FileName(n, ".sql")).string(), redact);
    if (!statement.has_value()) {
      return std::unexpected(statement.error());
    }
    out.push_back(*std::move(statement));
  }
  return out;
}

std::expected<AnswerTable, std::string> ParseAnswerText(std::string_view text) {
  if (text.empty()) {
    return std::unexpected("the text is empty (no header line)");
  }
  if (text.ends_with('\n')) {
    text.remove_suffix(1);  // only one: "x\n\n" is a header and one row with one NULL
  }
  AnswerTable table;
  std::size_t line_no = 1;
  std::size_t pos = 0;
  while (true) {
    const std::size_t end = std::min(text.find('\n', pos), text.size());
    auto fields = Fields(text.substr(pos, end - pos));
    if (line_no == 1) {
      table.columns = fields.size();
    } else if (fields.size() != table.columns) {
      return std::unexpected(std::format("line {}: {} field(s), the header has {}", line_no,
                                         fields.size(), table.columns));
    } else {
      table.rows.push_back(std::move(fields));
    }
    if (end == text.size()) {
      return table;
    }
    pos = end + 1;
    ++line_no;
  }
}

std::optional<std::string> NormalizeDecimal(std::string_view text) {
  const bool negative = text.starts_with('-');
  if (negative) {
    text.remove_prefix(1);
  }
  const std::size_t dot = text.find('.');
  std::string_view integer = text.substr(0, dot);
  std::string_view fraction =
      dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
  if ((integer.empty() && fraction.empty()) || !std::ranges::all_of(integer, IsDigit) ||
      !std::ranges::all_of(fraction, IsDigit)) {
    return std::nullopt;
  }
  integer.remove_prefix(std::min(integer.find_first_not_of('0'), integer.size()));
  const std::size_t last = fraction.find_last_not_of('0');
  fraction = last == std::string_view::npos ? std::string_view{} : fraction.substr(0, last + 1);
  std::string out = integer.empty() ? "0" : std::string(integer);
  if (!fraction.empty()) {
    out += '.';
    out += fraction;
  }
  if (negative && out != "0") {
    out.insert(0, 1, '-');
  }
  return out;
}

std::expected<AnswerMatch, Discrepancy> CompareWithAnswer(const ResultSet& oracle,
                                                          const AnswerTable& answer) {
  if (answer.columns != oracle.classes.size()) {
    return std::unexpected(
        Discrepancy{.what = std::format("column counts differ: answer {}, DuckDB {}",
                                        answer.columns, oracle.classes.size())});
  }
  ResultSet expected{
      .classes = oracle.classes, .type_names = oracle.type_names, .rows = answer.rows};
  ResultSet actual = oracle;
  Normalize(expected);
  Normalize(actual);
  auto in_order = CompareAnswers(expected, actual, SortMode::kNoSort, /*row_count_only=*/false);
  if (!in_order.has_value()) {
    return AnswerMatch{.reordered = false};
  }
  if (!CompareAnswers(expected, actual, SortMode::kRowSort, /*row_count_only=*/false).has_value()) {
    return AnswerMatch{.reordered = true};
  }
  return std::unexpected(*std::move(in_order));
}

AnswersStats RunAnswers(const std::vector<AnswerQuery>& queries, Engine& oracle,
                        const AnswersOptions& options, std::string& out) {
  AnswersStats stats;
  bool found = false;
  for (const AnswerQuery& q : queries) {
    if (options.only.has_value() && std::cmp_not_equal(*options.only, q.number)) {
      continue;
    }
    found = true;
    ++stats.queries;
    const auto match = Check(q, oracle);
    if (match.has_value()) {
      ++stats.passed;
      stats.reordered += match->reordered ? 1 : 0;
      out += std::format("Q{}: pass{}\n", q.number,
                         match->reordered ? " (rows in another order)" : "");
      continue;
    }
    ++stats.failed;
    out += std::format("FAIL Q{}: {}\n", q.number, match.error().what);
    AppendDiscrepancy(match.error(), q.statement.sql, SortMode::kNoSort, options.redact, out,
                      kAnswerLabels);
    out += options.redact ? "  repro (unredacted, prints values and query text; run it locally):\n"
                          : "  repro:\n";
    out += std::format("    {} --only {}\n", options.command, q.number);
  }
  if (options.only.has_value() && !found) {
    ++stats.failed;
    out += std::format("FAIL Q{}: no such query (there are {})\n", *options.only, queries.size());
  }
  out += std::format("ANSWERS: {} queries={} passed={} reordered={} failed={}\n",
                     stats.failed == 0 ? "PASS" : "FAIL", stats.queries, stats.passed,
                     stats.reordered, stats.failed);
  return stats;
}

}  // namespace antb1::slt
