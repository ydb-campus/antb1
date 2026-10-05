#include "antb1/common/int128.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
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

namespace {

constexpr int kMaxPowerOfTen = 38;

constexpr std::array<Int128, kMaxPowerOfTen + 1> kPowersOfTen = [] {
  std::array<Int128, kMaxPowerOfTen + 1> powers{};
  powers.at(0) = 1;
  for (std::size_t i = 1; i < powers.size(); ++i) {
    powers.at(i) = powers.at(i - 1) * 10;
  }
  return powers;
}();

}  // namespace

Int128 PowerOfTen(int n) {
  ANTB1_CHECK(n >= 0);
  ANTB1_CHECK(n <= kMaxPowerOfTen);
  return kPowersOfTen.at(static_cast<std::size_t>(n));
}

int CompareScaled(Int128 a, int a_scale, Int128 b, int b_scale) {
  if (a_scale < b_scale) {
    Int128 scaled = 0;
    if (__builtin_mul_overflow(a, PowerOfTen(b_scale - a_scale), &scaled)) {
      return a < 0 ? -1 : 1;
    }
    a = scaled;
  } else if (b_scale < a_scale) {
    Int128 scaled = 0;
    if (__builtin_mul_overflow(b, PowerOfTen(a_scale - b_scale), &scaled)) {
      return b < 0 ? 1 : -1;
    }
    b = scaled;
  }
  if (a == b) {
    return 0;
  }
  return a < b ? -1 : 1;
}

std::optional<Int128> Rescale(Int128 value, int from, int to) {
  if (to >= from) {
    Int128 scaled = 0;
    if (__builtin_mul_overflow(value, PowerOfTen(to - from), &scaled)) {
      return std::nullopt;
    }
    return scaled;
  }
  // f / 2 is at most 5 * 10^37, so no step overflows, the minimum included.
  Int128 halves = value / (PowerOfTen(from - to) / 2);
  halves += halves < 0 ? -1 : 1;
  return halves / 2;
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

// DuckDB's NumericHelper::DOUBLE_POWERS_OF_TEN: correctly rounded doubles.
constexpr std::array<double, 39> kDoublePowersOfTen = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11, 1e12,
    1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22, 1e23, 1e24, 1e25,
    1e26, 1e27, 1e28, 1e29, 1e30, 1e31, 1e32, 1e33, 1e34, 1e35, 1e36, 1e37, 1e38};

// DuckDB's cast of a DECIMAL's storage integer: int16/int32/int64 directly (a width up to 18), a
// hugeint through DOUBLE (also for FLOAT).
template <class Real>
Real DuckDbStorageToReal(Int128 value, int width) {
  if (width <= 18) {
    return static_cast<Real>(static_cast<int64_t>(value));
  }
  return static_cast<Real>(DuckDbHugeintToReal<double>(value));
}

// DuckDB's TryCastDecimalToFloatingPoint<SRC, Real>.
template <class Real>
Real DuckDbDecimalToReal(Int128 unscaled, int width, int scale) {
  ANTB1_CHECK(scale >= 0);
  ANTB1_CHECK(scale <= 38);
  // MAX_INT_REPRESENTABLE_IN_FLOAT / _IN_DOUBLE.
  constexpr auto kMaxExact =
      static_cast<Int128>(UInt128{1} << static_cast<unsigned>(std::numeric_limits<Real>::digits));
  const auto power = static_cast<Real>(kDoublePowersOfTen.at(static_cast<std::size_t>(scale)));
  // int16 storage (width <= 4) is always "representable exactly" in DuckDB.
  if (width <= 4 || scale == 0 || (unscaled <= kMaxExact && unscaled >= -kMaxExact)) {
    return DuckDbStorageToReal<Real>(unscaled, width) / power;
  }
  Int128 pow10 = 1;
  for (int i = 0; i < scale; ++i) {
    pow10 *= 10;
  }
  const Int128 div = unscaled / pow10;  // truncating, as in C++ and DuckDB
  const Int128 mod = unscaled % pow10;
  return DuckDbStorageToReal<Real>(div, width) + (DuckDbStorageToReal<Real>(mod, width) / power);
}

}  // namespace

double DuckDbHugeintToDouble(Int128 value) { return DuckDbHugeintToReal<double>(value); }

double DuckDbDecimalToDouble(Int128 unscaled, int width, int scale) {
  return DuckDbDecimalToReal<double>(unscaled, width, scale);
}

float DuckDbDecimalToFloat(Int128 unscaled, int width, int scale) {
  return DuckDbDecimalToReal<float>(unscaled, width, scale);
}

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
