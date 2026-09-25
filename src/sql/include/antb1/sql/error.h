#pragma once

#include <string>

#include "antb1/common/source_span.h"

namespace antb1::sql {

// Errors of the Arrow-free front end. kUnsupported marks valid-looking SQL outside the supported
// subset (docs/sql-subset.md); the CLI maps it to exit code 4.
struct ParseError {
  enum class Kind { kSyntax, kUnsupported };

  Kind kind = Kind::kSyntax;
  std::string message;
  SourceSpan span;
};

}  // namespace antb1::sql
