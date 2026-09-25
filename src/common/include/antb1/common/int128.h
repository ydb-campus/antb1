#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace antb1 {

// 128-bit integers for exact SUM/AVG accumulation (Arrow's integer "sum" silently wraps at int64).
// In strict C++23 (-std=c++23, no GNU extensions) libstdc++ does not treat __int128 as integral:
// std::is_integral, std::in_range, std::to_chars and std::make_unsigned do not support it.
// Use only the helpers below and __builtin_*_overflow; never std::in_range/TryNarrow on Int128.
// NOLINTBEGIN(modernize-use-using): __extension__ (silences -Wpedantic) only applies to typedef
// declarations.
__extension__ typedef __int128 Int128;
__extension__ typedef unsigned __int128 UInt128;
// NOLINTEND(modernize-use-using)

inline constexpr Int128 kInt128Max = static_cast<Int128>((static_cast<UInt128>(1) << 127U) - 1U);
inline constexpr Int128 kInt128Min = -kInt128Max - 1;

// Exact decimal representation, e.g. "-170141183460469231731687303715884105728".
std::string Int128ToString(Int128 value);

// value if it fits in int64_t, std::nullopt otherwise.
std::optional<int64_t> Int128ToInt64(Int128 value);

// a + b, or std::nullopt on overflow.
inline std::optional<Int128> CheckedAdd(Int128 a, Int128 b) {
  Int128 sum = 0;
  if (__builtin_add_overflow(a, b, &sum)) {
    return std::nullopt;
  }
  return sum;
}

// numerator/denominator (denominator > 0) as a double, used for AVG over integers. Accurate to
// about 1 ulp (exact integer accumulation, then one long double division); oracle comparisons use a
// relative tolerance.
double ExactDivideToDouble(Int128 numerator, int64_t denominator);

}  // namespace antb1
