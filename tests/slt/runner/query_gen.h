#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "antb1/common/int128.h"

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
// raw file there), SELECT * on a large table always has a LIMIT, and a DECIMAL column is only
// compared with literals of its own digits and with columns of numbers whose common type with it
// has at most 38 digits, which keep DuckDB's common DECIMAL type within 38 digits (divergence D13),
// or with numbers DuckDB types as DOUBLE (an exponent: compared in DOUBLE, never next to an integer
// column, divergence D7); its CASE values never fail to cast to DuckDB's CASE type, and under an
// aggregate keep its scale. DECIMAL SUM, AVG and arithmetic (an integer or decimal literal,
// DuckDB's capped types; / // % too, % beyond 38 digits only as a select item) stay within their
// types over the data, since an overflow fails both engines, and SUM and AVG never add inexact
// doubles a division made, whose sum's rounding follows the order of the additions.
//
// Over tables with refs (GenTable::refs), a query that may use a join feature (kCommaJoin,
// kJoinOn) now and then joins 2 or 3 tables along them (ADR 0022): inner joins only, written as
// commas, CROSS JOIN or [INNER] JOIN ... ON, connected through top-level equalities of the refs'
// keys (never DOUBLE ones, DECIMAL ones only within 38 common digits), in ON or in WHERE. FROM item
// names are distinct (aliases t1 to t3 where needed), every name that two items have is qualified
// by its item's own name, and the join has at most max(10,000, its largest table's rows) rows by
// JoinRowBound. Every draw that only a join needs comes from a second random stream of (s, i), so
// a query that joins nothing is the query the same tables give without refs.

namespace antb1::slt {

enum class ValueKind : std::uint8_t { kInteger, kDouble, kVarchar, kDate, kDecimal };

struct GenColumn {
  std::string name;
  ValueKind kind = ValueKind::kInteger;
  int64_t min = 0;  // kInteger: the range of the storage type (literals probe its edges)
  int64_t max = 0;
  bool via_override = false;  // DATE only through the clickbench option: never with FROM '<path>'
  std::vector<std::string> samples;  // canonical text of values in the data, used as literals
  // kInteger: the range of the values in every file (none if all are NULL): arithmetic stays
  // inside the type, since both engines fail on an overflow and the test compares answers.
  std::optional<int64_t> data_min;
  std::optional<int64_t> data_max;
  int precision = 0;  // kDecimal: DECIMAL(precision, scale)
  int scale = 0;
  // kDecimal: the largest magnitude of an unscaled value in every file (none if all are NULL): SUM
  // and arithmetic stay inside their types, as for data_min and data_max.
  std::optional<Int128> abs_max;
};

// The key of one side of a ref, over every file of its table.
struct GenKeyStats {
  int64_t non_null = 0;          // rows whose key columns are all non-NULL
  int64_t distinct = 0;          // distinct non-NULL keys
  int64_t max_multiplicity = 0;  // rows of the most frequent non-NULL key (0 without one)
  friend bool operator==(const GenKeyStats&, const GenKeyStats&) = default;
};

// A true upper bound of the rows of an inner join on two keys with these statistics (a NULL key
// never matches): min(a.non_null * b.max_multiplicity, b.non_null * a.max_multiplicity,
// min(a.distinct, b.distinct) * a.max_multiplicity * b.max_multiplicity), each product saturating
// at the largest int64_t.
int64_t JoinRowBound(const GenKeyStats& a, const GenKeyStats& b);

// A ref= option of the tables file (runner/tables.h) to a table of the same LoadGenTables call.
struct GenRef {
  std::vector<std::string> columns;      // of this table, as the tables file spells them
  std::string table;                     // the referenced table, as its own line spells it
  std::vector<std::string> ref_columns;  // of that table
  GenKeyStats stats;                     // of `columns` in this table
  GenKeyStats ref_stats;                 // of `ref_columns` in `table`
};

struct GenTable {
  std::string name;
  std::string path;  // the FROM '<path>' form (a file or a glob); empty: by name only
  int64_t rows = 0;
  std::vector<GenColumn> columns;
  bool other_columns = false;  // columns of types the generator skips: no SELECT *
  std::vector<GenRef> refs;    // the refs joins may follow (the generator checks their columns)
};

// Reads the row counts and schema of every table, and sample values from its first file, with the
// Parquet library directly (not through antb1). Keeps a table's refs (TableDef::refs) to the
// tables of this call whose columns on both sides are GenColumns of a key kind (integers, DECIMAL,
// DATE or VARCHAR, never DATE through the clickbench option), with the statistics of both keys over
// every file; refs to other tables or to other columns are dropped. A ref column that names no
// column of its table's first file is an error.
std::expected<std::vector<GenTable>, std::string> LoadGenTables(
    const std::vector<TableDef>& tables);

struct GeneratedQuery {
  uint64_t index = 0;
  std::string sql;
  std::string table;      // the table the query starts from (a join's first table), for messages
  int64_t row_bound = 0;  // at least the rows FROM yields: the table's rows, or JoinRowBound's
  FeatureSet features;    // every feature the SQL uses
  bool target_sample = false;         // drawn from the full target grammar
  SortMode sort = SortMode::kNoSort;  // kRowSort for projections and GROUP BY: SQL has no row order
  // LIMIT or OFFSET on a projection or GROUP BY (no ORDER BY): any rows of the unlimited answer are
  // right, so the answer is compared as a subset of DuckDB's answer without the LIMIT
  // (CompareSubset).
  bool unordered_limit = false;
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
