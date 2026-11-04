#include "antb1/exec/case_mapping.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace antb1::exec {
namespace {

// A codepoint and the one it maps to; a pair of an array, which the generated table initializes
// positionally.
using CaseMapping = std::array<char32_t, 2>;

#include "unicode_case.inc"

char32_t Mapped(char32_t codepoint, bool upper) {
  const auto& table =
      upper ? std::span<const CaseMapping>(kUpperCase) : std::span<const CaseMapping>(kLowerCase);
  const auto found =
      std::ranges::lower_bound(table, codepoint, {}, [](const CaseMapping& m) { return m[0]; });
  return found != table.end() && (*found)[0] == codepoint ? (*found)[1] : codepoint;
}

// The codepoint at `value[*at]`, advancing `*at` past it; std::nullopt for bytes that are no
// well-formed UTF-8 sequence (an overlong form, a surrogate, a codepoint past U+10FFFF, a cut-off
// or a stray continuation byte).
std::optional<char32_t> Decode(std::string_view value, std::size_t* at) {
  const auto byte = [&value](std::size_t i) { return static_cast<unsigned char>(value[i]); };
  const unsigned char lead = byte(*at);
  std::size_t length = 0;
  char32_t codepoint = 0;
  char32_t least = 0;
  if (lead < 0x80U) {
    ++*at;
    return lead;
  }
  if ((lead & 0xE0U) == 0xC0U) {
    length = 2;
    codepoint = lead & 0x1FU;
    least = 0x80;
  } else if ((lead & 0xF0U) == 0xE0U) {
    length = 3;
    codepoint = lead & 0x0FU;
    least = 0x800;
  } else if ((lead & 0xF8U) == 0xF0U) {
    length = 4;
    codepoint = lead & 0x07U;
    least = 0x10000;
  } else {
    return std::nullopt;
  }
  if (*at + length > value.size()) {
    return std::nullopt;
  }
  for (std::size_t i = 1; i < length; ++i) {
    const unsigned char next = byte(*at + i);
    if ((next & 0xC0U) != 0x80U) {
      return std::nullopt;
    }
    codepoint = (codepoint << 6U) | (next & 0x3FU);
  }
  if (codepoint < least || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
    return std::nullopt;
  }
  *at += length;
  return codepoint;
}

void Encode(char32_t codepoint, std::string& out) {
  const auto put = [&out](unsigned value) { out.push_back(static_cast<char>(value)); };
  if (codepoint < 0x80) {
    put(codepoint);
  } else if (codepoint < 0x800) {
    put(0xC0U | (codepoint >> 6U));
    put(0x80U | (codepoint & 0x3FU));
  } else if (codepoint < 0x10000) {
    put(0xE0U | (codepoint >> 12U));
    put(0x80U | ((codepoint >> 6U) & 0x3FU));
    put(0x80U | (codepoint & 0x3FU));
  } else {
    put(0xF0U | (codepoint >> 18U));
    put(0x80U | ((codepoint >> 12U) & 0x3FU));
    put(0x80U | ((codepoint >> 6U) & 0x3FU));
    put(0x80U | (codepoint & 0x3FU));
  }
}

}  // namespace

bool CaseConvertValue(std::string_view value, bool upper, std::string& out) {
  out.clear();
  out.reserve(value.size());
  std::size_t at = 0;
  while (at < value.size()) {
    const auto c = static_cast<unsigned char>(value[at]);
    if (c < 0x80U) {  // ASCII
      const bool flips = upper ? (c >= 'a' && c <= 'z') : (c >= 'A' && c <= 'Z');
      out.push_back(static_cast<char>(flips ? c ^ 0x20U : c));
      ++at;
      continue;
    }
    const std::optional<char32_t> codepoint = Decode(value, &at);
    if (!codepoint.has_value()) {
      return false;
    }
    Encode(Mapped(*codepoint, upper), out);
  }
  return true;
}

}  // namespace antb1::exec
