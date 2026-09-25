#include "result_diff.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "canonical.h"
#include "engine.h"
#include "sha256.h"

namespace antb1::slt {
namespace {

constexpr std::size_t kMaxDiffRows = 5;

std::string Letters(const ResultSet& r) {
  std::string letters;
  for (const auto c : r.classes) {
    letters += ClassLetter(c);
  }
  return letters;
}

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
        .what = std::format("row counts differ (LIMIT on a projection: any {} rows are right)",
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
