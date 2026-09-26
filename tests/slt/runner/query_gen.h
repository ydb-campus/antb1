#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <utility>
#include <vector>

#include "canonical.h"
#include "supported_features.h"
#include "tables.h"

// Random queries for the differential test (`antb1-slt diff`, tests/slt/README.md).
//
// Query number i of seed s is a pure function of (s, i, the tables, the supported features): it
// never depends on the other queries, so `--only i` reproduces one case alone. Most queries use
// only kSupportedFeatures; `target_percent` of them sample the full target grammar of
// docs/sql-subset.md instead, to count the answers antb1 does not support yet.
//
// Every query is valid SQL for DuckDB with the semantics antb1 targets: literals match the column
// type (no implicit casts), doubles only get literals that both engines convert to the same double,
// FLOAT columns (results are DOUBLE on antb1 only, divergence D11) are never referenced, columns
// read as DATE through the clickbench option are never used with FROM '<path>' (DuckDB reads the
// raw file there), and SELECT * on a large table always has a LIMIT.

namespace antb1::slt {

enum class ValueKind : std::uint8_t { kInteger, kDouble, kVarchar, kDate };

struct GenColumn {
  std::string name;
  ValueKind kind = ValueKind::kInteger;
  int64_t min = 0;  // kInteger: the range of the storage type (literals probe its edges)
  int64_t max = 0;
  bool via_override = false;  // DATE only through the clickbench option: never with FROM '<path>'
  std::vector<std::string> samples;  // canonical text of values in the data, used as literals
};

struct GenTable {
  std::string name;
  std::string path;  // the FROM '<path>' form (a file or a glob); empty: by name only
  int64_t rows = 0;
  std::vector<GenColumn> columns;
  bool other_columns = false;  // columns of types the generator skips: no SELECT *
};

// Reads the row counts and schema of every table, and sample values from its first file, with the
// Parquet library directly (not through antb1).
std::expected<std::vector<GenTable>, std::string> LoadGenTables(
    const std::vector<TableDef>& tables);

struct GeneratedQuery {
  uint64_t index = 0;
  std::string sql;
  std::string table;                  // the table the query reads, for messages
  FeatureSet features;                // every feature the SQL uses
  bool target_sample = false;         // drawn from the full target grammar
  SortMode sort = SortMode::kNoSort;  // kRowSort for projections: SQL does not order rows
  bool row_count_only = false;  // a projection with LIMIT: any n rows are right, compare counts
};

struct GeneratorOptions {
  FeatureSet supported = kSupportedFeatures;
  unsigned target_percent = 25;  // share of queries drawn from the full target grammar
};

class QueryGenerator {
 public:
  // Fails if no query can be built from `options.supported` over `tables`.
  static std::expected<QueryGenerator, std::string> Make(std::vector<GenTable> tables,
                                                         uint64_t seed, GeneratorOptions options);

  [[nodiscard]] GeneratedQuery Generate(uint64_t index) const;

  [[nodiscard]] uint64_t seed() const { return seed_; }
  [[nodiscard]] const GeneratorOptions& options() const { return options_; }

 private:
  QueryGenerator(std::vector<GenTable> tables, uint64_t seed, GeneratorOptions options)
      : tables_(std::move(tables)), seed_(seed), options_(options) {}

  std::vector<GenTable> tables_;
  uint64_t seed_;
  GeneratorOptions options_;
};

}  // namespace antb1::slt
