#pragma once

#include <string>
#include <vector>

#include <arrow/result.h>

namespace antb1::io {

// Expands a path that may contain *, ? or [...] into the sorted list of matching files.
// A pattern without wildcards is returned as is (existence is checked when the file is opened).
// IOError if a wildcard pattern matches nothing.
arrow::Result<std::vector<std::string>> ExpandGlob(const std::string& pattern);

}  // namespace antb1::io
