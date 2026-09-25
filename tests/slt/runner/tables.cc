#include "tables.h"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "antb1/io/glob.h"
#include "antb1/plan/catalog.h"

namespace antb1::slt {
namespace {

namespace fs = std::filesystem;

std::vector<std::string> Split(std::string_view text, std::string_view separators) {
  std::vector<std::string> parts;
  std::size_t pos = 0;
  while (pos < text.size()) {
    const std::size_t begin = text.find_first_not_of(separators, pos);
    if (begin == std::string_view::npos) {
      break;
    }
    const std::size_t end = std::min(text.find_first_of(separators, begin), text.size());
    parts.emplace_back(text.substr(begin, end - begin));
    pos = end;
  }
  return parts;
}

}  // namespace

std::expected<std::vector<TableDef>, std::string> LoadTables(const fs::path& tables_file,
                                                             const fs::path& fixtures_dir) {
  std::ifstream in(tables_file);
  if (!in) {
    return std::unexpected(std::format("cannot read tables file '{}'", tables_file.string()));
  }
  std::vector<TableDef> tables;
  int line_no = 0;
  for (std::string line; std::getline(in, line);) {
    ++line_no;
    const auto where = std::format("{}:{}", tables_file.string(), line_no);
    const auto words = Split(line, " \t");
    if (words.empty() || words[0].starts_with('#')) {
      continue;
    }
    if (words.size() < 2) {
      return std::unexpected(where + ": expected '<name> <file>[,<file>...] [option...]'");
    }
    TableDef table{.name = words[0], .files = {}, .patterns = {}, .clickbench = false};
    const bool duplicate = std::ranges::any_of(tables, [&](const TableDef& t) {
      return plan::AsciiLower(t.name) == plan::AsciiLower(table.name);
    });
    if (duplicate) {
      return std::unexpected(std::format("{}: duplicate table '{}'", where, table.name));
    }
    for (const auto& pattern : Split(words[1], ",")) {
      std::error_code absolute_ec;
      table.patterns.push_back(
          fs::absolute(fixtures_dir / pattern, absolute_ec).lexically_normal().string());
      auto files = io::ExpandGlob((fixtures_dir / pattern).string());
      if (!files.ok()) {
        return std::unexpected(
            std::format("{}: {} (run `pixi run test`: the fixtures.generate test writes them)",
                        where, files.status().message()));
      }
      for (const auto& f : *files) {
        std::error_code ec;
        if (!fs::is_regular_file(f, ec)) {
          return std::unexpected(
              std::format("{}: fixture '{}' not found (run `pixi run test`: the "
                          "fixtures.generate test writes it)",
                          where, f));
        }
        table.files.push_back(fs::absolute(f, ec).string());
      }
    }
    for (std::size_t i = 2; i < words.size(); ++i) {
      if (words[i] == "clickbench") {
        table.clickbench = true;
      } else {
        return std::unexpected(
            std::format("{}: unknown option '{}' (known: clickbench)", where, words[i]));
      }
    }
    tables.push_back(std::move(table));
  }
  return tables;
}

}  // namespace antb1::slt
