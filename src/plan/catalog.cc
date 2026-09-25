#include "antb1/plan/catalog.h"

#include <cctype>
#include <memory>
#include <string>
#include <string_view>

#include <arrow/status.h>

namespace antb1::plan {

std::string AsciiLower(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

arrow::Status Catalog::Register(const std::string& name, const std::shared_ptr<Table>& table) {
  if (!table) {
    return arrow::Status::Invalid("cannot register a null table as '", name, "'");
  }
  auto [it, inserted] = tables_.try_emplace(AsciiLower(name), name, table);
  if (!inserted) {
    return arrow::Status::AlreadyExists("table '", name, "' is already registered (as '",
                                        it->second.first, "')");
  }
  return arrow::Status::OK();
}

std::shared_ptr<Table> Catalog::Find(std::string_view name) const {
  const auto it = tables_.find(AsciiLower(name));
  return it == tables_.end() ? nullptr : it->second.second;
}

arrow::Result<std::shared_ptr<Table>> Catalog::OpenPath(const std::string& path) const {
  if (!opener_) {
    return arrow::Status::NotImplemented("file paths in FROM are not enabled in this session");
  }
  return opener_(path);
}

}  // namespace antb1::plan
