#include "antb1/common/int128.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

#include "antb1/common/check.h"

namespace antb1 {

std::string Int128ToString(Int128 value) {
  if (value == 0) {
    return "0";
  }
  const bool negative = value < 0;
  // Work on the unsigned magnitude so that kInt128Min does not overflow.
  UInt128 magnitude = negative ? static_cast<UInt128>(0) - static_cast<UInt128>(value)
                               : static_cast<UInt128>(value);
  std::string digits;
  while (magnitude != 0) {
    digits.push_back(static_cast<char>('0' + static_cast<int>(magnitude % 10)));
    magnitude /= 10;
  }
  if (negative) {
    digits.push_back('-');
  }
  std::ranges::reverse(digits);
  return digits;
}

std::optional<int64_t> Int128ToInt64(Int128 value) {
  if (value < static_cast<Int128>(INT64_MIN) || value > static_cast<Int128>(INT64_MAX)) {
    return std::nullopt;
  }
  return static_cast<int64_t>(value);
}

double ExactDivideToDouble(Int128 numerator, int64_t denominator) {
  ANTB1_CHECK(denominator > 0);
  const Int128 den = denominator;
  const Int128 quotient = numerator / den;
  const Int128 remainder = numerator % den;
  // Split into integral quotient and fractional remainder so large sums do not lose the fraction.
  // Accurate to about 1 ulp; oracle comparisons of AVG use a relative tolerance.
  const auto whole = static_cast<long double>(quotient);
  const long double frac = static_cast<long double>(remainder) / static_cast<long double>(den);
  return static_cast<double>(whole + frac);
}

namespace {

// DuckDB's Hugeint::TryCast to a floating type (CastBigintToFloating), from the two's-complement
// halves of the value, with its special case for a negative upper half of -1.
template <class Real>
Real DuckDbHugeintToReal(Int128 value) {
  const auto bits = static_cast<UInt128>(value);
  const auto lower = static_cast<uint64_t>(bits);
  const auto upper = static_cast<int64_t>(static_cast<uint64_t>(bits >> 64U));
  constexpr uint64_t kUint64Max = std::numeric_limits<uint64_t>::max();
  if (upper == -1) {
    return -static_cast<Real>(kUint64Max - lower) - 1;
  }
  return static_cast<Real>(lower) +
         (static_cast<Real>(upper) * (static_cast<Real>(kUint64Max) + 1));
}

}  // namespace

double DuckDbDecimalAverage(Int128 sum, int64_t count, int width, int scale) {
  ANTB1_CHECK(count > 0);
  ANTB1_CHECK(scale >= 0);
  ANTB1_CHECK(scale <= 38);
  Int128 power = 1;
  for (int i = 0; i < scale; ++i) {
    power *= 10;
  }
  // DuckDB's AverageDecimalBindData holds 10^scale as a double.
  const auto power_of_ten = DuckDbHugeintToReal<double>(power);
  if (width <= 4) {
    // DuckDB sums a 16-bit DECIMAL in int64 and divides in double.
    const double divisor = static_cast<double>(count) * power_of_ten;
    return static_cast<double>(static_cast<int64_t>(sum)) / divisor;
  }
  const long double divisor =
      static_cast<long double>(count) * static_cast<long double>(power_of_ten);
  return static_cast<double>(DuckDbHugeintToReal<long double>(sum) / divisor);
}

}  // namespace antb1
