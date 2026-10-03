#pragma once

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>

#include <arrow/result.h>
#include <arrow/type_fwd.h>

namespace antb1::plan {

// Engine-level (SQL) type: an id and, for DECIMAL, a width (precision) and a scale (0 for every
// other type). See docs/sql-subset.md for the mapping from Parquet/Arrow.
class LogicalType {
 public:
  enum class Id : std::uint8_t {
    kSmallInt,   // int16
    kInteger,    // int32
    kBigInt,     // int64
    kUSmallInt,  // uint16 (ClickBench EventDate before the DATE override)
    kHugeInt,    // 128-bit integer result of integer SUM: arrow decimal128(38, 0)
    // DECIMAL(p, s), 1 <= p <= 38, 0 <= s <= p: arrow decimal128(p, s), the unscaled value in an
    // Int128 (docs/adr/0021-decimal-semantics.md). Never an integer or numeric type for
    // IsInteger/IsNumeric: every context but the ones ADR 0021 lists for it rejects it.
    kDecimal,
    kDouble,   // float64 (float32 columns are widened)
    kVarchar,  // bytes; compared byte-wise (unannotated Parquet BYTE_ARRAY included)
    kDate,     // date32
    // timestamp[us] without a time zone: only computed (toDateTime, date_trunc), never read from a
    // table (a Parquet timestamp column stays unsupported).
    kTimestamp,
    // A condition's value (bool): only inside a query (a CASE WHEN or a WHERE/HAVING condition that
    // is computed), never a column of a table or of a result.
    kBoolean,
  };
  using enum Id;  // LogicalType::kBigInt keeps working; it is an Id

  constexpr LogicalType() = default;  // SMALLINT, as a value-initialized enum was
  // A type without parameters is written as its id.
  constexpr explicit(false) LogicalType(Id type_id) : id_(type_id) {}

  // DECIMAL(width, scale); the caller checks 1 <= width <= 38 and scale <= width.
  static constexpr LogicalType Decimal(std::uint8_t width, std::uint8_t scale) {
    LogicalType type(kDecimal);
    type.width_ = width;
    type.scale_ = scale;
    return type;
  }
  // The widest DECIMAL precision (and the widest HUGEINT, in digits).
  static constexpr std::uint8_t kMaxDecimalWidth = 38;

  [[nodiscard]] constexpr Id id() const { return id_; }
  [[nodiscard]] constexpr std::uint8_t width() const { return width_; }
  [[nodiscard]] constexpr std::uint8_t scale() const { return scale_; }

  // The same id, width and scale.
  friend constexpr bool operator==(const LogicalType&, const LogicalType&) = default;
  // The same id, whatever the width and scale: `type == LogicalType::kBigInt` asks for the
  // type's kind.
  friend constexpr bool operator==(const LogicalType& type, Id type_id) {
    return type.id_ == type_id;
  }

  // Writes ToString(type); gtest prints types and ids with it.
  friend std::ostream& operator<<(std::ostream& os, LogicalType type);

 private:
  Id id_ = kSmallInt;
  std::uint8_t width_ = 0;
  std::uint8_t scale_ = 0;
};

// The SQL name: INTEGER, DECIMAL(15,2), ... (DuckDB's names, a DECIMAL without a space).
std::string ToString(LogicalType type);

// The Arrow type the engine uses for a logical type (VARCHAR is always arrow::binary()).
std::shared_ptr<arrow::DataType> ToArrow(LogicalType type);

// Maps a storage type to a logical type; NotImplemented (with a clear message) for unsupported
// types.
arrow::Result<LogicalType> FromArrow(const arrow::DataType& type);

bool IsInteger(LogicalType type);
bool IsNumeric(LogicalType type);

}  // namespace antb1::plan
