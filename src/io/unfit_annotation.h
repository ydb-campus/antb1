#pragma once

#include <memory>
#include <string>
#include <utility>

#include <arrow/array/array_base.h>
#include <arrow/array/data.h>
#include <arrow/extension_type.h>
#include <arrow/result.h>
#include <arrow/type.h>

// Private to io: the engine type of a Parquet column that ParquetTable marks unsupported.

namespace antb1::io {

// The engine type of a column whose Parquet logical type does not fit its physical type (a STRING
// annotation on an INT32 column, say). Arrow 26 drops such an annotation, and the column's
// converted type with it, and reads the physical type (apache/arrow GH-50910), where DuckDB reads
// by the converted type: an INT32 DECIMAL(9,2) would read as its unscaled INTEGERs, a DATE as day
// numbers. The engine view marks such a column unsupported instead, so that a query reading it
// exits 4 and no query answers by values the file does not mean; the file still registers. No query
// reads the column, so the engine never makes an array of this type.
class UnfitAnnotationType final : public arrow::ExtensionType {
 public:
  explicit UnfitAnnotationType(std::shared_ptr<arrow::DataType> storage)
      : arrow::ExtensionType(std::move(storage)) {}

  std::string extension_name() const override { return "antb1.unfit_annotation"; }
  bool ExtensionEquals(const arrow::ExtensionType& other) const override {
    return other.extension_name() == extension_name() &&
           other.storage_type()->Equals(*storage_type());
  }
  std::shared_ptr<arrow::Array> MakeArray(std::shared_ptr<arrow::ArrayData> data) const override {
    return std::make_shared<arrow::ExtensionArray>(std::move(data));
  }
  arrow::Result<std::shared_ptr<arrow::DataType>> Deserialize(
      std::shared_ptr<arrow::DataType> storage, const std::string& /*serialized*/) const override {
    return std::make_shared<UnfitAnnotationType>(std::move(storage));
  }
  std::string Serialize() const override { return {}; }
  // What `antb1 schema` shows, as unsupported(...), and the message of a query that reads it.
  std::string ToString(bool /*show_metadata*/) const override {
    return storage_type()->ToString() + " under a Parquet logical type that does not fit it";
  }
};

}  // namespace antb1::io
