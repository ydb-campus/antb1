#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/type_fwd.h>

#include "hits_schema.h"

// Deterministic Parquet fixtures for the test harness (docs/testing.md). Values come from
// splitmix64 with integer-only arithmetic (no <random> distributions, no libm; doubles are exact
// integer ratios, never NaN), so the files are identical on every platform that uses the same Arrow
// version.

namespace antb1::fixturegen {

inline constexpr int64_t kHitsRows = 10'000;

// splitmix64 (Steele, Lea, Flood: "Fast splittable pseudorandom number generators", 2014).
class SplitMix64 {
 public:
  static constexpr uint64_t kGamma = 0x9E3779B97F4A7C15ULL;

  explicit constexpr SplitMix64(uint64_t seed) : state_(seed) {}

  constexpr uint64_t Next() {
    state_ += kGamma;
    return Mix(state_);
  }

  static constexpr uint64_t Mix(uint64_t z) {
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
  }

 private:
  uint64_t state_;
};

enum class Nulls : std::uint8_t {
  kNone,
  kSprinkled,  // row 0 entirely NULL, two columns entirely NULL, about 1 in 13 other cells NULL
};

// Rows [first_row, first_row + num_rows) of the hits-like data set. Row r has the same values in
// every variant and every slicing (NULLs aside), so variants and split files compare row for row.
arrow::Result<std::shared_ptr<arrow::Table>> MakeHitsTable(HitsVariant variant, Nulls nulls,
                                                           int64_t first_row, int64_t num_rows);

// A small table of edge values: int16/int32/int64/uint16 extremes, doubles, empty and escaped
// strings, NULLs. Columns: id i16 i32 i64 u16 d s (BYTE_ARRAY) u (UTF8).
arrow::Result<std::shared_ptr<arrow::Table>> MakeEdgeTable();

// FLOAT values where comparing in FLOAT and in DOUBLE differ, as exact bit patterns: 0.1F and its
// neighbour below, 0.2F, 0.3F, +-19.99F, 3.3F, 1234.5F, 2^24 and the next FLOAT, 2^100 and the
// next FLOAT, the FLOAT maximum, +-inf, +-0 and NULL. Columns: id (INTEGER) f (FLOAT).
arrow::Result<std::shared_ptr<arrow::Table>> MakeFloatTable();

// Writes table as Parquet: SNAPPY, row groups of at most row_group_rows rows, no stored
// ARROW:schema. Writes to a temporary file first, then renames it (readers never see partial
// files).
arrow::Status WriteParquet(const arrow::Table& table, const std::filesystem::path& path,
                           int64_t row_group_rows);

struct FixtureFile {
  std::string path;  // relative to the output directory
  int64_t rows = 0;
  int row_groups = 0;
};

// Writes every fixture into dir (created if needed):
//   hits_like.parquet            10,000 rows, 4 row groups, the partitioned hits schema (all
//   OPTIONAL) hits_like_nulls.parquet      the same rows with NULLs sprinkled
//   hits_like_split/part-N.parquet  the same rows split into 4 files (1000/3000/2500/3500 rows)
//   hits_like_required.parquet   the same rows, REQUIRED columns and UTF8 strings (single-file
//   layout) edge.parquet                 MakeEdgeTable() empty.parquet                the
//   partitioned hits schema, 0 rows floats.parquet               MakeFloatTable()
arrow::Result<std::vector<FixtureFile>> WriteAllFixtures(const std::filesystem::path& dir);

// Compares the Parquet schema (column path, physical type, logical type, converted type,
// repetition) of the file at path with the partitioned hits-like schema, column by column.
// Returns one line per difference (empty: the schemas match); IOError if the file cannot be read.
arrow::Result<std::vector<std::string>> CheckHitsSchema(const std::string& path);

}  // namespace antb1::fixturegen
