#include "antb1/common/source_span.h"

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace antb1 {

LineColumn ToLineColumn(std::string_view text, SourceSpan span) {
  LineColumn result;
  const std::size_t end = std::min(span.offset, text.size());
  for (std::size_t i = 0; i < end; ++i) {
    if (text[i] == '\n') {
      ++result.line;
      result.column = 1;
    } else {
      ++result.column;
    }
  }
  return result;
}

}  // namespace antb1
