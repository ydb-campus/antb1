#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <vector>

// tests/slt/tables.txt: the tables every .slt file can query, identically on both engines.
//   <name> <file>[,<file>...] [option...]
// Files are relative to the fixtures directory; globs (*, ?, [..]) expand to the sorted matches.
// Options:
//   clickbench   EventDate (uint16 days since 1970-01-01) reads as DATE: antb1 --clickbench,
//                DuckDB SELECT * REPLACE (make_date(EventDate) AS EventDate)

namespace antb1::slt {

struct TableDef {
  std::string name;
  std::vector<std::string> files;     // absolute paths (globs expanded)
  std::vector<std::string> patterns;  // absolute paths as written (globs not expanded)
  bool clickbench = false;
};

std::expected<std::vector<TableDef>, std::string> LoadTables(
    const std::filesystem::path& tables_file, const std::filesystem::path& fixtures_dir);

}  // namespace antb1::slt
