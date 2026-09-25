#pragma once

#include <string>

#include "antb1/sql/ast.h"

namespace antb1::sql {

// Canonical SQL text for a statement: upper-case keywords, single spaces, column/table names quoted
// only when they were quoted in the source, aliases always quoted, numbers as written, comparisons
// column-first. For every statement Parse() returns, Parse(ToSql(s)) is EqualIgnoringSpans to s and
// ToSql is idempotent.
std::string ToSql(const SelectStatement& stmt);

}  // namespace antb1::sql
