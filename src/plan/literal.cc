#include "antb1/plan/literal.h"

#include <algorithm>
#include <charconv>
#include <chrono>
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
  const std::chrono::year_month_day date{std::chrono::sys_days{std::chrono::days{days}}};
  return std::format("{:04}-{:02}-{:02}", static_cast<int>(date.year()),
                     static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()));
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
