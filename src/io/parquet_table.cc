#include "antb1/io/parquet_table.h"

#include <cctype>
#include <cstdint>
#include <exception>
#include <format>
#include <memory>
#include <string>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/schema.h>
#include <parquet/exception.h>
#include <parquet/file_reader.h>
#include <parquet/metadata.h>
#include <parquet/properties.h>

#include "antb1/io/glob.h"
#include "antb1/plan/catalog.h"
#include "antb1/plan/types.h"

namespace antb1::io {
namespace {

struct FileInfo {
  std::shared_ptr<arrow::Schema> schema;
  int64_t num_rows = 0;
  int64_t bytes = 0;
};

arrow::Result<FileInfo> ReadFooter(const std::string& path) {
  try {
    ARROW_ASSIGN_OR_RAISE(auto file, arrow::io::ReadableFile::Open(path));
    ARROW_ASSIGN_OR_RAISE(const int64_t size, file->GetSize());
    auto reader = parquet::ParquetFileReader::Open(file);
    const std::shared_ptr<parquet::FileMetaData> metadata = reader->metadata();
    FileInfo info;
    info.num_rows = metadata->num_rows();
    info.bytes = size;
    ARROW_RETURN_NOT_OK(parquet::arrow::FromParquetSchema(
        metadata->schema(), parquet::default_arrow_reader_properties(),
        metadata->key_value_metadata(), &info.schema));
    return info;
  } catch (const parquet::ParquetException& e) {
    return arrow::Status::IOError("cannot read Parquet file '", path, "': ", e.what());
  } catch (const std::exception& e) {
    return arrow::Status::IOError("cannot read '", path, "': ", e.what());
  }
}

std::shared_ptr<arrow::Field> EngineField(const std::shared_ptr<arrow::Field>& storage,
                                          const ParquetTableOptions& options, bool* overridden) {
  const std::string lower = plan::AsciiLower(storage->name());
  for (const auto& ov : options.overrides) {
    if (plan::AsciiLower(ov.column) == lower) {
      *overridden = true;
      return arrow::field(storage->name(), plan::ToArrow(ov.type), storage->nullable());
    }
  }
  auto logical = plan::FromArrow(*storage->type());
  if (!logical.ok()) {
    return storage;  // unsupported: binding a reference fails with the type in the message
  }
  return arrow::field(storage->name(), plan::ToArrow(*logical), storage->nullable());
}

arrow::Status ValidateOverride(const ColumnOverride& ov, const arrow::Schema& storage) {
  const std::string lower = plan::AsciiLower(ov.column);
  for (const auto& f : storage.fields()) {
    if (plan::AsciiLower(f->name()) != lower) {
      continue;
    }
    auto logical = plan::FromArrow(*f->type());
    const bool ok =
        logical.ok() && ov.type == plan::LogicalType::kDate &&
        (*logical == plan::LogicalType::kUSmallInt || *logical == plan::LogicalType::kInteger ||
         *logical == plan::LogicalType::kDate);
    if (!ok) {
      return arrow::Status::Invalid("cannot read column '", f->name(), "' of type ",
                                    f->type()->ToString(), " as ", plan::ToString(ov.type));
    }
    return arrow::Status::OK();
  }
  return arrow::Status::Invalid("column type override: no column named '", ov.column, "'");
}

}  // namespace

arrow::Result<std::shared_ptr<ParquetTable>> ParquetTable::Open(
    const std::vector<std::string>& paths, const ParquetTableOptions& options) {
  if (paths.empty()) {
    return arrow::Status::Invalid("a Parquet table needs at least one file");
  }
  std::shared_ptr<ParquetTable> table(new ParquetTable());
  for (const auto& p : paths) {
    ARROW_ASSIGN_OR_RAISE(auto expanded, ExpandGlob(p));
    table->files_.insert(table->files_.end(), expanded.begin(), expanded.end());
  }
  for (const auto& file : table->files_) {
    ARROW_ASSIGN_OR_RAISE(auto info, ReadFooter(file));
    if (!table->storage_schema_) {
      table->storage_schema_ = info.schema;
    } else if (!table->storage_schema_->Equals(*info.schema, /*check_metadata=*/false)) {
      return arrow::Status::IOError("schema of '", file, "' differs from '", table->files_.front(),
                                    "'");
    }
    table->num_rows_ += info.num_rows;
    table->total_bytes_ += info.bytes;
  }
  for (const auto& ov : options.overrides) {
    ARROW_RETURN_NOT_OK(ValidateOverride(ov, *table->storage_schema_));
  }
  arrow::FieldVector fields;
  for (const auto& f : table->storage_schema_->fields()) {
    bool overridden = false;
    fields.push_back(EngineField(f, options, &overridden));
  }
  table->schema_ = arrow::schema(std::move(fields));
  return table;
}

arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> ParquetTable::Scan(
    const std::vector<int>& /*fields*/, int64_t /*batch_size*/) const {
  return arrow::Status::NotImplemented("Parquet scans are not implemented yet");
}

std::string ParquetTable::Describe() const {
  return std::format("parquet(files={}, rows={})", files_.size(), num_rows_);
}

}  // namespace antb1::io
