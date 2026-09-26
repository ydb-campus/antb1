#pragma once

#include <cstddef>
#include <string_view>

namespace antb1 {

// Length (2 to 4) of the well-formed UTF-8 sequence that starts with the non-ASCII byte s[i], or 0
// when the bytes there are ill-formed or cut short by the end of s: RFC 3629 and Table 3-7 of the
// Unicode standard, which rule out C0, C1 and F5..FF, overlong forms, surrogates and code points
// above U+10FFFF. An ASCII byte also gives 0; callers handle ASCII themselves. Requires i <
// s.size().
std::size_t Utf8SequenceLength(std::string_view s, std::size_t i);

}  // namespace antb1
