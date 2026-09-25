#pragma once

#include <concepts>
#include <optional>
#include <utility>

#include "antb1/common/check.h"

namespace antb1 {

// Value-preserving integer conversion, or std::nullopt. (Named Narrow* to avoid confusion with
// arrow::internal::checked_cast, which is a pointer cast.)
template <std::integral To, std::integral From>
constexpr std::optional<To> TryNarrow(From value) noexcept {
  if (!std::in_range<To>(value)) {
    return std::nullopt;
  }
  return static_cast<To>(value);
}

// Value-preserving integer conversion; aborts on loss. For values that are in range by construction
// (e.g. Arrow's int row-group indices vs int64_t counts). Never use it on user input.
template <std::integral To, std::integral From>
constexpr To Narrow(From value) {
  ANTB1_CHECK(std::in_range<To>(value));
  return static_cast<To>(value);
}

}  // namespace antb1
