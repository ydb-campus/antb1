#include "antb1/plan/types.h"

#include <cstdint>
#include <format>
#include <memory>
#include <ostream>
#include <string>

#include <arrow/api.h>

#include "antb1/common/narrow.h"

namespace antb1::plan {

std::string ToString(LogicalType type) {
  switch (type.id()) {
    case LogicalType::kSmallInt:
      return "SMALLINT";
    case LogicalType::kInteger:
      return "INTEGER";
    case LogicalType::kBigInt:
      return "BIGINT";
    case LogicalType::kUSmallInt:
      return "USMALLINT";
    case LogicalType::kHugeInt:
      return "HUGEINT";
    case LogicalType::kDecimal:
      return std::format("DECIMAL({},{})", type.width(), type.scale());
    case LogicalType::kDouble:
      return "DOUBLE";
    case LogicalType::kVarchar:
      return "VARCHAR";
    case LogicalType::kDate:
      return "DATE";
    case LogicalType::kTimestamp:
      return "TIMESTAMP";
    case LogicalType::kBoolean:
      return "BOOLEAN";
  }
  return "?";
}

std::ostream& operator<<(std::ostream& os, LogicalType type) { return os << ToString(type); }

std::shared_ptr<arrow::DataType> ToArrow(LogicalType type) {
  switch (type.id()) {
    case LogicalType::kSmallInt:
      return arrow::int16();
    case LogicalType::kInteger:
      return arrow::int32();
    case LogicalType::kBigInt:
      return arrow::int64();
    case LogicalType::kUSmallInt:
      return arrow::uint16();
    case LogicalType::kHugeInt:
      return arrow::decimal128(38, 0);
    case LogicalType::kDecimal:
      return arrow::decimal128(type.width(), type.scale());
    case LogicalType::kDouble:
      return arrow::float64();
    case LogicalType::kVarchar:
      return arrow::binary();
    case LogicalType::kDate:
      return arrow::date32();
    case LogicalType::kTimestamp:
      return arrow::timestamp(arrow::TimeUnit::MICRO);
    case LogicalType::kBoolean:
      return arrow::boolean();
  }
  return nullptr;
}

arrow::Result<LogicalType> FromArrow(const arrow::DataType& type) {
  switch (type.id()) {
    case arrow::Type::INT16:
      return LogicalType::kSmallInt;
    case arrow::Type::INT32:
      return LogicalType::kInteger;
    case arrow::Type::INT64:
      return LogicalType::kBigInt;
    case arrow::Type::UINT16:
      return LogicalType::kUSmallInt;
    case arrow::Type::FLOAT:
    case arrow::Type::DOUBLE:
      return LogicalType::kDouble;
    case arrow::Type::BINARY:
    case arrow::Type::STRING:
    case arrow::Type::LARGE_BINARY:
    case arrow::Type::LARGE_STRING:
      return LogicalType::kVarchar;
    case arrow::Type::DATE32:
      return LogicalType::kDate;
    // Any decimal of at most 38 digits is DECIMAL(p, s), DECIMAL(38, 0) included (ADR 0021 rule
    // 2); io reads decimal32, decimal64 and decimal256 columns as decimal128.
    case arrow::Type::DECIMAL32:
    case arrow::Type::DECIMAL64:
    case arrow::Type::DECIMAL128:
    case arrow::Type::DECIMAL256: {
      const auto& dec = static_cast<const arrow::DecimalType&>(type);
      if (dec.precision() >= 1 && dec.precision() <= LogicalType::kMaxDecimalWidth &&
          dec.scale() >= 0 && dec.scale() <= dec.precision()) {
        return LogicalType::Decimal(Narrow<std::uint8_t>(dec.precision()),
                                    Narrow<std::uint8_t>(dec.scale()));
      }
      break;
    }
    default:
      break;
  }
  return arrow::Status::NotImplemented("unsupported column type ", type.ToString());
}

bool IsInteger(LogicalType type) {
  switch (type.id()) {
    case LogicalType::kSmallInt:
    case LogicalType::kInteger:
    case LogicalType::kBigInt:
    case LogicalType::kUSmallInt:
    case LogicalType::kHugeInt:
      return true;
    default:
      return false;
  }
}

bool IsNumeric(LogicalType type) { return IsInteger(type) || type == LogicalType::kDouble; }

}  // namespace antb1::plan
