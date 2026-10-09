#pragma once

#include <string>

#include "antb1/sql/ast.h"

namespace antb1::sql {

// Canonical SQL text for a statement: upper-case keywords, single spaces, column/table names and
// qualifiers quoted only when they were quoted in the source, aliases always quoted after AS (a
// table alias too, so it never reads back as a keyword), and so are the names of a WITH list and of
// a column alias list (WITH "c"("x") AS (...), FROM (...) AS "s"("x")), JOIN as INNER JOIN and
// LEFT OUTER JOIN as LEFT JOIN, numbers as written, expressions in the order written with only the
// parentheses that the operator precedence needs (a unary minus always takes them), casts as
// CAST(x AS T) (also x::T). For every statement Parse() returns, Parse(ToSql(s)) is
// EqualIgnoringSpans to s and ToSql is idempotent.
std::string ToSql(const SelectStatement& stmt);

// The canonical SQL text of one expression.
std::string ToSql(const Expr& expr);

}  // namespace antb1::sql
