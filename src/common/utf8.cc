#include "antb1/common/utf8.h"

#include <cstddef>
#include <string_view>

namespace antb1 {

std::size_t Utf8SequenceLength(std::string_view s, std::size_t i) {
  const auto byte = [&](std::size_t k) {
    return static_cast<unsigned>(static_cast<unsigned char>(s[k]));
  };
  const unsigned lead = byte(i);
  std::size_t len = 0;
  unsigned lo = 0x80U;  // range of the second byte
  unsigned hi = 0xBFU;
  if (lead >= 0xC2U && lead <= 0xDFU) {
    len = 2;
  } else if (lead >= 0xE0U && lead <= 0xEFU) {
    len = 3;
    lo = lead == 0xE0U ? 0xA0U : lo;  // E0 80..9F: overlong
    hi = lead == 0xEDU ? 0x9FU : hi;  // ED A0..BF: surrogates
  } else if (lead >= 0xF0U && lead <= 0xF4U) {
    len = 4;
    lo = lead == 0xF0U ? 0x90U : lo;  // F0 80..8F: overlong
    hi = lead == 0xF4U ? 0x8FU : hi;  // F4 90..BF: above U+10FFFF
  } else {
    return 0;  // ASCII, a continuation byte, C0, C1 or F5..FF
  }
  if (len > s.size() - i || byte(i + 1) < lo || byte(i + 1) > hi) {
    return 0;
  }
  for (std::size_t k = 2; k < len; ++k) {
    if ((byte(i + k) & 0xC0U) != 0x80U) {
      return 0;
    }
  }
  return len;
}

}  // namespace antb1
