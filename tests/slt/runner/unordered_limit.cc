#include "unordered_limit.h"

#include <optional>
#include <string>
#include <string_view>

#include "antb1/sql/parser.h"
#include "antb1/sql/unparse.h"

namespace antb1::slt {

std::optional<std::string> UnlimitedSql(std::string_view sql) {
  auto stmt = sql::Parse(sql);
  if (!stmt.has_value() || !stmt->order_by.empty() ||
      (!stmt->limit.has_value() && !stmt->offset.has_value())) {
    return std::nullopt;
  }
  stmt->limit.reset();
  stmt->offset.reset();
  return sql::ToSql(*stmt);
}

}  // namespace antb1::slt
