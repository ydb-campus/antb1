#include "canonical.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "antb1/plan/literal.h"

#include "engine.h"
#include "sha256.h"

namespace antb1::slt {
namespace {

// Length of the valid UTF-8 sequence starting at s[i], or 0 (RFC 3629: no overlongs, no
// surrogates).
std::size_t Utf8SequenceLength(std::string_view s, std::size_t i) {
  const auto byte = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
  const unsigned lead = byte(i);
  std::size_t len = 0;
  unsigned lo = 0x80;
  unsigned hi = 0xBF;
  if (lead >= 0xC2 && lead <= 0xDF) {
    len = 2;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    len = 3;
    lo = lead == 0xE0 ? 0xA0 : 0x80;
    hi = lead == 0xED ? 0x9F : 0xBF;
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    len = 4;
    lo = lead == 0xF0 ? 0x90 : 0x80;
    hi = lead == 0xF4 ? 0x8F : 0xBF;
  } else {
    return 0;
  }
  if (i + len > s.size() || byte(i + 1) < lo || byte(i + 1) > hi) {
    return 0;
  }
  for (std::size_t k = 2; k < len; ++k) {
    if (byte(i + k) < 0x80 || byte(i + k) > 0xBF) {
      return 0;
    }
  }
  return len;
}

std::string Hex(unsigned char c) { return std::format("\\x{:02x}", c); }

std::optional<double> ParseDouble(std::string_view text) {
  double value = 0;
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc{} || ptr != end) {
    return std::nullopt;
  }
  return value;
}

bool RealEqual(std::string_view a, std::string_view b, double rel_tolerance) {
  if (a == b) {
    return true;
  }
  const auto x = ParseDouble(a);
  const auto y = ParseDouble(b);
  if (!x.has_value() || !y.has_value() || !std::isfinite(*x) || !std::isfinite(*y)) {
    return false;
  }
  return std::fabs(*x - *y) <=
         kAbsTolerance + (rel_tolerance * std::max(std::fabs(*x), std::fabs(*y)));
}

std::vector<std::string_view> SplitCells(std::string_view line) {
  std::vector<std::string_view> cells;
  std::size_t start = 0;
  while (true) {
    const std::size_t tab = line.find('\t', start);
    cells.push_back(
        line.substr(start, tab == std::string_view::npos ? std::string_view::npos : tab - start));
    if (tab == std::string_view::npos) {
      return cells;
    }
    start = tab + 1;
  }
}

bool LinesEqual(std::string_view expected, std::string_view actual, std::string_view types,
                SortMode sort, double rel_tolerance) {
  if (expected == actual) {
    return true;
  }
  if (!types.contains('R')) {
    return false;
  }
  if (sort == SortMode::kValueSort) {  // one value per line; its column is unknown
    return RealEqual(expected, actual, rel_tolerance);
  }
  const auto e = SplitCells(expected);
  const auto a = SplitCells(actual);
  if (e.size() != a.size() || e.size() != types.size()) {
    return false;
  }
  for (std::size_t i = 0; i < e.size(); ++i) {
    const bool equal = types[i] == 'R' ? RealEqual(e[i], a[i], rel_tolerance) : e[i] == a[i];
    if (!equal) {
      return false;
    }
  }
  return true;
}

}  // namespace

char ClassLetter(ColumnClass c) {
  switch (c) {
    case ColumnClass::kInteger:
      return 'I';
    case ColumnClass::kReal:
      return 'R';
    case ColumnClass::kText:
      return 'T';
  }
  return '?';
}

std::string CanonicalDouble(double value) {
  if (std::isnan(value)) {
    return "nan";
  }
  if (std::isinf(value)) {
    return value > 0 ? "inf" : "-inf";
  }
  std::array<char, 64> buf{};
  const auto [end, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value);
  return ec == std::errc{} ? std::string(buf.data(), end) : std::format("{}", value);
}

std::string CanonicalDate(int32_t days_since_epoch) {
  return plan::FormatDate(days_since_epoch);  // what engine::FormatValue prints for antb1
}

std::string SltCell(const std::optional<std::string>& value) {
  if (!value.has_value()) {
    return "NULL";
  }
  const std::string_view s = *value;
  if (s.empty()) {
    return "(empty)";
  }
  std::string out;
  std::size_t i = 0;
  while (i < s.size()) {
    const auto c = static_cast<unsigned char>(s[i]);
    const bool edge_space = c == ' ' && (i == 0 || i + 1 == s.size());
    const std::size_t utf8 = c >= 0x80 ? Utf8SequenceLength(s, i) : 0;
    if (utf8 > 0) {
      out.append(s.substr(i, utf8));
      i += utf8;
      continue;
    }
    if (c == '\t') {
      out += "\\t";
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\\') {
      out += "\\\\";
    } else if (c < 0x20 || c >= 0x7F || edge_space) {  // control, DEL, invalid UTF-8, edge space
      out += Hex(c);
    } else {
      out += static_cast<char>(c);
    }
    ++i;
  }
  if (out == "NULL" || out == "(empty)") {
    return Hex(static_cast<unsigned char>(out[0])) + out.substr(1);
  }
  return out;
}

std::vector<std::string> RenderBlock(const ResultSet& result, SortMode sort,
                                     int64_t hash_threshold) {
  std::vector<std::vector<std::string>> rows;
  rows.reserve(result.rows.size());
  for (const auto& row : result.rows) {
    std::vector<std::string> cells;
    cells.reserve(row.size());
    for (const auto& value : row) {
      cells.push_back(SltCell(value));
    }
    rows.push_back(std::move(cells));
  }
  if (sort == SortMode::kRowSort) {
    std::ranges::sort(rows);
  }
  std::vector<std::string> values;
  for (const auto& row : rows) {
    values.insert(values.end(), row.begin(), row.end());
  }
  if (sort == SortMode::kValueSort) {
    std::ranges::sort(values);
  }
  const bool has_real = std::ranges::contains(result.classes, ColumnClass::kReal);
  if (hash_threshold > 0 && std::cmp_greater(values.size(), hash_threshold) && !has_real) {
    std::string joined;
    for (const auto& v : values) {
      joined += v;
      joined += '\n';
    }
    return {std::format("{} values hashing to {}", values.size(), Sha256Hex(joined))};
  }
  if (sort == SortMode::kValueSort) {
    return values;
  }
  std::vector<std::string> lines;
  lines.reserve(rows.size());
  for (const auto& row : rows) {
    std::string line;
    for (std::size_t c = 0; c < row.size(); ++c) {
      if (c > 0) {
        line += '\t';
      }
      line += row[c];
    }
    lines.push_back(std::move(line));
  }
  return lines;
}

std::optional<std::pair<int64_t, std::string>> ParseHashLine(std::string_view line) {
  constexpr std::string_view kMiddle = " values hashing to ";
  const std::size_t pos = line.find(kMiddle);
  if (pos == std::string_view::npos || pos == 0) {
    return std::nullopt;
  }
  int64_t count = 0;
  const char* begin = line.data();
  const char* end = begin + pos;
  const auto [ptr, ec] = std::from_chars(begin, end, count);
  const std::string_view hash = line.substr(pos + kMiddle.size());
  if (ec != std::errc{} || ptr != end || hash.size() != 64 ||
      !std::ranges::all_of(
          hash, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) {
    return std::nullopt;
  }
  return std::pair{count, std::string(hash)};
}

std::optional<BlockDiff> CompareBlocks(const std::vector<std::string>& expected,
                                       const std::vector<std::string>& actual,
                                       std::string_view types, SortMode sort,
                                       double rel_tolerance) {
  if (expected == actual) {
    return std::nullopt;
  }
  const bool hashed = (expected.size() == 1 && ParseHashLine(expected[0]).has_value()) ||
                      (actual.size() == 1 && ParseHashLine(actual[0]).has_value());
  if (hashed) {
    return BlockDiff{.reason = "hashed results differ", .first_row = std::nullopt};
  }
  const std::size_t common = std::min(expected.size(), actual.size());
  std::optional<std::size_t> first;
  for (std::size_t i = 0; i < common && !first.has_value(); ++i) {
    if (!LinesEqual(expected[i], actual[i], types, sort, rel_tolerance)) {
      first = i;
    }
  }
  const std::string_view unit = sort == SortMode::kValueSort ? "values" : "rows";
  if (expected.size() != actual.size()) {
    return BlockDiff{
        .reason = std::format("expected {} {}, got {}", expected.size(), unit, actual.size()),
        .first_row = first.value_or(common)};
  }
  if (!first.has_value()) {
    return std::nullopt;  // equal within the R tolerance
  }
  return BlockDiff{.reason = "values differ", .first_row = first};
}

}  // namespace antb1::slt
