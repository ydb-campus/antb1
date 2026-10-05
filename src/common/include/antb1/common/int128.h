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

// 10^n for 0 <= n <= 38, the largest power of ten an Int128 holds.
Int128 PowerOfTen(int n);

// Compares a / 10^a_scale with b / 10^b_scale exactly (scales 0 to 38): -1, 0 or 1. The side with
// the smaller scale is multiplied up; when that overflows, its magnitude exceeds every Int128, so
// its sign decides. Any Int128 is allowed, the minimum included (nothing is negated).
int CompareScaled(Int128 a, int a_scale, Int128 b, int b_scale);

// The unscaled value of scale `from` at scale `to` (scales 0 to 38), as DuckDB casts a DECIMAL:
// multiplied up exactly (std::nullopt when that overflows an Int128), or divided down and rounded
// half away from zero (q = value / (f / 2), then q +- 1, then q / 2, with f = 10^(from - to)).
std::optional<Int128> Rescale(Int128 value, int from, int to);

// numerator/denominator (denominator > 0) as a double, used for AVG over integers. Accurate to
// about 1 ulp (exact integer accumulation, then one long double division); oracle comparisons use a
// relative tolerance.
double ExactDivideToDouble(Int128 numerator, int64_t denominator);

// DuckDB's AVG of `count` (> 0) DECIMAL(width, scale) values from the exact sum of their unscaled
// values (ADR 0021 rule 13): the sum as a long double from its two 64-bit halves (lower + upper *
// 2^64), divided in long double by count * 10^scale (10^scale first converted to a double the same
// way), then rounded to double; in double for a width up to 4 (DuckDB's 16-bit DECIMAL). long
// double is the platform's, as in DuckDB's own build.
double DuckDbDecimalAverage(Int128 sum, int64_t count, int width, int scale);

// DuckDB's cast of a HUGEINT to DOUBLE (Hugeint::TryCast, CastBigintToFloating): lower + upper *
// 2^64 from the two's-complement halves, with its special case for an upper half of -1.
double DuckDbHugeintToDouble(Int128 value);

// DuckDB's cast of a DECIMAL(width, scale) with the unscaled value `unscaled` to DOUBLE or FLOAT
// (TryCastDecimalToFloatingPoint, ADR 0021 rule 8): the storage integer (int64 up to 18 digits,
// else the 128-bit formula above) divided by 10^scale when the width is at most 4, the scale 0 or
// |unscaled| at most 2^53 (2^24 for FLOAT); otherwise (unscaled div 10^scale) + (unscaled mod
// 10^scale) / 10^scale, truncating toward zero, each part converted on its own. Not always
// correctly rounded.
double DuckDbDecimalToDouble(Int128 unscaled, int width, int scale);
float DuckDbDecimalToFloat(Int128 unscaled, int width, int scale);

}  // namespace antb1
