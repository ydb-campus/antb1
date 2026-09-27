#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace antb1::slt {

// For a query with LIMIT or OFFSET but no ORDER BY, where any rows of the unlimited answer are a
// right answer: the same query without LIMIT and OFFSET (antb1's canonical SQL, which DuckDB also
// reads). std::nullopt for any other query, or text antb1's parser does not accept.
std::optional<std::string> UnlimitedSql(std::string_view sql);

}  // namespace antb1::slt
