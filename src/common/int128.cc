#include "antb1/common/int128.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
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

}  // namespace antb1
