#include "memory_size.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <unistd.h>

namespace antb1::cli {

std::optional<int64_t> ParseMemorySize(std::string_view text, std::optional<int64_t> physical) {
  while (!text.empty() && text.front() == ' ') {
    text.remove_prefix(1);
  }
  while (!text.empty() && text.back() == ' ') {
    text.remove_suffix(1);
  }
  double number = 0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), number);
  if (ec != std::errc() || end == text.data() || !(number >= 0)) {
    return std::nullopt;
  }
  std::string unit(end, text.data() + text.size());
  std::ranges::transform(unit, unit.begin(), [](char c) {
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c;
  });
  while (!unit.empty() && unit.front() == ' ') {
    unit.erase(0, 1);
  }
  double bytes = 0;
  if (unit == "%") {
    if (!physical.has_value() || number > 100) {
      return std::nullopt;
    }
    bytes = static_cast<double>(*physical) * number / 100;
  } else {
    constexpr auto kUnits = std::to_array<std::pair<std::string_view, double>>({
        {"", 1},
        {"B", 1},
        {"KB", 1e3},
        {"MB", 1e6},
        {"GB", 1e9},
        {"TB", 1e12},
        {"KIB", 0x1p10},
        {"MIB", 0x1p20},
        {"GIB", 0x1p30},
        {"TIB", 0x1p40},
    });
    const auto* it = std::ranges::find(kUnits, std::string_view(unit),
                                       &std::pair<std::string_view, double>::first);
    if (it == kUnits.end()) {
      return std::nullopt;
    }
    bytes = number * it->second;
  }
  // Below 2^63 (9.2e18) the conversion is exact enough and cannot overflow.
  if (!(bytes >= 1) || bytes >= 9e18) {
    return std::nullopt;
  }
  return static_cast<int64_t>(bytes);
}

std::optional<int64_t> PhysicalMemory() {
  const long pages = sysconf(_SC_PHYS_PAGES);    // NOLINT(google-runtime-int): the POSIX type
  const long page_size = sysconf(_SC_PAGESIZE);  // NOLINT(google-runtime-int): the POSIX type
  if (pages <= 0 || page_size <= 0) {
    return std::nullopt;
  }
  return static_cast<int64_t>(pages) * static_cast<int64_t>(page_size);
}

}  // namespace antb1::cli
