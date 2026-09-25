#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include <arrow/result.h>
#include <arrow/status.h>

#include "antb1/plan/table.h"

namespace antb1::plan {

// Opens a table for `FROM 'path'` references (installed by the engine; io implements it).
using PathOpener = std::function<arrow::Result<std::shared_ptr<Table>>(const std::string& path)>;

// Named tables. Names are matched ASCII case-insensitively (DuckDB semantics).
class Catalog {
 public:
  explicit Catalog(PathOpener opener = {}) : opener_(std::move(opener)) {}

  arrow::Status Register(const std::string& name, const std::shared_ptr<Table>& table);
  // nullptr if not found.
  [[nodiscard]] std::shared_ptr<Table> Find(std::string_view name) const;
  arrow::Result<std::shared_ptr<Table>> OpenPath(const std::string& path) const;
  [[nodiscard]] bool has_path_opener() const { return static_cast<bool>(opener_); }

 private:
  std::map<std::string, std::pair<std::string, std::shared_ptr<Table>>>
      tables_;  // lower -> (name, table)
  PathOpener opener_;
};

std::string AsciiLower(std::string_view text);

}  // namespace antb1::plan
