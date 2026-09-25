#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include <arrow/type_fwd.h>

// The physical schema of the hits-like fixtures: 105 columns with the names, order and Parquet
// types of the ClickBench *partitioned* files (hits_{0..99}.parquet). This description is our own;
// no ClickBench data is used. It is defined once here and shared by the generator and
// --check-schema.

namespace antb1::fixturegen {

enum class HitsType : std::uint8_t {
  kInt64,   // INT64, no annotation
  kInt32,   // INT32, no annotation
  kInt16,   // INT32 + Int(16, signed)
  kUInt16,  // INT32 + Int(16, unsigned): days since 1970-01-01 (EventDate)
  kBinary,  // BYTE_ARRAY, no annotation (UTF8 in the single-file variant)
};

struct HitsColumn {
  std::string_view name;
  HitsType type;
};

inline constexpr std::size_t kHitsColumnCount = 105;

// The 105 columns in file order.
std::span<const HitsColumn> HitsColumns();

enum class HitsVariant : std::uint8_t {
  kPartitioned,  // every column OPTIONAL, strings unannotated BYTE_ARRAY (like hits_N.parquet)
  kSingleFile,   // every column REQUIRED, strings BYTE_ARRAY + UTF8 (like the single hits.parquet)
};

// The Arrow schema that parquet::arrow writes as the hits-like Parquet schema.
std::shared_ptr<arrow::Schema> HitsArrowSchema(HitsVariant variant);

}  // namespace antb1::fixturegen
