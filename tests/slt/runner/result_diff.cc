#include "result_diff.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "canonical.h"
#include "engine.h"
#include "sha256.h"

namespace antb1::slt {
namespace {

constexpr std::size_t kMaxDiffRows = 5;

std::string Join(const std::vector<std::string>& lines) {
  std::string text;
  for (const auto& line : lines) {
    text += line;
    text += '\n';
  }
  return text;
}

bool SameLine(const std::string& e, const std::string& a, std::string_view types, SortMode sort) {
  return !CompareBlocks({e}, {a}, types, sort, kDefaultRelTolerance).has_value();
}

void AppendDifferingRows(const Discrepancy& d, SortMode sort, std::string& out) {
  out += std::format("  DuckDB: {} row(s), antb1: {} row(s); differing rows (at most {}):\n",
                     d.expected.size(), d.actual.size(), kMaxDiffRows);
  std::size_t shown = 0;
  const std::size_t n = std::max(d.expected.size(), d.actual.size());
  for (std::size_t i = 0; i < n && shown < kMaxDiffRows; ++i) {
    const bool has_e = i < d.expected.size();
    const bool has_a = i < d.actual.size();
    if (has_e && has_a && SameLine(d.expected[i], d.actual[i], d.types, sort)) {
      continue;
    }
    const std::string label = std::format("row {}:", i);
    out += std::format("    {} DuckDB {}\n", label, has_e ? d.expected[i] : "(no row)");
    out += std::format("    {:{}} antb1  {}\n", "", label.size(), has_a ? d.actual[i] : "(no row)");
    ++shown;
  }
}

}  // namespace

std::string Letters(const ResultSet& result) {
  std::string letters;
  for (const auto c : result.classes) {
    letters += ClassLetter(c);
  }
  return letters;
}

std::string ExactCells(std::string_view line, std::string_view types) {
  std::string out;
  std::size_t column = 0;
  std::size_t start = 0;
  while (start <= line.size()) {
    const std::size_t end = std::min(line.find('\t', start), line.size());
    if (column >= types.size() || types[column] != 'R') {
      out.append(line.substr(start, end - start));
      out += '\t';
    }
    ++column;
    start = end + 1;
  }
  return out;
}

std::string ColumnTypes(const ResultSet& result) {
  std::string names;
  for (const auto& n : result.type_names) {
    names += (names.empty() ? "" : ", ") + n;
  }
  return std::format("{} ({})", Letters(result), names);
}

std::optional<Discrepancy> CompareAnswers(const ResultSet& oracle, const ResultSet& antb1,
                                          SortMode sort, bool row_count_only) {
  const std::string letters = Letters(oracle);
  if (letters != Letters(antb1) || oracle.type_names != antb1.type_names) {
    return Discrepancy{.what = std::format("column types differ: DuckDB {}, antb1 {}",
                                           ColumnTypes(oracle), ColumnTypes(antb1))};
  }
  auto expected = RenderBlock(oracle, sort, 0);
  auto actual = RenderBlock(antb1, sort, 0);
  if (row_count_only) {
    if (expected.size() == actual.size()) {
      return std::nullopt;
    }
    return Discrepancy{
        .what = std::format("row counts differ (LIMIT without ORDER BY: any {} rows are right)",
                            expected.size()),
        .mismatch = true,
        .types = letters,
        .expected = std::move(expected),
        .actual = std::move(actual),
        .first_row = std::nullopt};
  }
  if (auto diff = CompareBlocks(expected, actual, letters, sort, kDefaultRelTolerance)) {
    return Discrepancy{.what = "result mismatch: " + diff->reason,
                       .mismatch = true,
                       .types = letters,
                       .expected = std::move(expected),
                       .actual = std::move(actual),
                       .first_row = diff->first_row};
  }
  return std::nullopt;
}

std::optional<Discrepancy> CompareSubset(const ResultSet& oracle, const ResultSet& unlimited,
                                         const ResultSet& antb1) {
  if (auto types = CompareAnswers(oracle, antb1, SortMode::kRowSort, /*row_count_only=*/true)) {
    return types;  // different column types or row counts
  }
  const std::string letters = Letters(oracle);
  auto pool = RenderBlock(unlimited, SortMode::kRowSort, 0);
  auto actual = RenderBlock(antb1, SortMode::kRowSort, 0);
  std::vector<bool> used(pool.size(), false);
  // Rows by the text of their exact (I, D and T) cells, for the matches within the R tolerance.
  std::unordered_map<std::string, std::vector<std::size_t>> by_exact_cells;
  if (letters.contains('R')) {
    for (std::size_t j = 0; j < pool.size(); ++j) {
      by_exact_cells[ExactCells(pool[j], letters)].push_back(j);
    }
  }
  for (std::size_t i = 0; i < actual.size(); ++i) {
    // An exact match first (rows are sorted), then a match within the R tolerance.
    const auto exact = std::ranges::equal_range(pool, actual[i]);
    std::optional<std::size_t> match;
    for (auto it = exact.begin(); it != exact.end() && !match; ++it) {
      const auto at = static_cast<std::size_t>(it - pool.begin());
      match = used[at] ? std::nullopt : std::optional(at);
    }
    if (!match && letters.contains('R')) {
      const auto candidates = by_exact_cells.find(ExactCells(actual[i], letters));
      for (std::size_t k = 0;
           candidates != by_exact_cells.end() && k < candidates->second.size() && !match; ++k) {
        const std::size_t j = candidates->second[k];
        match = !used[j] && SameLine(pool[j], actual[i], letters, SortMode::kRowSort)
                    ? std::optional(j)
                    : std::nullopt;
      }
    }
    if (!match) {
      return Discrepancy{
          .what =
              "antb1 returns a row that the unlimited answer does not have (LIMIT without "
              "ORDER BY: any rows of it are right)",
          .mismatch = true,
          .types = letters,
          .expected = std::move(pool),
          .actual = std::move(actual),
          .first_row = i};
    }
    used[*match] = true;
  }
  return std::nullopt;
}

std::optional<Discrepancy> CompareLimited(const ResultSet& oracle, const ResultSet& antb1,
                                          const std::function<ExecResult()>& unlimited) {
  if (!CompareAnswers(oracle, antb1, SortMode::kRowSort, /*row_count_only=*/false).has_value()) {
    return std::nullopt;
  }
  const ExecResult all = unlimited();
  if (!all.has_value()) {
    return ErrorDiscrepancy("DuckDB fails on the query without its LIMIT", all.error());
  }
  return CompareSubset(oracle, *all, antb1);
}

Discrepancy ErrorDiscrepancy(std::string what, const EngineError& error) {
  return Discrepancy{.what = std::move(what),
                     .detail = error.message,
                     .redacted = std::format("error kind: {}", error.kind)};
}

void AppendDiscrepancy(const Discrepancy& d, std::string_view sql, SortMode sort, bool redact,
                       std::string& out) {
  if (redact) {
    if (!d.redacted.empty()) {
      out += "  " + d.redacted + "\n";
    }
    if (d.mismatch) {
      out += std::format("  rows: DuckDB {}, antb1 {}", d.expected.size(), d.actual.size());
      if (d.first_row.has_value()) {
        out += std::format("; first differing row: {}", *d.first_row);
      }
      out += std::format("\n  sha256: DuckDB {}\n          antb1  {}\n",
                         Sha256Hex(Join(d.expected)), Sha256Hex(Join(d.actual)));
    }
    return;
  }
  if (!d.detail.empty()) {
    out += "  " + d.detail + "\n";
  }
  out += "  SQL:\n";
  std::size_t pos = 0;
  while (pos <= sql.size()) {
    const std::size_t end = std::min(sql.find('\n', pos), sql.size());
    out += "    ";
    out += sql.substr(pos, end - pos);
    out += '\n';
    pos = end + 1;
  }
  if (d.mismatch) {
    AppendDifferingRows(d, sort, out);
  }
}

}  // namespace antb1::slt
