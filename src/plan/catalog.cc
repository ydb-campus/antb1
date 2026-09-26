#include "antb1/plan/catalog.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>

#include <arrow/status.h>

namespace antb1::plan {

std::string AsciiLower(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');  // ASCII only, whatever the C locale
    }
  }
  return out;
}

bool IsPlainIdentifier(std::string_view name) {
  const auto word = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
  };
  const bool starts_with_digit = !name.empty() && name.front() >= '0' && name.front() <= '9';
  return !name.empty() && !starts_with_digit && std::ranges::all_of(name, word);
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
