#include "slt_file.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "canonical.h"

namespace antb1::slt {
namespace {

bool IsBlank(std::string_view line) {
  return line.find_first_not_of(" \t") == std::string_view::npos;
}

std::vector<std::string_view> SplitWords(std::string_view line) {
  std::vector<std::string_view> words;
  std::size_t pos = 0;
  while (true) {
    const std::size_t begin = line.find_first_not_of(" \t", pos);
    if (begin == std::string_view::npos) {
      return words;
    }
    const std::size_t end = line.find_first_of(" \t", begin);
    words.push_back(
        line.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin));
    if (end == std::string_view::npos) {
      return words;
    }
    pos = end;
  }
}

std::string_view Trim(std::string_view s) {
  const std::size_t begin = s.find_first_not_of(" \t");
  if (begin == std::string_view::npos) {
    return {};
  }
  return s.substr(begin, s.find_last_not_of(" \t") - begin + 1);
}

std::vector<std::string> SplitLines(std::string_view text) {
  std::vector<std::string> lines;
  std::size_t pos = 0;
  while (pos < text.size()) {
    std::size_t end = text.find('\n', pos);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    std::string_view line = text.substr(pos, end - pos);
    if (line.ends_with('\r')) {
      line.remove_suffix(1);
    }
    lines.emplace_back(line);
    pos = end + 1;
  }
  return lines;
}

std::optional<SortMode> ParseSortMode(std::string_view word) {
  if (word == "nosort") {
    return SortMode::kNoSort;
  }
  if (word == "rowsort") {
    return SortMode::kRowSort;
  }
  if (word == "valuesort") {
    return SortMode::kValueSort;
  }
  return std::nullopt;
}

template <class T>
std::optional<T> ParseNumber(std::string_view text) {
  T value{};
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc{} || ptr != end) {
    return std::nullopt;
  }
  return value;
}

class Parser {
 public:
  Parser(const std::string& path, std::string_view text) {
    file_.path = path;
    file_.lines = SplitLines(text);
  }

  std::expected<SltFile, std::string> Parse() {
    const auto& lines = file_.lines;
    std::size_t i = 0;
    while (i < lines.size()) {
      const std::string_view line = lines[i];
      if (IsBlank(line)) {
        if (!pending_.skipif.empty() || !pending_.onlyif.empty()) {
          return Error(i, "skipif/onlyif must be followed directly by a record");
        }
        ++i;
        continue;
      }
      if (line.starts_with('#')) {
        if (auto status = Comment(i, line); !status) {
          return std::unexpected(status.error());
        }
        ++i;
        continue;
      }
      auto next = Directive(i);
      if (!next) {
        return std::unexpected(next.error());
      }
      i = *next;
    }
    if (!pending_.skipif.empty() || !pending_.onlyif.empty() || pending_.tolerance.has_value()) {
      return Error(lines.size() - 1,
                   "condition or '# tol' without a record at the end of the file");
    }
    return std::move(file_);
  }

 private:
  std::unexpected<std::string> Error(std::size_t index, std::string_view message) const {
    return std::unexpected(std::format("{}:{}: {}", file_.path, index + 1, message));
  }

  std::expected<void, std::string> Comment(std::size_t i, std::string_view line) {
    const auto words = SplitWords(line.substr(1));
    if (words.empty() || words[0] != "tol") {
      return {};
    }
    const auto tol = words.size() == 2 ? ParseNumber<double>(words[1]) : std::nullopt;
    if (!tol.has_value() || !std::isfinite(*tol) || *tol <= 0 || *tol >= 1) {
      return Error(i, "expected '# tol <relative tolerance>' with 0 < tolerance < 1");
    }
    pending_.tolerance = tol;
    return {};
  }

  // Parses the directive at line i; returns the index of the line after the record.
  std::expected<std::size_t, std::string> Directive(std::size_t i) {
    const auto& lines = file_.lines;
    const std::string_view line = lines[i];
    const auto words = SplitWords(line);
    const std::string_view word = words[0];
    if (word == "skipif" || word == "onlyif") {
      if (words.size() != 2 || !std::ranges::contains(kEngineNames, words[1])) {
        return Error(i, std::format("expected '{} antb1' or '{} duckdb'", word, word));
      }
      (word == "skipif" ? pending_.skipif : pending_.onlyif).emplace_back(words[1]);
      return i + 1;
    }
    Record record = std::move(pending_);
    pending_ = Record{};
    record.line = static_cast<int>(i) + 1;
    record.header = i;
    if (record.tolerance.has_value() && word != "query") {
      return Error(i, "'# tol' must precede a query record");
    }
    std::size_t next = i + 1;
    if (word == "halt" && words.size() == 1) {
      record.kind = RecordKind::kHalt;
    } else if (word == "hash-threshold") {
      const auto n = words.size() == 2 ? ParseNumber<int64_t>(words[1]) : std::nullopt;
      if (!n.has_value() || *n < 0) {
        return Error(i, "expected 'hash-threshold <n>' with n >= 0");
      }
      record.kind = RecordKind::kHashThreshold;
      record.hash_threshold = *n;
    } else if (word == "statement" && words.size() >= 2 &&
               (words[1] == "ok" || words[1] == "error")) {
      if (words[1] == "ok" && words.size() != 2) {
        return Error(i, "'statement ok' takes no arguments");
      }
      record.kind = words[1] == "ok" ? RecordKind::kStatementOk : RecordKind::kStatementError;
      if (record.kind == RecordKind::kStatementError) {
        const std::size_t after = line.find("error") + std::string_view("error").size();
        record.error_regex = std::string(Trim(line.substr(after)));
        try {
          record.error_pattern = std::regex(record.error_regex);
        } catch (const std::regex_error& e) {
          return Error(i, std::format("invalid error regex: {}", e.what()));
        }
      }
      next = ReadSql(i + 1, record);
      if (next < lines.size() && lines[next] == "----") {
        return Error(next, "statement records have no result block");
      }
    } else if (word == "query") {
      if (auto status = QueryHeader(i, words, record); !status) {
        return std::unexpected(status.error());
      }
      next = ReadSql(i + 1, record);
      record.sql_end = next;
      record.block_end = next;
      if (next < lines.size() && lines[next] == "----") {
        record.has_separator = true;
        ++next;
        while (next < lines.size() && !IsBlank(lines[next])) {
          record.expected.push_back(lines[next++]);
        }
        record.block_end = next;
      }
    } else {
      return Error(i, std::format("unknown or malformed directive '{}'", line));
    }
    if ((record.kind == RecordKind::kStatementOk || record.kind == RecordKind::kStatementError ||
         record.kind == RecordKind::kQuery) &&
        Trim(record.sql).empty()) {
      return Error(i, "record without SQL");
    }
    file_.records.push_back(std::move(record));
    return next;
  }

  std::expected<void, std::string> QueryHeader(std::size_t i,
                                               const std::vector<std::string_view>& words,
                                               Record& record) const {
    if (words.size() < 2 || words.size() > 4 ||
        !std::ranges::all_of(words[1], [](char c) { return c == 'I' || c == 'R' || c == 'T'; })) {
      return Error(i, "expected 'query <I|R|T...> [nosort|rowsort|valuesort] [label]'");
    }
    record.kind = RecordKind::kQuery;
    record.types = std::string(words[1]);
    if (words.size() >= 3) {
      const auto sort = ParseSortMode(words[2]);
      if (!sort.has_value()) {
        return Error(
            i, std::format("unknown sort mode '{}' (nosort, rowsort or valuesort)", words[2]));
      }
      record.sort = *sort;
    }
    if (words.size() == 4) {
      record.label = std::string(words[3]);
    }
    return {};
  }

  // Reads SQL lines from `from` up to a blank line or "----"; returns the index after the SQL.
  std::size_t ReadSql(std::size_t from, Record& record) const {
    const auto& lines = file_.lines;
    std::size_t j = from;
    while (j < lines.size() && !IsBlank(lines[j]) && lines[j] != "----") {
      if (!record.sql.empty()) {
        record.sql += '\n';
      }
      record.sql += lines[j++];
    }
    record.sql_end = j;
    record.block_end = j;
    return j;
  }

  SltFile file_;
  Record pending_;
};

}  // namespace

bool Record::RunsOn(std::string_view engine) const {
  if (!onlyif.empty() && !std::ranges::contains(onlyif, engine)) {
    return false;
  }
  return !std::ranges::contains(skipif, engine);
}

std::expected<SltFile, std::string> ParseSlt(const std::string& path, std::string_view text) {
  return Parser(path, text).Parse();
}

std::string RewriteSlt(const SltFile& file, const std::map<std::size_t, BlockUpdate>& updates) {
  std::vector<std::string> out;
  std::size_t pos = 0;
  for (const auto& [index, update] : updates) {
    const Record& r = file.records[index];
    out.insert(out.end(), file.lines.begin() + static_cast<std::ptrdiff_t>(pos),
               file.lines.begin() + static_cast<std::ptrdiff_t>(r.header));
    std::string header = file.lines[r.header];
    if (update.new_types.has_value()) {
      const auto words = SplitWords(file.lines[r.header]);  // views into the file, not into header
      header = std::format("query {}", *update.new_types);
      for (std::size_t w = 2; w < words.size(); ++w) {
        header += ' ';
        header += words[w];
      }
    }
    out.push_back(std::move(header));
    out.insert(out.end(), file.lines.begin() + static_cast<std::ptrdiff_t>(r.header) + 1,
               file.lines.begin() + static_cast<std::ptrdiff_t>(r.sql_end));
    out.emplace_back("----");
    out.insert(out.end(), update.lines.begin(), update.lines.end());
    pos = r.block_end;
  }
  out.insert(out.end(), file.lines.begin() + static_cast<std::ptrdiff_t>(pos), file.lines.end());
  std::string text;
  for (const auto& line : out) {
    text += line;
    text += '\n';
  }
  return text;
}

}  // namespace antb1::slt
