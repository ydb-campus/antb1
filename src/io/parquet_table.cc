#include "antb1/io/parquet_table.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <limits>
#include <memory>
#include <new>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/schema.h>
#include <parquet/exception.h>
#include <parquet/file_reader.h>
#include <parquet/metadata.h>
#include <parquet/properties.h>
#include <parquet/statistics.h>

#include "antb1/common/int128.h"
#include "antb1/common/narrow.h"
#include "antb1/io/glob.h"
#include "antb1/plan/catalog.h"
#include "antb1/plan/types.h"

#include "filtered_scan.h"
#include "scan_segment.h"

namespace antb1::io {
namespace {

struct FileInfo {
  std::shared_ptr<arrow::Schema> schema;
  std::shared_ptr<parquet::FileMetaData> metadata;
  std::shared_ptr<arrow::Buffer> footer;  // as stored (FooterBytes)
  std::vector<int64_t> row_group_rows;
  int64_t num_rows = 0;
  int64_t bytes = 0;
};

// The footer as stored at the end of a Parquet file: the serialized FileMetaData (`metadata`,
// already parsed), its 4-byte length and the magic. It holds the offsets and types of every column
// chunk, so a scan compares it with the file to decode the file only with the footer it was
// opened with.
arrow::Result<std::shared_ptr<arrow::Buffer>> FooterBytes(arrow::io::RandomAccessFile& file,
                                                          int64_t size,
                                                          const parquet::FileMetaData& metadata) {
  const int64_t bytes = int64_t{metadata.size()} + 8;
  return file.ReadAt(size - bytes, bytes);
}

arrow::Result<FileInfo> ReadFooter(const std::string& path) {
  try {
    ARROW_ASSIGN_OR_RAISE(auto file, arrow::io::ReadableFile::Open(path));
    ARROW_ASSIGN_OR_RAISE(const int64_t size, file->GetSize());
    auto reader = parquet::ParquetFileReader::Open(file);
    FileInfo info;
    info.metadata = reader->metadata();
    info.num_rows = info.metadata->num_rows();
    info.bytes = size;
    ARROW_ASSIGN_OR_RAISE(info.footer, FooterBytes(*file, size, *info.metadata));
    info.row_group_rows.reserve(Narrow<std::size_t>(info.metadata->num_row_groups()));
    for (int row_group = 0; row_group < info.metadata->num_row_groups(); ++row_group) {
      info.row_group_rows.push_back(info.metadata->RowGroup(row_group)->num_rows());
    }
    ARROW_RETURN_NOT_OK(parquet::arrow::FromParquetSchema(
        info.metadata->schema(), parquet::default_arrow_reader_properties(),
        info.metadata->key_value_metadata(), &info.schema));
    return info;
  } catch (const parquet::ParquetException& e) {
    return arrow::Status::IOError("cannot read Parquet file '", path, "': ", e.what());
  } catch (const std::exception& e) {
    return arrow::Status::IOError("cannot read '", path, "': ", e.what());
  }
}

std::shared_ptr<arrow::Field> EngineField(const std::shared_ptr<arrow::Field>& storage,
                                          const ParquetTableOptions& options) {
  const std::string lower = plan::AsciiLower(storage->name());
  for (const auto& ov : options.overrides) {
    if (plan::AsciiLower(ov.column) == lower) {
      return arrow::field(storage->name(), plan::ToArrow(ov.type), storage->nullable());
    }
  }
  auto logical = plan::FromArrow(*storage->type());
  if (!logical.ok()) {
    return storage;  // unsupported: binding a reference fails with the type in the message
  }
  return arrow::field(storage->name(), plan::ToArrow(*logical), storage->nullable());
}

// Every column the override matches (EngineField applies it to all of them, e.g. to both "a" and
// "A") must be readable as the new type.
arrow::Status ValidateOverride(const ColumnOverride& ov, const arrow::Schema& storage) {
  const std::string lower = plan::AsciiLower(ov.column);
  bool matched = false;
  for (const auto& f : storage.fields()) {
    if (plan::AsciiLower(f->name()) != lower) {
      continue;
    }
    matched = true;
    auto logical = plan::FromArrow(*f->type());
    const bool ok =
        logical.ok() && ov.type == plan::LogicalType::kDate &&
        (*logical == plan::LogicalType::kUSmallInt || *logical == plan::LogicalType::kInteger ||
         *logical == plan::LogicalType::kDate);
    if (!ok) {
      return arrow::Status::Invalid("cannot read column '", f->name(), "' of type ",
                                    f->type()->ToString(), " as ", plan::ToString(ov.type));
    }
  }
  if (!matched) {
    return arrow::Status::Invalid("column type override: no column named '", ov.column, "'");
  }
  return arrow::Status::OK();
}

// ---- scan: storage arrays -> the engine view (plan::Table::schema()) ----

enum class Conversion : std::uint8_t {
  kNone,
  kView,            // same layout: utf8 -> binary, int32 -> date32
  kLargeToBinary,   // large_utf8 / large_binary -> binary (64-bit to 32-bit offsets)
  kUInt16ToDate32,  // ClickBench EventDate read as DATE
  kFloatToDouble,
  kDecimal32To128,   // a decimal of at most 38 digits that an ARROW:schema restores narrower or
  kDecimal64To128,   // wider than decimal128 (ADR 0021): the engine reads every DECIMAL(p, s) as
  kDecimal256To128,  // decimal128(p, s)
};

struct ConversionRule {
  arrow::Type::type storage;
  arrow::Type::type engine;
  Conversion conversion;
};

// Every storage type that differs from its engine type (plan::ToArrow of the logical type).
constexpr auto kConversionRules = std::to_array<ConversionRule>({
    // VARCHAR is bytes: never validated as UTF-8.
    {.storage = arrow::Type::STRING,
     .engine = arrow::Type::BINARY,
     .conversion = Conversion::kView},
    {.storage = arrow::Type::LARGE_STRING,
     .engine = arrow::Type::BINARY,
     .conversion = Conversion::kLargeToBinary},
    {.storage = arrow::Type::LARGE_BINARY,
     .engine = arrow::Type::BINARY,
     .conversion = Conversion::kLargeToBinary},
    {.storage = arrow::Type::FLOAT,
     .engine = arrow::Type::DOUBLE,
     .conversion = Conversion::kFloatToDouble},
    {.storage = arrow::Type::DECIMAL32,
     .engine = arrow::Type::DECIMAL128,
     .conversion = Conversion::kDecimal32To128},
    {.storage = arrow::Type::DECIMAL64,
     .engine = arrow::Type::DECIMAL128,
     .conversion = Conversion::kDecimal64To128},
    {.storage = arrow::Type::DECIMAL256,
     .engine = arrow::Type::DECIMAL128,
     .conversion = Conversion::kDecimal256To128},
    // Column overrides (--column-type COL=DATE, --clickbench).
    {.storage = arrow::Type::INT32, .engine = arrow::Type::DATE32, .conversion = Conversion::kView},
    {.storage = arrow::Type::UINT16,
     .engine = arrow::Type::DATE32,
     .conversion = Conversion::kUInt16ToDate32},
});

arrow::Result<Conversion> ConversionFor(const arrow::DataType& storage,
                                        const arrow::DataType& engine) {
  if (storage.Equals(engine)) {
    return Conversion::kNone;
  }
  for (const ConversionRule& rule : kConversionRules) {
    if (rule.storage == storage.id() && rule.engine == engine.id()) {
      return rule.conversion;
    }
  }
  return arrow::Status::Invalid("cannot read ", storage.ToString(), " as ", engine.ToString());
}

// Converts every value of a fixed-width array, keeping the validity bitmap and the offset (slots
// before the offset are zero).
template <class In, class Out>
arrow::Result<std::shared_ptr<arrow::Array>> Widen(const arrow::ArrayData& data,
                                                   const std::shared_ptr<arrow::DataType>& type,
                                                   arrow::MemoryPool* pool) {
  const int64_t end = data.offset + data.length;
  ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::Buffer> values,
                        arrow::AllocateBuffer(end * Narrow<int64_t>(sizeof(Out)), pool));
  const std::span<Out> out = values->mutable_span_as<Out>();
  std::ranges::fill(out, Out{});
  // Scans never convert empty batches (they are skipped), so the values buffer exists.
  const std::span<const In> in(data.GetValues<In>(1, 0), Narrow<std::size_t>(end));
  for (auto i = Narrow<std::size_t>(data.offset); i < in.size(); ++i) {
    out[i] = static_cast<Out>(in[i]);
  }
  return arrow::MakeArray(arrow::ArrayData::Make(type, data.length, {data.buffers[0], values},
                                                 data.null_count, data.offset));
}

// decimal256 -> decimal128 of the same precision and scale: a value of at most 38 digits is its low
// 128 bits (two's complement, little-endian words), keeping the validity bitmap and the offset.
arrow::Result<std::shared_ptr<arrow::Array>> Decimal256To128(
    const arrow::ArrayData& data, const std::shared_ptr<arrow::DataType>& type,
    arrow::MemoryPool* pool) {
  constexpr std::size_t kIn = 32;
  constexpr std::size_t kOut = 16;
  const int64_t end = data.offset + data.length;
  ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::Buffer> values,
                        arrow::AllocateBuffer(end * Narrow<int64_t>(kOut), pool));
  const std::span<uint8_t> out = values->mutable_span_as<uint8_t>();
  std::ranges::fill(out, uint8_t{0});
  const std::span<const uint8_t> in(data.GetValues<uint8_t>(1, 0), Narrow<std::size_t>(end) * kIn);
  for (auto i = Narrow<std::size_t>(data.offset); i < Narrow<std::size_t>(end); ++i) {
    std::ranges::copy(in.subspan(i * kIn, kOut), out.subspan(i * kOut, kOut).begin());
  }
  return arrow::MakeArray(arrow::ArrayData::Make(type, data.length, {data.buffers[0], values},
                                                 data.null_count, data.offset));
}

// large_binary (or large_utf8) -> binary: rebases the 64-bit offsets onto the first value and
// narrows them to 32 bits; the value bytes are shared.
arrow::Result<std::shared_ptr<arrow::Array>> LargeToBinary(const arrow::ArrayData& data,
                                                           arrow::MemoryPool* pool) {
  // Scans never convert empty batches (they are skipped), so the offsets buffer has end + 1
  // entries.
  const int64_t end = data.offset + data.length;
  const std::span<const int64_t> offsets(data.GetValues<int64_t>(1, 0),
                                         Narrow<std::size_t>(end + 1));
  const int64_t base = offsets[Narrow<std::size_t>(data.offset)];
  const int64_t bytes = offsets.back() - base;
  if (bytes > std::numeric_limits<int32_t>::max()) {
    return arrow::Status::CapacityError("a batch of a VARCHAR column holds ", bytes,
                                        " bytes, more than 2 GiB; use a smaller batch size");
  }
  ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::Buffer> narrow,
                        arrow::AllocateBuffer((end + 1) * Narrow<int64_t>(sizeof(int32_t)), pool));
  const std::span<int32_t> out = narrow->mutable_span_as<int32_t>();
  std::ranges::fill(out, 0);
  for (auto i = Narrow<std::size_t>(data.offset); i < offsets.size(); ++i) {
    out[i] = Narrow<int32_t>(offsets[i] - base);
  }
  std::shared_ptr<arrow::Buffer> values = data.buffers[2];
  if (values == nullptr) {
    ARROW_ASSIGN_OR_RAISE(values, arrow::AllocateBuffer(0, pool));
  } else {
    values = arrow::SliceBuffer(values, base, bytes);
  }
  return arrow::MakeArray(arrow::ArrayData::Make(arrow::binary(), data.length,
                                                 {data.buffers[0], narrow, values}, data.null_count,
                                                 data.offset));
}

arrow::Result<std::shared_ptr<arrow::Array>> Convert(const std::shared_ptr<arrow::Array>& array,
                                                     Conversion conversion,
                                                     const std::shared_ptr<arrow::DataType>& type,
                                                     arrow::MemoryPool* pool) {
  switch (conversion) {
    case Conversion::kNone:
      return array;
    case Conversion::kView:
      return array->View(type);
    case Conversion::kLargeToBinary:
      return LargeToBinary(*array->data(), pool);
    case Conversion::kUInt16ToDate32:
      return Widen<uint16_t, int32_t>(*array->data(), type, pool);
    case Conversion::kFloatToDouble:
      return Widen<float, double>(*array->data(), type, pool);
    case Conversion::kDecimal32To128:
      return Widen<int32_t, arrow::Decimal128>(*array->data(), type, pool);
    case Conversion::kDecimal64To128:
      return Widen<int64_t, arrow::Decimal128>(*array->data(), type, pool);
    case Conversion::kDecimal256To128:
      return Decimal256To128(*array->data(), type, pool);
  }
  return arrow::Status::Invalid("unknown conversion");
}

// The Parquet leaf columns of a top-level field (a nested field has several).
void CollectLeaves(const parquet::arrow::SchemaField& field, std::vector<int>& leaves) {
  if (field.is_leaf()) {
    leaves.push_back(field.column_index);
    return;
  }
  for (const auto& child : field.children) {
    CollectLeaves(child, leaves);
  }
}

// Reads the projected fields of every segment in order, one file at a time, as batches of the
// engine view. Each segment's reader is opened when the previous segment is exhausted.
class ScanReader final : public arrow::RecordBatchReader {
 public:
  struct Column {
    int field = 0;                              // top-level field index
    Conversion conversion = Conversion::kNone;  // to schema()->field(i)->type()
  };

  ScanReader(std::vector<Segment> segments, std::vector<Column> columns,
             std::shared_ptr<arrow::Schema> schema, int64_t batch_size, arrow::MemoryPool* pool)
      : pool_(pool),
        segments_(std::move(segments)),
        columns_(std::move(columns)),
        schema_(std::move(schema)),
        batch_size_(batch_size) {}

  ScanReader(const ScanReader&) = delete;
  ScanReader& operator=(const ScanReader&) = delete;
  ScanReader(ScanReader&&) = delete;
  ScanReader& operator=(ScanReader&&) = delete;
  ~ScanReader() override = default;

  using arrow::RecordBatchReader::ReadNext;

  [[nodiscard]] std::shared_ptr<arrow::Schema> schema() const override { return schema_; }

  arrow::Status ReadNext(std::shared_ptr<arrow::RecordBatch>* batch) override {
    try {
      return ReadNextBatch(batch);
    } catch (const parquet::ParquetStatusException& e) {
      return FileError(e.status());
    } catch (const std::bad_alloc&) {
      return arrow::Status::OutOfMemory("out of memory reading '", current_file_, "'");
    } catch (const parquet::ParquetException& e) {
      return arrow::Status::IOError("cannot read Parquet file '", current_file_, "': ", e.what());
    } catch (const std::exception& e) {
      return arrow::Status::IOError("cannot read '", current_file_, "': ", e.what());
    }
  }

  arrow::Status Close() override {
    CloseFile();
    next_segment_ = segments_.size();
    return arrow::Status::OK();
  }

 private:
  arrow::Status ReadNextBatch(std::shared_ptr<arrow::RecordBatch>* out) {
    while (true) {
      if (batch_reader_ == nullptr) {
        if (next_segment_ >= segments_.size()) {
          *out = nullptr;
          return arrow::Status::OK();
        }
        ARROW_RETURN_NOT_OK(FileError(OpenFile(segments_[next_segment_++])));
      }
      std::shared_ptr<arrow::RecordBatch> batch;
      ARROW_RETURN_NOT_OK(FileError(batch_reader_->ReadNext(&batch)));
      if (batch == nullptr) {
        CloseFile();
        continue;
      }
      if (batch->num_rows() > 0) {
        ARROW_ASSIGN_OR_RAISE(*out, ToEngine(*batch));
        return arrow::Status::OK();
      }
    }
  }

  arrow::Status OpenFile(const Segment& segment) {
    current_file_ = segment.path;
    // The column chunks are read into the pool too.
    ARROW_ASSIGN_OR_RAISE(auto input, OpenSegmentFile(segment, pool_));
    parquet::arrow::FileReaderBuilder builder;
    ARROW_RETURN_NOT_OK(builder.Open(input, parquet::ReaderProperties(pool_), segment.metadata));
    builder.memory_pool(pool_);
    parquet::ArrowReaderProperties properties(/*use_threads=*/false);
    properties.set_batch_size(batch_size_);
    // No pre-buffering (on by default): its read cache keeps every column chunk it has read until
    // the FileReader closes, so memory would grow with the file (every row group of the scanned
    // columns) instead of one row group, and its reads would run on Arrow's I/O threads. Local
    // files gain nothing from coalescing reads.
    properties.set_pre_buffer(false);
    ARROW_ASSIGN_OR_RAISE(file_reader_, builder.properties(properties)->Build());
    const auto& top_level = file_reader_->manifest().schema_fields;
    std::vector<int> leaves;  // named: a braced list would pick a deprecated overload
    for (const Column& column : columns_) {
      const auto field = Narrow<std::size_t>(column.field);
      CollectLeaves(top_level.at(field), leaves);  // the manifest of the footer read at Open
    }
    ARROW_ASSIGN_OR_RAISE(batch_reader_,
                          file_reader_->GetRecordBatchReader(segment.row_groups, leaves));
    return arrow::Status::OK();
  }

  void CloseFile() {
    batch_reader_.reset();  // before the FileReader it reads from
    file_reader_.reset();
  }

  // Any failure while reading a file is an I/O error naming the file (exit code 3), except running
  // out of the query's memory, which stays OutOfMemory.
  arrow::Status FileError(const arrow::Status& status) const {
    if (status.ok() || status.IsOutOfMemory()) {
      return status;
    }
    return arrow::Status::IOError("cannot read Parquet file '", current_file_,
                                  "': ", status.message());
  }

  arrow::Result<std::shared_ptr<arrow::RecordBatch>> ToEngine(
      const arrow::RecordBatch& batch) const {
    // The footer read at Open fixes the columns and their storage types.
    arrow::ArrayVector arrays;
    arrays.reserve(columns_.size());
    for (std::size_t i = 0; i < columns_.size(); ++i) {
      const Column& column = columns_[i];
      const std::shared_ptr<arrow::Array>& array = batch.column(Narrow<int>(i));
      ARROW_ASSIGN_OR_RAISE(auto converted, Convert(array, column.conversion,
                                                    schema_->field(Narrow<int>(i))->type(), pool_));
      arrays.push_back(std::move(converted));
    }
    return arrow::RecordBatch::Make(schema_, batch.num_rows(), std::move(arrays));
  }

  arrow::MemoryPool* pool_;  // pages, decoded columns and conversions
  std::vector<Segment> segments_;
  std::vector<Column> columns_;
  std::shared_ptr<arrow::Schema> schema_;
  int64_t batch_size_;
  std::size_t next_segment_ = 0;
  std::string current_file_;
  // A FileReader must outlive the RecordBatchReader it creates: declared first, destroyed last.
  std::unique_ptr<parquet::arrow::FileReader> file_reader_;
  std::unique_ptr<arrow::RecordBatchReader> batch_reader_;
};

// A reader of the segments' rows for Scan and ScanPart: validates the request and projects the
// fields from the storage schema onto the engine view.
arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> MakeScan(
    std::vector<Segment> segments, const arrow::Schema& storage_schema, const arrow::Schema& schema,
    const std::vector<int>& fields, int64_t batch_size, arrow::MemoryPool* pool) {
  if (batch_size < 1) {
    return arrow::Status::Invalid("the scan batch size must be positive, not ", batch_size);
  }
  std::vector<ScanReader::Column> columns;
  arrow::FieldVector engine_fields;
  std::vector<bool> seen(Narrow<std::size_t>(schema.num_fields()), false);
  for (const int field : fields) {
    if (field < 0 || field >= schema.num_fields()) {
      return arrow::Status::Invalid("scan of field ", field, " of a table with ",
                                    schema.num_fields(), " fields");
    }
    if (seen[Narrow<std::size_t>(field)]) {
      return arrow::Status::Invalid("field ", field, " is scanned twice");
    }
    seen[Narrow<std::size_t>(field)] = true;
    const auto& storage = storage_schema.field(field)->type();
    const auto& engine = schema.field(field);
    ARROW_ASSIGN_OR_RAISE(const Conversion conversion, ConversionFor(*storage, *engine->type()));
    columns.push_back(ScanReader::Column{.field = field, .conversion = conversion});
    engine_fields.push_back(engine);
  }
  return std::make_unique<ScanReader>(std::move(segments), std::move(columns),
                                      arrow::schema(std::move(engine_fields)), batch_size, pool);
}

// The value of a statistics scalar of a column whose engine type is integer-valued: SMALLINT,
// INTEGER, BIGINT, USMALLINT or DATE (also read from day numbers); std::nullopt for anything else.
std::optional<Int128> IntegerOf(const arrow::Scalar& scalar) {
  switch (scalar.type->id()) {
    case arrow::Type::INT16:
      return Int128{static_cast<const arrow::Int16Scalar&>(scalar).value};
    case arrow::Type::INT32:
      return Int128{static_cast<const arrow::Int32Scalar&>(scalar).value};
    case arrow::Type::INT64:
      return Int128{static_cast<const arrow::Int64Scalar&>(scalar).value};
    case arrow::Type::UINT16:
      return Int128{static_cast<const arrow::UInt16Scalar&>(scalar).value};
    case arrow::Type::DATE32:
      return Int128{static_cast<const arrow::Date32Scalar&>(scalar).value};
    default:
      return std::nullopt;
  }
}

}  // namespace

arrow::Result<std::shared_ptr<arrow::io::ReadableFile>> OpenSegmentFile(const Segment& segment,
                                                                        arrow::MemoryPool* pool) {
  ARROW_ASSIGN_OR_RAISE(auto input, arrow::io::ReadableFile::Open(segment.path, pool));
  ARROW_ASSIGN_OR_RAISE(const int64_t size, input->GetSize());
  if (size != segment.bytes) {
    return arrow::Status::IOError("the file changed after it was opened (", size, " bytes, was ",
                                  segment.bytes, ")");
  }
  ARROW_ASSIGN_OR_RAISE(const std::shared_ptr<arrow::Buffer> footer,
                        input->ReadAt(size - segment.footer->size(), segment.footer->size()));
  if (!footer->Equals(*segment.footer)) {
    return arrow::Status::IOError("the file changed after it was opened (its footer differs)");
  }
  return input;
}

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
    const std::size_t index = table->metadata_.size();
    for (std::size_t row_group = 0; row_group < info.row_group_rows.size(); ++row_group) {
      const int64_t rows = info.row_group_rows[row_group];
      if (rows > 0) {
        table->parts_.push_back(
            Part{.file = index, .row_group = Narrow<int>(row_group), .rows = rows});
      }
    }
    table->metadata_.push_back(std::move(info.metadata));
    table->file_bytes_.push_back(info.bytes);
    table->footers_.push_back(std::move(info.footer));
    table->num_rows_ += info.num_rows;
    table->total_bytes_ += info.bytes;
  }
  for (const auto& ov : options.overrides) {
    ARROW_RETURN_NOT_OK(ValidateOverride(ov, *table->storage_schema_));
  }
  arrow::FieldVector fields;
  for (const auto& f : table->storage_schema_->fields()) {
    fields.push_back(EngineField(f, options));
  }
  table->schema_ = arrow::schema(std::move(fields));
  table->leaf_of_field_.assign(Narrow<std::size_t>(table->schema_->num_fields()), -1);
  if (!table->metadata_.empty()) {
    const auto& first = table->metadata_.front();
    parquet::arrow::SchemaManifest manifest;
    if (parquet::arrow::SchemaManifest::Make(first->schema(), first->key_value_metadata(),
                                             parquet::default_arrow_reader_properties(), &manifest)
            .ok()) {
      for (std::size_t f = 0; f < manifest.schema_fields.size() && f < table->leaf_of_field_.size();
           ++f) {
        if (manifest.schema_fields[f].is_leaf()) {
          table->leaf_of_field_[f] = manifest.schema_fields[f].column_index;
        }
      }
    }
  }
  return table;
}

arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> ParquetTable::DoScan(
    const std::vector<int>& fields, int64_t batch_size, arrow::MemoryPool* pool) const {
  std::vector<Segment> segments;
  segments.reserve(files_.size());
  for (std::size_t file = 0; file < files_.size(); ++file) {
    std::vector<int> row_groups(Narrow<std::size_t>(metadata_[file]->num_row_groups()));
    std::ranges::iota(row_groups, 0);
    segments.push_back(Segment{.path = files_[file],
                               .metadata = metadata_[file],
                               .bytes = file_bytes_[file],
                               .footer = footers_[file],
                               .row_groups = std::move(row_groups)});
  }
  return MakeScan(std::move(segments), *storage_schema_, *schema_, fields, batch_size, pool);
}

int64_t ParquetTable::num_parts() const { return Narrow<int64_t>(parts_.size()); }

std::optional<int64_t> ParquetTable::part_rows(int64_t part) const {
  if (part < 0 || part >= num_parts()) {
    return std::nullopt;
  }
  return parts_[Narrow<std::size_t>(part)].rows;
}

std::optional<plan::PartStats> ParquetTable::part_stats(int64_t part, int field) const {
  if (part < 0 || part >= num_parts() || field < 0 || field >= schema_->num_fields()) {
    return std::nullopt;
  }
  const int leaf = leaf_of_field_[Narrow<std::size_t>(field)];
  // The schema is the engine view, so its types always convert.
  const plan::LogicalType engine =
      plan::FromArrow(*schema_->field(field)->type()).ValueOr(plan::LogicalType::kVarchar);
  // DECIMAL is left out (not an integer type; ADR 0021): a FIXED_LEN_BYTE_ARRAY decimal's
  // statistics some writers got wrong (compared as unsigned bytes) in ways a reader cannot always
  // detect. No column is HUGEINT; the check below keeps it out as well.
  const bool integer_valued = (plan::IsInteger(engine) && engine != plan::LogicalType::kHugeInt) ||
                              engine == plan::LogicalType::kDate;
  if (leaf < 0 || !integer_valued) {
    return std::nullopt;
  }
  const Part& p = parts_[Narrow<std::size_t>(part)];
  try {
    const auto chunk = metadata_[p.file]->RowGroup(p.row_group)->ColumnChunk(leaf);
    const std::shared_ptr<parquet::Statistics> stats = chunk->statistics();
    // statistics() is null when there are none or the writer is known to get them wrong.
    if (stats == nullptr || !stats->HasNullCount()) {
      return std::nullopt;
    }
    plan::PartStats out{.min = std::nullopt,
                        .max = std::nullopt,
                        .null_count = stats->null_count(),
                        .rows = p.rows};
    if (out.null_count >= out.rows) {
      return out;  // every value NULL
    }
    std::shared_ptr<arrow::Scalar> min;
    std::shared_ptr<arrow::Scalar> max;
    if (stats->HasMinMax() && parquet::arrow::StatisticsAsScalars(*stats, &min, &max).ok()) {
      out.min = IntegerOf(*min);
      out.max = IntegerOf(*max);
    }
    // Without both (or with min above max, which no valid file has) the part is never skipped.
    return out.min.has_value() && out.max.has_value() && *out.min <= *out.max ? std::optional(out)
                                                                              : std::nullopt;
  } catch (const std::exception&) {
    return std::nullopt;  // no usable statistics: never skip
  }
}

std::optional<int64_t> ParquetTable::part_distinct_count(int64_t part, int field) const {
  if (part < 0 || part >= num_parts() || field < 0 || field >= schema_->num_fields()) {
    return std::nullopt;
  }
  const int leaf = leaf_of_field_[Narrow<std::size_t>(field)];
  if (leaf < 0) {
    return std::nullopt;  // a nested field: no single column chunk
  }
  const Part& p = parts_[Narrow<std::size_t>(part)];
  try {
    const std::shared_ptr<parquet::Statistics> stats =
        metadata_[p.file]->RowGroup(p.row_group)->ColumnChunk(leaf)->statistics();
    // statistics() is null when there are none or the writer is known to get them wrong.
    if (stats == nullptr || !stats->HasDistinctCount() || stats->distinct_count() < 0) {
      return std::nullopt;
    }
    return stats->distinct_count();
  } catch (const std::exception&) {
    return std::nullopt;  // no usable statistics: no hint
  }
}

arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> ParquetTable::DoScanPart(
    int64_t part, const std::vector<int>& fields, int64_t batch_size,
    arrow::MemoryPool* pool) const {
  if (part < 0 || part >= num_parts()) {
    return arrow::Status::Invalid("scan of part ", part, " of a table with ", num_parts(),
                                  " parts");
  }
  const Part& p = parts_[Narrow<std::size_t>(part)];
  std::vector<Segment> segments;
  segments.push_back(Segment{.path = files_[p.file],
                             .metadata = metadata_[p.file],
                             .bytes = file_bytes_[p.file],
                             .footer = footers_[p.file],
                             .row_groups = std::vector<int>{p.row_group}});
  return MakeScan(std::move(segments), *storage_schema_, *schema_, fields, batch_size, pool);
}

bool ParquetTable::supports_scan_filter(const std::vector<int>& fields) const {
  if (metadata_.empty()) {
    return false;
  }
  return std::ranges::all_of(fields, [&](int field) {
    return field >= 0 && field < schema_->num_fields() &&
           FilteredColumnOf(*metadata_.front(), leaf_of_field_[Narrow<std::size_t>(field)],
                            *storage_schema_->field(field)->type(), *schema_->field(field)->type())
               .has_value();
  });
}

arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> ParquetTable::DoScanPartFiltered(
    int64_t part, const std::vector<int>& fields, int64_t batch_size, arrow::MemoryPool* pool,
    const std::shared_ptr<const plan::ScanFilter>& filter, bool positions) const {
  if (part < 0 || part >= num_parts()) {
    return arrow::Status::Invalid("scan of part ", part, " of a table with ", num_parts(),
                                  " parts");
  }
  const Part& p = parts_[Narrow<std::size_t>(part)];
  std::vector<FilteredColumn> columns;
  arrow::FieldVector engine_fields;
  std::vector<bool> seen(Narrow<std::size_t>(schema_->num_fields()), false);
  for (const int field : fields) {
    if (field < 0 || field >= schema_->num_fields() || seen[Narrow<std::size_t>(field)]) {
      return arrow::Status::Invalid("invalid or repeated field ", field, " in a filtered scan");
    }
    seen[Narrow<std::size_t>(field)] = true;
    // Every file has the schema of the first (checked at Open), and so the same leaves.
    std::optional<FilteredColumn> column =
        FilteredColumnOf(*metadata_[p.file], leaf_of_field_[Narrow<std::size_t>(field)],
                         *storage_schema_->field(field)->type(), *schema_->field(field)->type());
    if (!column.has_value()) {
      return arrow::Status::NotImplemented("a filtered scan cannot read field ", field);
    }
    columns.push_back(*std::move(column));
    engine_fields.push_back(schema_->field(field));
  }
  return MakeFilteredScan(Segment{.path = files_[p.file],
                                  .metadata = metadata_[p.file],
                                  .bytes = file_bytes_[p.file],
                                  .footer = footers_[p.file],
                                  .row_groups = std::vector<int>{p.row_group}},
                          std::move(columns), arrow::schema(std::move(engine_fields)), batch_size,
                          pool, filter, positions);
}

bool ParquetTable::StoredAsFloat(int field) const {
  return field >= 0 && field < storage_schema_->num_fields() &&
         storage_schema_->field(field)->type()->id() == arrow::Type::FLOAT;
}

std::string ParquetTable::Describe() const {
  return std::format("parquet(files={}, rows={})", files_.size(), num_rows_);
}

}  // namespace antb1::io
