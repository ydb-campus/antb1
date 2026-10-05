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

// The tests that write the fixtures, for the hint of a missing file.
constexpr std::string_view kFixtureTests =
    "the fixtures.generate test writes them, and fixtures.tpch those under tpch/";

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

constexpr std::string_view kRefSyntax =
    "expected ref=<column>[+<column>...]:<table>.<column>[+<column>...]";

bool SameName(std::string_view a, std::string_view b) {
  return plan::AsciiLower(a) == plan::AsciiLower(b);
}

bool SameNames(const std::vector<std::string>& a, const std::vector<std::string>& b) {
  return std::ranges::equal(a, b, SameName);
}

// One side of a ref= option: plain identifiers joined by '+', none of them twice.
std::expected<std::vector<std::string>, std::string> RefColumns(std::string_view text) {
  std::vector<std::string> columns;
  std::size_t begin = 0;
  while (true) {
    const std::size_t end = std::min(text.find('+', begin), text.size());
    const std::string_view column = text.substr(begin, end - begin);
    if (!plan::IsPlainIdentifier(column)) {
      return std::unexpected(std::string(kRefSyntax));
    }
    if (std::ranges::any_of(columns, [&](const std::string& c) { return SameName(c, column); })) {
      return std::unexpected(std::format("column '{}' repeats", column));
    }
    columns.emplace_back(column);
    if (end == text.size()) {
      return columns;
    }
    begin = end + 1;
  }
}

// The value of a ref= option: <column>[+<column>...]:<table>.<column>[+<column>...].
std::expected<ForeignKey, std::string> ParseRef(std::string_view value) {
  const std::size_t colon = value.find(':');
  const std::size_t dot =
      colon == std::string_view::npos ? std::string_view::npos : value.find('.', colon + 1);
  if (dot == std::string_view::npos) {
    return std::unexpected(std::string(kRefSyntax));
  }
  const std::string_view table = value.substr(colon + 1, dot - colon - 1);
  if (!plan::IsPlainIdentifier(table)) {
    return std::unexpected(std::string(kRefSyntax));
  }
  auto columns = RefColumns(value.substr(0, colon));
  if (!columns) {
    return std::unexpected(columns.error());
  }
  auto ref_columns = RefColumns(value.substr(dot + 1));
  if (!ref_columns) {
    return std::unexpected(ref_columns.error());
  }
  if (columns->size() != ref_columns->size()) {
    return std::unexpected(
        std::format("{} column(s) reference {}", columns->size(), ref_columns->size()));
  }
  return ForeignKey{.columns = *std::move(columns),
                    .table = std::string(table),
                    .ref_columns = *std::move(ref_columns)};
}

// Where a ref= option was written: its table resolves once every line is read.
struct RefSite {
  std::size_t table = 0;  // index in the tables read
  std::size_t ref = 0;    // index in that table's refs
  std::string where;      // <file>:<line>
  std::string option;     // as written
};

}  // namespace

std::expected<std::vector<TableDef>, std::string> LoadTables(const fs::path& tables_file,
                                                             const fs::path& fixtures_dir) {
  std::ifstream in(tables_file);
  if (!in) {
    return std::unexpected(std::format("cannot read tables file '{}'", tables_file.string()));
  }
  std::vector<TableDef> tables;
  std::vector<RefSite> sites;
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
    TableDef table{.name = words[0],
                   .files = {},
                   .patterns = {},
                   .clickbench = false,
                   .redact = false,
                   .refs = {}};
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
        return std::unexpected(std::format("{}: {} (run `pixi run test`: {})", where,
                                           files.status().message(), kFixtureTests));
      }
      for (const auto& f : *files) {
        std::error_code ec;
        if (!fs::is_regular_file(f, ec)) {
          return std::unexpected(std::format("{}: fixture '{}' not found (run `pixi run test`: {})",
                                             where, f, kFixtureTests));
        }
        table.files.push_back(fs::absolute(f, ec).string());
      }
    }
    for (std::size_t i = 2; i < words.size(); ++i) {
      const std::string& option = words[i];
      if (option == "clickbench") {
        table.clickbench = true;
      } else if (option == "redact") {
        table.redact = true;
      } else if (option.starts_with("ref=")) {
        auto parsed = ParseRef(std::string_view(option).substr(4));
        if (!parsed) {
          return std::unexpected(
              std::format("{}: ref option '{}': {}", where, option, parsed.error()));
        }
        ForeignKey ref = *std::move(parsed);
        const bool repeated = std::ranges::any_of(table.refs, [&](const ForeignKey& k) {
          return SameNames(k.columns, ref.columns) && SameName(k.table, ref.table) &&
                 SameNames(k.ref_columns, ref.ref_columns);
        });
        if (repeated) {
          return std::unexpected(std::format("{}: duplicate ref option '{}'", where, option));
        }
        sites.push_back(RefSite{
            .table = tables.size(), .ref = table.refs.size(), .where = where, .option = option});
        table.refs.push_back(std::move(ref));
      } else {
        return std::unexpected(std::format(
            "{}: unknown option '{}' (known: clickbench, redact, ref=)", where, option));
      }
    }
    tables.push_back(std::move(table));
  }
  // Every ref names a table of this file, stored as that table's line spells it.
  for (const RefSite& site : sites) {
    ForeignKey& ref = tables[site.table].refs[site.ref];
    const auto target = std::ranges::find_if(
        tables, [&](const TableDef& t) { return SameName(t.name, ref.table); });
    if (target == tables.end()) {
      return std::unexpected(std::format("{}: ref option '{}': no table '{}' in {}", site.where,
                                         site.option, ref.table, tables_file.filename().string()));
    }
    ref.table = target->name;
  }
  return tables;
}

}  // namespace antb1::slt
