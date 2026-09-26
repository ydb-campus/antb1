#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine.h"

// The one canonical text form of result values, shared by both engines, and the sqllogictest
// rendering and comparison built on it:
//   value text  integers (up to 128 bits) exactly; doubles as the shortest round-trip form ("nan",
//               "inf", "-inf"); dates as plan::FormatDate prints them (YYYY-MM-DD, DuckDB's form
//               outside years 1 to 9999); strings as their bytes. antb1 produces it with
//               engine::FormatValue; the DuckDB adapter converts its values with the helpers below.
//   slt cell    NULL; (empty) for ""; \t \n \r \\ escaped; control characters, invalid UTF-8 bytes
//               and leading/trailing spaces as \xHH; a string that reads NULL or (empty) gets its
//               first byte escaped, so every cell is unambiguous.
//   block       one row per line with cells separated by a tab; rowsort sorts the rows, valuesort
//               sorts the single values (one per line); results with more than hash-threshold
//               values (and no R column) become "<n> values hashing to <sha256>".

namespace antb1::slt {

std::string CanonicalDouble(double value);
std::string CanonicalDate(int32_t days_since_epoch);

// Escapes one value into an slt cell (see above).
std::string SltCell(const std::optional<std::string>& value);

enum class SortMode : std::uint8_t { kNoSort, kRowSort, kValueSort };

// Renders a result as the lines of an expected-result block.
std::vector<std::string> RenderBlock(const ResultSet& result, SortMode sort,
                                     int64_t hash_threshold);

// Parses "<n> values hashing to <hex>"; std::nullopt for any other line.
std::optional<std::pair<int64_t, std::string>> ParseHashLine(std::string_view line);

struct BlockDiff {
  std::string reason;
  std::optional<std::size_t> first_row;  // first differing line of the block
};

// Compares an expected block with a rendered actual block. I and T cells compare exactly; R cells
// (types[i] == 'R') compare numerically: |a - b| <= 1e-12 + rel_tolerance * max(|a|, |b|).
std::optional<BlockDiff> CompareBlocks(const std::vector<std::string>& expected,
                                       const std::vector<std::string>& actual,
                                       std::string_view types, SortMode sort, double rel_tolerance);

inline constexpr double kDefaultRelTolerance = 1e-9;
inline constexpr double kAbsTolerance = 1e-12;

}  // namespace antb1::slt
