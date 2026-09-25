#include "antb1/plan/types.h"

#include <memory>
#include <string_view>

#include <arrow/api.h>

namespace antb1::plan {

std::string_view ToString(LogicalType type) {
  switch (type) {
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
    case LogicalType::kDouble:
      return "DOUBLE";
    case LogicalType::kVarchar:
      return "VARCHAR";
    case LogicalType::kDate:
      return "DATE";
  }
  return "?";
}

std::shared_ptr<arrow::DataType> ToArrow(LogicalType type) {
  switch (type) {
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
    case LogicalType::kDouble:
      return arrow::float64();
    case LogicalType::kVarchar:
      return arrow::binary();
    case LogicalType::kDate:
      return arrow::date32();
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
    case arrow::Type::DECIMAL128: {
      const auto& dec = static_cast<const arrow::Decimal128Type&>(type);
      if (dec.precision() == 38 && dec.scale() == 0) {
        return LogicalType::kHugeInt;
      }
      break;
    }
    default:
      break;
  }
  return arrow::Status::NotImplemented("unsupported column type ", type.ToString());
}

bool IsInteger(LogicalType type) {
  switch (type) {
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
