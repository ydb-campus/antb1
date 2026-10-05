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
//   redact       the table's values must never be printed (data derived from TPC-H, ADR 0006):
//                every subcommand that reads the tables file runs redacted, as with --redact,
//                and `diff --list` and `complete`, which print or write values, are refused.
//                --show-values lifts the redaction for local runs (never on GitHub Actions).
//   ref=<column>[+<column>...]:<table>.<column>[+<column>...]
//                the table's columns reference those of a table of the same file (itself too),
//                pairwise: a foreign key, a hint for generated joins (roadmap PR T1) and not a
//                constraint, so NULL and dangling keys are intended. Repeatable. Names are plain
//                identifiers; the table is found ASCII case-insensitively, also on a later line.
//                LoadTables reads no Parquet file: metamorphic.TablesTxt.RefsJoinColumnsOfOneKind
//                checks the columns. The engines and the query generator ignore refs until T1.

namespace antb1::slt {

// A ref= option: `columns` of the declaring table reference `ref_columns` of `table`, pairwise.
struct ForeignKey {
  std::vector<std::string> columns;      // of the declaring table, as written
  std::string table;                     // the referenced table, as its own line spells it
  std::vector<std::string> ref_columns;  // of that table, as written
};

struct TableDef {
  std::string name;
  std::vector<std::string> files;     // absolute paths (globs expanded)
  std::vector<std::string> patterns;  // absolute paths as written (globs not expanded)
  bool clickbench = false;
  bool redact = false;
  std::vector<ForeignKey> refs;  // the ref= options, in their order
};

std::expected<std::vector<TableDef>, std::string> LoadTables(
    const std::filesystem::path& tables_file, const std::filesystem::path& fixtures_dir);

}  // namespace antb1::slt
