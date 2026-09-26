#include "antb1/plan/literal.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "antb1/common/check.h"
#include "antb1/common/int128.h"
#include "antb1/common/narrow.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

namespace antb1::plan {
namespace {

// Larger exponents saturate; the result stays exact (huge, or a fraction of magnitude 0).
constexpr int64_t kMaxExponent = 1'000'000'000;
// Integer digits of the largest HUGEINT (10^38 - 1); more digits are huge.
constexpr int64_t kMaxIntegerDigits = 38;

constexpr Int128 Pow10(int64_t n) {
  Int128 value = 1;
  for (int64_t i = 0; i < n; ++i) {
    value *= 10;
  }
  return value;
}

constexpr Int128 kHugeIntMax = Pow10(kMaxIntegerDigits) - 1;

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

// Value of at most 38 decimal digits.
Int128 DigitsValue(std::string_view digits) {
  Int128 value = 0;
  for (const char c : digits) {
    value = (value * 10) + (c - '0');
  }
  return value;
}

}  // namespace

std::optional<ExactNumber> ParseExactNumber(std::string_view text, bool negative) {
  std::size_t pos = 0;
  const auto digits = [&text, &pos] {
    const std::size_t begin = pos;
    while (pos < text.size() && IsDigit(text[pos])) {
      ++pos;
    }
    return text.substr(begin, pos - begin);
  };
  const std::string_view integer_part = digits();
  std::string_view fraction_part;
  if (pos < text.size() && text[pos] == '.') {
    ++pos;
    fraction_part = digits();
  }
  if (integer_part.empty() && fraction_part.empty()) {
    return std::nullopt;
  }
  int64_t exponent = 0;
  if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
    ++pos;
    const bool negative_exponent = pos < text.size() && text[pos] == '-';
    if (pos < text.size() && (text[pos] == '-' || text[pos] == '+')) {
      ++pos;
    }
    const std::string_view exponent_digits = digits();
    if (exponent_digits.empty()) {
      return std::nullopt;
    }
    for (const char c : exponent_digits) {
      exponent = std::min((exponent * 10) + (c - '0'), kMaxExponent);
    }
    exponent = negative_exponent ? -exponent : exponent;
  }
  if (pos != text.size()) {
    return std::nullopt;
  }
  // |value| = significand * 10^scale, where the significand has no leading zeros.
  std::string significand(integer_part);
  significand += fraction_part;
  const std::size_t first = significand.find_first_not_of('0');
  ExactNumber number;
  if (first == std::string::npos) {
    return number;  // zero, also when written as -0.0
  }
  number.negative = negative;
  const std::string_view sig = std::string_view(significand).substr(first);
  const int64_t scale = exponent - Narrow<int64_t>(fraction_part.size());
  const int64_t integer_digits = Narrow<int64_t>(sig.size()) + scale;
  if (integer_digits > kMaxIntegerDigits) {
    number.huge = true;
  } else if (scale >= 0) {
    number.magnitude = DigitsValue(sig) * Pow10(scale);
  } else if (integer_digits > 0) {
    const auto split = Narrow<std::size_t>(integer_digits);
    number.magnitude = DigitsValue(sig.substr(0, split));
    number.fraction = sig.substr(split).find_first_not_of('0') != std::string_view::npos;
  } else {
    number.fraction = true;  // 0 < |value| < 1: sig starts with a non-zero digit
  }
  return number;
}

std::optional<double> ParseDoubleLiteral(std::string_view text, bool negative) {
  const auto exact = ParseExactNumber(text, negative);
  if (!exact.has_value()) {
    return std::nullopt;
  }
  double value = 0;
  const char* const first = std::to_address(text.begin());
  const char* const end = std::to_address(text.end());
  const auto [ptr, ec] = std::from_chars(first, end, value, std::chars_format::general);
  if (ec == std::errc::result_out_of_range) {
    value = exact->huge ? std::numeric_limits<double>::infinity() : 0.0;
  } else if (ec != std::errc{} || ptr != end) {
    return std::nullopt;
  }
  return negative ? -value : value;
}

namespace {

// DuckDB's NumericHelper::DOUBLE_POWERS_OF_TEN: correctly rounded doubles.
constexpr std::array<double, 39> kDoublePowersOfTen = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11, 1e12,
    1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22, 1e23, 1e24, 1e25,
    1e26, 1e27, 1e28, 1e29, 1e30, 1e31, 1e32, 1e33, 1e34, 1e35, 1e36, 1e37, 1e38};

// double(UINT64_MAX), which rounds to 2^64; DuckDB's casts are written with it.
constexpr auto kUint64MaxAsDouble = static_cast<double>(std::numeric_limits<uint64_t>::max());

// DuckDB's Hugeint::TryCast(hugeint_t, double) (CastBigintToFloating), from the two's-complement
// halves of the value.
double DuckDbHugeintToDouble(Int128 value) {
  const auto bits = static_cast<UInt128>(value);
  const auto lower = static_cast<uint64_t>(bits);
  const auto upper = static_cast<int64_t>(static_cast<uint64_t>(bits >> 64U));
  if (upper == -1) {
    // DuckDB's special case for small negative numbers: -double(UINT64_MAX - lower) - 1.
    return -static_cast<double>(std::numeric_limits<uint64_t>::max() - lower) - 1;
  }
  return static_cast<double>(lower) + (static_cast<double>(upper) * (kUint64MaxAsDouble + 1));
}

// DuckDB's Uhugeint::TryCast(uhugeint_t, double) (CastUhugeintToFloating).
double DuckDbUhugeintToDouble(UInt128 value) {
  const auto lower = static_cast<uint64_t>(value);
  const auto upper = static_cast<uint64_t>(value >> 64U);
  return static_cast<double>(lower) + (static_cast<double>(upper) * kUint64MaxAsDouble);
}

// DuckDB's cast of a DECIMAL's storage integer to float: int16/int32/int64 directly (width up to
// 18), hugeint through a double.
float DuckDbStorageToFloat(Int128 value, std::size_t width) {
  if (width <= 18) {
    return static_cast<float>(static_cast<int64_t>(value));
  }
  return static_cast<float>(DuckDbHugeintToDouble(value));
}

// DuckDB's TryCastDecimalToFloatingPoint<SRC, float>.
float DuckDbDecimalToFloat(Int128 unscaled, std::size_t width, std::size_t scale) {
  constexpr Int128 kMaxExactInFloat = 16'777'216;  // 2^24, MAX_INT_REPRESENTABLE_IN_FLOAT
  const auto power = static_cast<float>(kDoublePowersOfTen.at(scale));
  // int16 storage (width <= 4) is always "representable exactly" in DuckDB.
  if (width <= 4 || scale == 0 || (unscaled <= kMaxExactInFloat && unscaled >= -kMaxExactInFloat)) {
    return DuckDbStorageToFloat(unscaled, width) / power;
  }
  Int128 pow10 = 1;
  for (std::size_t i = 0; i < scale; ++i) {
    pow10 *= 10;
  }
  const Int128 div = unscaled / pow10;  // truncating, as in C++ and DuckDB
  const Int128 mod = unscaled % pow10;
  return DuckDbStorageToFloat(div, width) + (DuckDbStorageToFloat(mod, width) / power);
}

}  // namespace

std::optional<float> DuckDbFloatOf(std::string_view text, bool negative) {
  if (IsApproximateNumber(text) || !ParseExactNumber(text, negative).has_value()) {
    return std::nullopt;  // an exponent or more than 38 digits: DOUBLE
  }
  const std::size_t dot = text.find('.');
  if (dot != std::string_view::npos) {
    // DECIMAL(width, scale): every digit counts, leading zeros too.
    const std::size_t scale = text.size() - dot - 1;
    const std::size_t width = text.size() - 1;
    std::string digits(text.substr(0, dot));
    digits += text.substr(dot + 1);
    const Int128 magnitude = DigitsValue(digits);
    return DuckDbDecimalToFloat(negative ? -magnitude : magnitude, std::max<std::size_t>(width, 1),
                                scale);
  }
  // An integer, typed by value; the magnitude is checked against 2^128 - 1 while it is read.
  UInt128 magnitude = 0;
  for (const char c : text) {
    if (__builtin_mul_overflow(magnitude, UInt128{10}, &magnitude) ||
        __builtin_add_overflow(magnitude, static_cast<UInt128>(c - '0'), &magnitude)) {
      return std::nullopt;  // beyond UHUGEINT: DOUBLE
    }
  }
  constexpr UInt128 kHugeIntLimit = UInt128{1} << 127U;  // |HUGEINT min|
  if (negative) {
    if (magnitude > kHugeIntLimit) {
      return std::nullopt;  // below HUGEINT's minimum: DOUBLE
    }
    const auto value = static_cast<Int128>(UInt128{0} - magnitude);
    if (const auto small = Int128ToInt64(value)) {
      return static_cast<float>(*small);  // INTEGER or BIGINT
    }
    return static_cast<float>(DuckDbHugeintToDouble(value));
  }
  if (magnitude >= kHugeIntLimit) {
    return static_cast<float>(DuckDbUhugeintToDouble(magnitude));  // UHUGEINT
  }
  const auto value = static_cast<Int128>(magnitude);
  if (const auto small = Int128ToInt64(value)) {
    return static_cast<float>(*small);
  }
  return static_cast<float>(DuckDbHugeintToDouble(value));
}

bool IsApproximateNumber(std::string_view text) {
  if (text.find_first_of("eE") != std::string_view::npos) {
    return true;
  }
  return text.contains('.') && std::ranges::count_if(text, IsDigit) > kMaxIntegerDigits;
}

ExactNumber ExactNumberOf(double value) {
  ExactNumber number;
  if (std::isnan(value) || value == 0) {
    return number;
  }
  number.negative = value < 0;
  const double magnitude = std::fabs(value);
  if (!(magnitude < 0x1p127)) {  // +-inf too
    number.huge = true;
    return number;
  }
  const double whole = std::floor(magnitude);
  number.magnitude = static_cast<Int128>(whole);  // exact: an integral double below 2^127
  if (number.magnitude > kHugeIntMax) {
    number.magnitude = 0;
    number.huge = true;
    return number;
  }
  number.fraction = whole != magnitude;
  return number;
}

std::optional<int32_t> ParseDate(std::string_view text) {
  if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
    return std::nullopt;
  }
  const auto field = [text](std::size_t pos, std::size_t len) -> std::optional<int> {
    int value = 0;
    for (const char c : text.substr(pos, len)) {
      if (!IsDigit(c)) {
        return std::nullopt;
      }
      value = (value * 10) + (c - '0');
    }
    return value;
  };
  const auto year = field(0, 4);
  const auto month = field(5, 2);
  const auto day = field(8, 2);
  if (!year.has_value() || !month.has_value() || !day.has_value()) {
    return std::nullopt;
  }
  const std::chrono::year_month_day date{std::chrono::year{*year},
                                         std::chrono::month{static_cast<unsigned>(*month)},
                                         std::chrono::day{static_cast<unsigned>(*day)}};
  if (!date.ok()) {
    return std::nullopt;
  }
  return Narrow<int32_t>(std::chrono::sys_days{date}.time_since_epoch().count());
}

std::string FormatDate(int32_t days) {
  if (days == std::numeric_limits<int32_t>::max()) {
    return "infinity";
  }
  if (days == -std::numeric_limits<int32_t>::max()) {
    return "-infinity";
  }
  // std::chrono::year holds only -32767..32767, so the civil date is computed in 64 bits
  // (H. Hinnant's days_from_civil inverse: 400-year eras of 146097 days starting on March 1).
  const int64_t z = int64_t{days} + 719'468;
  const int64_t era = (z >= 0 ? z : z - 146'096) / 146'097;
  const int64_t day_of_era = z - (era * 146'097);
  const int64_t year_of_era =
      (day_of_era - (day_of_era / 1'460) + (day_of_era / 36'524) - (day_of_era / 146'096)) / 365;
  const int64_t day_of_year =
      day_of_era - ((365 * year_of_era) + (year_of_era / 4) - (year_of_era / 100));
  const int64_t shifted_month = ((5 * day_of_year) + 2) / 153;  // 0 = March
  const int64_t day = day_of_year - (((153 * shifted_month) + 2) / 5) + 1;
  const int64_t month = shifted_month < 10 ? shifted_month + 3 : shifted_month - 9;
  const int64_t year = year_of_era + (era * 400) + (month <= 2 ? 1 : 0);
  if (year <= 0) {
    return std::format("{:04}-{:02}-{:02} (BC)", 1 - year, month, day);
  }
  return std::format("{:04}-{:02}-{:02}", year, month, day);
}

IntegerRange RangeOf(LogicalType integer_type) {
  switch (integer_type) {
    case LogicalType::kSmallInt:
      return {.min = std::numeric_limits<int16_t>::min(),
              .max = std::numeric_limits<int16_t>::max()};
    case LogicalType::kInteger:
      return {.min = std::numeric_limits<int32_t>::min(),
              .max = std::numeric_limits<int32_t>::max()};
    case LogicalType::kBigInt:
      return {.min = std::numeric_limits<int64_t>::min(),
              .max = std::numeric_limits<int64_t>::max()};
    case LogicalType::kUSmallInt:
      return {.min = 0, .max = std::numeric_limits<uint16_t>::max()};
    case LogicalType::kHugeInt:
      return {.min = -kHugeIntMax, .max = kHugeIntMax};
    case LogicalType::kDouble:
    case LogicalType::kVarchar:
    case LogicalType::kDate:
      break;
  }
  ANTB1_CHECK(IsInteger(integer_type));
  return {};
}

FoldedComparison FoldIntegerComparison(CompareOp op, const ExactNumber& literal,
                                       IntegerRange range) {
  using Kind = Predicate::Kind;
  const Int128 magnitude = literal.magnitude;
  Int128 value = literal.negative ? -magnitude : magnitude;
  if (literal.fraction && !literal.huge) {
    // No integer equals a non-integer; otherwise compare with the nearest integer on the side the
    // comparison keeps: c > 1.5 and c >= 1.5 are c >= 2; c < -1.5 and c <= -1.5 are c <= -2.
    switch (op) {
      case CompareOp::kEq:
        return {.kind = Kind::kFalse};
      case CompareOp::kNe:
        return {.kind = Kind::kIsNotNull};
      case CompareOp::kGt:
      case CompareOp::kGe:
        op = CompareOp::kGe;
        value = literal.negative ? -magnitude : magnitude + 1;  // ceiling
        break;
      case CompareOp::kLt:
      case CompareOp::kLe:
        op = CompareOp::kLe;
        value = literal.negative ? -(magnitude + 1) : magnitude;  // floor
        break;
    }
  }
  const bool below = literal.huge ? literal.negative : value < range.min;
  const bool above = literal.huge ? !literal.negative : value > range.max;
  if (!below && !above) {
    return {.kind = Kind::kCompare, .op = op, .value = value};
  }
  // Every column value lies on the other side of the literal.
  bool always = false;
  switch (op) {
    case CompareOp::kEq:
      break;
    case CompareOp::kNe:
      always = true;
      break;
    case CompareOp::kLt:
    case CompareOp::kLe:
      always = above;
      break;
    case CompareOp::kGt:
    case CompareOp::kGe:
      always = below;
      break;
  }
  return {.kind = always ? Kind::kIsNotNull : Kind::kFalse};
}

}  // namespace antb1::plan
