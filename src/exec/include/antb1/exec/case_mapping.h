#pragma once

#include <string>
#include <string_view>

// upper() and lower() of a VARCHAR value as DuckDB 1.5.6 computes them (ADR 0037's FN4): each
// codepoint mapped by the simple case mapping of DuckDB's bundled utf8proc, whose Unicode version
// is DuckDB's own and not the one Arrow links, so the table is DuckDB's (src/exec/unicode_case.inc,
// written by tools/unicode/case_tables.py). Every mapping is one codepoint to one codepoint.

namespace antb1::exec {

// `value` upper-cased (`upper`) or lower-cased into `out`; false, with `out` unspecified, when
// `value` is no valid UTF-8, which then stays as it is (divergence D15).
bool CaseConvertValue(std::string_view value, bool upper, std::string& out);

}  // namespace antb1::exec
