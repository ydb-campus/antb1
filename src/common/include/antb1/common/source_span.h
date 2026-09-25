#pragma once

#include <cstddef>
#include <string_view>

namespace antb1 {

// A byte range in the original query text; used by parse/bind errors and EXPLAIN.
struct SourceSpan {
  std::size_t offset = 0;
  std::size_t length = 0;

  friend bool operator==(const SourceSpan&, const SourceSpan&) = default;
};

// 1-based line/column of span.offset within text (columns count bytes).
struct LineColumn {
  std::size_t line = 1;
  std::size_t column = 1;
};

LineColumn ToLineColumn(std::string_view text, SourceSpan span);

}  // namespace antb1
