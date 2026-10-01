#include "filtered_scan.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/util/bit_util.h>
#include <parquet/column_reader.h>
#include <parquet/exception.h>
#include <parquet/file_reader.h>
#include <parquet/properties.h>
#include <parquet/schema.h>
#include <parquet/types.h>

#include "antb1/common/narrow.h"

namespace antb1::io {
namespace {

using Physical = FilteredColumn::Physical;

bool BitAt(const std::uint8_t* bits, int64_t i) {
  return arrow::bit_util::GetBit(bits, static_cast<std::uint64_t>(i));
}
void SetBitAt(std::uint8_t* bits, int64_t i) { arrow::bit_util::SetBit(bits, i); }

// The bytes of one value of a fixed-width engine type.
int WidthOf(const arrow::DataType& type) { return type.bit_width() / 8; }

// A batch's rows of one column, decoded: definition levels (when the column has NULLs) and the
// values of its non-NULL rows, densely.
template <class Value>
struct Decoded {
  std::vector<int16_t> levels;
  std::vector<Value> values;
  int64_t rows = 0;
  int64_t values_read = 0;
};

// Reads `rows` rows of a column (a flat leaf with max definition level `max_def`).
template <class Reader, class Value>
arrow::Status ReadRows(Reader& reader, int16_t max_def, int64_t rows, Decoded<Value>& out) {
  out.levels.resize(static_cast<std::size_t>(rows));
  out.values.resize(static_cast<std::size_t>(rows));
  out.rows = 0;
  out.values_read = 0;
  while (out.rows < rows) {
    int64_t values = 0;
    const int64_t levels =
        reader.ReadBatch(rows - out.rows, max_def > 0 ? out.levels.data() + out.rows : nullptr,
                         nullptr, out.values.data() + out.values_read, &values);
    if (levels <= 0) {
      return arrow::Status::IOError("a column chunk holds fewer rows than its row group");
    }
    out.rows += levels;
    out.values_read += values;
  }
  return arrow::Status::OK();
}

// Skips `rows` rows of a column.
template <class Reader>
arrow::Status SkipRows(Reader& reader, int64_t rows) {
  while (rows > 0) {
    const int64_t skipped = reader.Skip(rows);
    if (skipped <= 0) {
      return arrow::Status::IOError("a column chunk holds fewer rows than its row group");
    }
    rows -= skipped;
  }
  return arrow::Status::OK();
}

// The engine array of `decoded` (every row, NULLs where the levels say so); with `selected`, only
// the rows whose bit is set (`count` of them).
template <class Value>
arrow::Result<std::shared_ptr<arrow::Array>> FixedArray(
    const Decoded<Value>& decoded, int16_t max_def, const std::shared_ptr<arrow::DataType>& type,
    const std::uint8_t* selected, int64_t count, arrow::MemoryPool* pool) {
  const int64_t length = selected == nullptr ? decoded.rows : count;
  const int width = WidthOf(*type);
  ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::Buffer> values,
                        arrow::AllocateBuffer(length * width, pool));
  std::uint8_t* out = values->mutable_data();
  std::shared_ptr<arrow::Buffer> validity;
  std::uint8_t* valid = nullptr;
  int64_t nulls = 0;
  if (max_def > 0) {
    ARROW_ASSIGN_OR_RAISE(validity, arrow::AllocateEmptyBitmap(length, pool));
    valid = validity->mutable_data();
  }
  const auto store = [&](int64_t at, const Value* v) {
    std::uint8_t* p = out + (at * width);
    if (v == nullptr) {
      std::memset(p, 0, static_cast<std::size_t>(width));
      return;
    }
    switch (type->id()) {
      case arrow::Type::INT16: {
        const auto x = static_cast<int16_t>(*v);
        std::memcpy(p, &x, sizeof(x));
        break;
      }
      case arrow::Type::UINT16: {
        const auto x = static_cast<uint16_t>(*v);
        std::memcpy(p, &x, sizeof(x));
        break;
      }
      case arrow::Type::DOUBLE: {
        const auto x = static_cast<double>(*v);
        std::memcpy(p, &x, sizeof(x));
        break;
      }
      default:  // INT32, DATE32, INT64: the stored value as it is
        std::memcpy(p, v, sizeof(Value));
        break;
    }
  };
  int64_t at = 0;
  int64_t next_value = 0;
  for (int64_t row = 0; row < decoded.rows; ++row) {
    const bool present = max_def == 0 || decoded.levels[static_cast<std::size_t>(row)] == max_def;
    const Value* v = present ? &decoded.values[static_cast<std::size_t>(next_value)] : nullptr;
    if (present) {
      ++next_value;
    }
    if (selected != nullptr && !BitAt(selected, row)) {
      continue;
    }
    store(at, v);
    if (valid != nullptr) {
      if (present) {
        SetBitAt(valid, at);
      } else {
        ++nulls;
      }
    }
    ++at;
  }
  if (nulls == 0) {
    validity = nullptr;
  }
  return arrow::MakeArray(arrow::ArrayData::Make(type, length, {validity, values}, nulls));
}

// A column being read: its reader, as the right typed reader.
struct ColumnState {
  FilteredColumn column;
  int16_t max_def = 0;
  std::shared_ptr<parquet::ColumnReader> reader;
};

class FilteredScanReader final : public arrow::RecordBatchReader {
 public:
  FilteredScanReader(Segment segment, std::vector<FilteredColumn> columns,
                     std::shared_ptr<arrow::Schema> schema, int64_t batch_size,
                     arrow::MemoryPool* pool, std::shared_ptr<const plan::ScanFilter> filter)
      : segment_(std::move(segment)),
        schema_(std::move(schema)),
        batch_size_(batch_size),
        pool_(pool),
        filter_(std::move(filter)) {
    for (FilteredColumn& column : columns) {
      columns_.push_back(ColumnState{.column = std::move(column), .max_def = 0, .reader = nullptr});
    }
    filter_index_.assign(columns_.size(), -1);
    const std::vector<int>& filtered = filter_->columns();
    for (std::size_t i = 0; i < filtered.size(); ++i) {
      filter_index_[static_cast<std::size_t>(filtered[i])] = static_cast<int>(i);
    }
  }

  FilteredScanReader(const FilteredScanReader&) = delete;
  FilteredScanReader& operator=(const FilteredScanReader&) = delete;
  FilteredScanReader(FilteredScanReader&&) = delete;
  FilteredScanReader& operator=(FilteredScanReader&&) = delete;
  ~FilteredScanReader() override = default;

  using arrow::RecordBatchReader::ReadNext;

  [[nodiscard]] std::shared_ptr<arrow::Schema> schema() const override { return schema_; }

  arrow::Status ReadNext(std::shared_ptr<arrow::RecordBatch>* batch) override {
    try {
      return FileError(ReadNextBatch(batch));
    } catch (const parquet::ParquetStatusException& e) {
      if (e.status().IsOutOfMemory()) {
        return e.status();
      }
      return arrow::Status::IOError("cannot read Parquet file '", segment_.path,
                                    "': ", e.status().message());
    } catch (const std::bad_alloc&) {
      return arrow::Status::OutOfMemory("out of memory reading '", segment_.path, "'");
    } catch (const parquet::ParquetException& e) {
      return arrow::Status::IOError("cannot read Parquet file '", segment_.path, "': ", e.what());
    } catch (const std::exception& e) {
      return arrow::Status::IOError("cannot read '", segment_.path, "': ", e.what());
    }
  }

  arrow::Status Close() override {
    for (ColumnState& state : columns_) {
      state.reader.reset();
    }
    row_group_.reset();
    file_reader_.reset();
    rows_left_ = 0;
    closed_ = true;
    return arrow::Status::OK();
  }

 private:
  arrow::Status Open() {
    ARROW_ASSIGN_OR_RAISE(auto input, OpenSegmentFile(segment_, pool_));
    file_reader_ = parquet::ParquetFileReader::Open(input, parquet::ReaderProperties(pool_),
                                                    segment_.metadata);
    const int group = segment_.row_groups.at(0);
    row_group_ = file_reader_->RowGroup(group);
    rows_left_ = segment_.metadata->RowGroup(group)->num_rows();
    for (ColumnState& state : columns_) {
      state.reader = row_group_->Column(state.column.leaf);
      state.max_def =
          segment_.metadata->schema()->Column(state.column.leaf)->max_definition_level();
    }
    opened_ = true;
    return arrow::Status::OK();
  }

  arrow::Status ReadNextBatch(std::shared_ptr<arrow::RecordBatch>* out) {
    *out = nullptr;
    if (closed_) {
      return arrow::Status::OK();
    }
    if (!opened_) {
      ARROW_RETURN_NOT_OK(Open());
    }
    while (rows_left_ > 0) {
      const int64_t rows = std::min(batch_size_, rows_left_);
      rows_left_ -= rows;
      ARROW_ASSIGN_OR_RAISE(*out, ReadBatch(rows));
      if (*out != nullptr) {
        return arrow::Status::OK();
      }
    }
    return arrow::Status::OK();
  }

  // The next `rows` rows: the filter's columns first, then the rows that pass of every column;
  // nullptr when none passes.
  arrow::Result<std::shared_ptr<arrow::RecordBatch>> ReadBatch(int64_t rows) {
    ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::Buffer> selection,
                          arrow::AllocateBitmap(rows, pool_));
    std::uint8_t* selected = selection->mutable_data();
    arrow::bit_util::SetBitsTo(selected, 0, rows, true);
    std::vector<std::shared_ptr<arrow::Array>> arrays(columns_.size());
    // The filter's columns: fixed-width ones whole, strings piece by piece (the views of a piece
    // live until the next read), keeping the values of the rows still selected.
    std::vector<std::unique_ptr<Candidates>> candidates(columns_.size());
    std::vector<std::shared_ptr<arrow::Array>> whole(columns_.size());
    for (const int position : filter_->columns()) {
      const auto p = static_cast<std::size_t>(position);
      ColumnState& state = columns_[p];
      const int index = filter_index_[p];
      if (state.column.physical == Physical::kByteArray) {
        candidates[p] = std::make_unique<Candidates>();
        ARROW_RETURN_NOT_OK(FilterStrings(state, index, rows, selected, *candidates[p]));
      } else {
        ARROW_ASSIGN_OR_RAISE(whole[p], ReadFixed(state, rows, nullptr, 0));
        ARROW_RETURN_NOT_OK(filter_->Apply(
            index,
            plan::ScanValues{.array = whole[p], .strings = {}, .validity = nullptr, .rows = rows},
            0, selected));
      }
    }
    const int64_t count = arrow::internal::CountSetBits(selected, 0, rows);
    for (std::size_t p = 0; p < columns_.size(); ++p) {
      ColumnState& state = columns_[p];
      if (filter_index_[p] >= 0) {
        if (count == 0) {
          continue;
        }
        if (state.column.physical == Physical::kByteArray) {
          ARROW_ASSIGN_OR_RAISE(arrays[p], candidates[p]->Finish(selected, count, pool_));
        } else {
          ARROW_ASSIGN_OR_RAISE(arrays[p], Compact(*whole[p], state, selected, count));
        }
        continue;
      }
      if (count == 0) {
        ARROW_RETURN_NOT_OK(Skip(state, rows));
        continue;
      }
      if (state.column.physical == Physical::kByteArray) {
        ARROW_ASSIGN_OR_RAISE(arrays[p], ReadStrings(state, rows, selected, count));
      } else {
        ARROW_ASSIGN_OR_RAISE(arrays[p], ReadFixed(state, rows, selected, count));
      }
    }
    if (count == 0) {
      return nullptr;
    }
    return arrow::RecordBatch::Make(schema_, count, std::move(arrays));
  }

  // The values a filter's string column kept for the rows still selected after its own
  // predicates: the rows (in order) and their values.
  struct Candidates {
    std::vector<int64_t> rows;
    std::vector<int64_t> offsets{0};
    std::vector<char> bytes;
    std::vector<bool> nulls;

    void Add(int64_t row, const std::string_view* value) {
      rows.push_back(row);
      nulls.push_back(value == nullptr);
      if (value != nullptr) {
        bytes.insert(bytes.end(), value->begin(), value->end());
      }
      offsets.push_back(static_cast<int64_t>(bytes.size()));
    }

    // The values of the rows selected in the end.
    arrow::Result<std::shared_ptr<arrow::Array>> Finish(const std::uint8_t* selected, int64_t count,
                                                        arrow::MemoryPool* pool) const {
      arrow::BinaryBuilder builder(pool);
      ARROW_RETURN_NOT_OK(builder.Reserve(count));
      for (std::size_t i = 0; i < rows.size(); ++i) {
        if (!BitAt(selected, rows[i])) {
          continue;
        }
        if (nulls[i]) {
          ARROW_RETURN_NOT_OK(builder.AppendNull());
        } else {
          ARROW_RETURN_NOT_OK(builder.Append(bytes.data() + offsets[i],
                                             Narrow<int32_t>(offsets[i + 1] - offsets[i])));
        }
      }
      return builder.Finish();
    }
  };

  // Reads `rows` rows of a string column piece by piece, applies the filter's predicates of the
  // column to each piece and keeps the values of the rows still selected.
  arrow::Status FilterStrings(ColumnState& state, int index, int64_t rows, std::uint8_t* selected,
                              Candidates& kept) {
    auto& reader = static_cast<parquet::ByteArrayReader&>(*state.reader);
    int64_t done = 0;
    while (done < rows) {
      ARROW_RETURN_NOT_OK(ReadStringPiece(reader, state.max_def, rows - done));
      const auto piece = static_cast<int64_t>(views_.size());
      ARROW_RETURN_NOT_OK(
          filter_->Apply(index,
                         plan::ScanValues{.array = nullptr,
                                          .strings = std::span<const std::string_view>(views_),
                                          .validity = piece_nulls_ ? piece_valid_.data() : nullptr,
                                          .rows = piece},
                         done, selected));
      for (int64_t i = 0; i < piece; ++i) {
        if (BitAt(selected, done + i)) {
          const bool present = !piece_nulls_ || BitAt(piece_valid_.data(), i);
          kept.Add(done + i, present ? &views_[static_cast<std::size_t>(i)] : nullptr);
        }
      }
      done += piece;
    }
    return arrow::Status::OK();
  }

  // Reads the selected rows of `rows` rows of a string column (not one of the filter's).
  arrow::Result<std::shared_ptr<arrow::Array>> ReadStrings(ColumnState& state, int64_t rows,
                                                           const std::uint8_t* selected,
                                                           int64_t count) {
    auto& reader = static_cast<parquet::ByteArrayReader&>(*state.reader);
    arrow::BinaryBuilder builder(pool_);
    ARROW_RETURN_NOT_OK(builder.Reserve(count));
    int64_t done = 0;
    while (done < rows) {
      ARROW_RETURN_NOT_OK(ReadStringPiece(reader, state.max_def, rows - done));
      const auto piece = static_cast<int64_t>(views_.size());
      for (int64_t i = 0; i < piece; ++i) {
        if (!BitAt(selected, done + i)) {
          continue;
        }
        if (piece_nulls_ && !BitAt(piece_valid_.data(), i)) {
          ARROW_RETURN_NOT_OK(builder.AppendNull());
        } else {
          const std::string_view v = views_[static_cast<std::size_t>(i)];
          ARROW_RETURN_NOT_OK(builder.Append(v.data(), Narrow<int32_t>(v.size())));
        }
      }
      done += piece;
    }
    return builder.Finish();
  }

  // Reads the next piece (at most `rows` rows, within one page) of a string column into views_,
  // one view per row, with piece_valid_ when some row is NULL.
  arrow::Status ReadStringPiece(parquet::ByteArrayReader& reader, int16_t max_def, int64_t rows) {
    levels_.resize(static_cast<std::size_t>(rows));
    values_.resize(static_cast<std::size_t>(rows));
    int64_t values_read = 0;
    const int64_t levels = reader.ReadBatch(rows, max_def > 0 ? levels_.data() : nullptr, nullptr,
                                            values_.data(), &values_read);
    if (levels <= 0) {
      return arrow::Status::IOError("a column chunk holds fewer rows than its row group");
    }
    views_.resize(static_cast<std::size_t>(levels));
    piece_nulls_ = values_read < levels;
    if (piece_nulls_) {
      piece_valid_.assign(static_cast<std::size_t>(arrow::bit_util::BytesForBits(levels)), 0);
    }
    int64_t next = 0;
    for (int64_t i = 0; i < levels; ++i) {
      if (!piece_nulls_ || levels_[static_cast<std::size_t>(i)] == max_def) {
        const parquet::ByteArray& v = values_[static_cast<std::size_t>(next++)];
        views_[static_cast<std::size_t>(i)] =
            std::string_view(reinterpret_cast<const char*>(v.ptr), v.len);
        if (piece_nulls_) {
          SetBitAt(piece_valid_.data(), i);
        }
      } else {
        views_[static_cast<std::size_t>(i)] = std::string_view();
      }
    }
    return arrow::Status::OK();
  }

  // Reads `rows` rows of a fixed-width column as its engine array (with `selected`: only those).
  arrow::Result<std::shared_ptr<arrow::Array>> ReadFixed(ColumnState& state, int64_t rows,
                                                         const std::uint8_t* selected,
                                                         int64_t count) {
    const auto& type = state.column.engine;
    switch (state.column.physical) {
      case Physical::kInt32: {
        Decoded<int32_t> d;
        ARROW_RETURN_NOT_OK(
            ReadRows(static_cast<parquet::Int32Reader&>(*state.reader), state.max_def, rows, d));
        return FixedArray(d, state.max_def, type, selected, count, pool_);
      }
      case Physical::kInt64: {
        Decoded<int64_t> d;
        ARROW_RETURN_NOT_OK(
            ReadRows(static_cast<parquet::Int64Reader&>(*state.reader), state.max_def, rows, d));
        return FixedArray(d, state.max_def, type, selected, count, pool_);
      }
      case Physical::kFloat: {
        Decoded<float> d;
        ARROW_RETURN_NOT_OK(
            ReadRows(static_cast<parquet::FloatReader&>(*state.reader), state.max_def, rows, d));
        return FixedArray(d, state.max_def, type, selected, count, pool_);
      }
      case Physical::kDouble: {
        Decoded<double> d;
        ARROW_RETURN_NOT_OK(
            ReadRows(static_cast<parquet::DoubleReader&>(*state.reader), state.max_def, rows, d));
        return FixedArray(d, state.max_def, type, selected, count, pool_);
      }
      case Physical::kByteArray:
        break;
    }
    return arrow::Status::Invalid("not a fixed-width column");
  }

  // The selected rows of a fixed-width engine array.
  arrow::Result<std::shared_ptr<arrow::Array>> Compact(const arrow::Array& array,
                                                       const ColumnState& state,
                                                       const std::uint8_t* selected,
                                                       int64_t count) const {
    const int width = WidthOf(*state.column.engine);
    ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::Buffer> values,
                          arrow::AllocateBuffer(count * width, pool_));
    std::shared_ptr<arrow::Buffer> validity;
    if (array.null_count() > 0) {
      ARROW_ASSIGN_OR_RAISE(validity, arrow::AllocateEmptyBitmap(count, pool_));
    }
    const std::uint8_t* in = array.data()->buffers[1]->data();
    int64_t at = 0;
    int64_t nulls = 0;
    for (int64_t row = 0; row < array.length(); ++row) {
      if (!BitAt(selected, row)) {
        continue;
      }
      std::memcpy(values->mutable_data() + (at * width), in + (row * width),
                  static_cast<std::size_t>(width));
      if (validity != nullptr) {
        if (array.IsValid(row)) {
          SetBitAt(validity->mutable_data(), at);
        } else {
          ++nulls;
        }
      }
      ++at;
    }
    if (nulls == 0) {
      validity = nullptr;
    }
    return arrow::MakeArray(
        arrow::ArrayData::Make(state.column.engine, count, {validity, values}, nulls));
  }

  arrow::Status Skip(ColumnState& state, int64_t rows) {
    switch (state.column.physical) {
      case Physical::kInt32:
        return SkipRows(static_cast<parquet::Int32Reader&>(*state.reader), rows);
      case Physical::kInt64:
        return SkipRows(static_cast<parquet::Int64Reader&>(*state.reader), rows);
      case Physical::kFloat:
        return SkipRows(static_cast<parquet::FloatReader&>(*state.reader), rows);
      case Physical::kDouble:
        return SkipRows(static_cast<parquet::DoubleReader&>(*state.reader), rows);
      case Physical::kByteArray:
        return SkipRows(static_cast<parquet::ByteArrayReader&>(*state.reader), rows);
    }
    return arrow::Status::Invalid("unknown column");
  }

  // A failure while reading the file is an I/O error naming it (exit code 3), except running out
  // of the query's memory; the filter's own errors pass through unchanged.
  arrow::Status FileError(const arrow::Status& status) const {
    if (!status.IsIOError() || status.message().find(segment_.path) != std::string::npos) {
      return status;
    }
    return arrow::Status::IOError("cannot read Parquet file '", segment_.path,
                                  "': ", status.message());
  }

  Segment segment_;
  std::vector<ColumnState> columns_;
  std::shared_ptr<arrow::Schema> schema_;
  int64_t batch_size_;
  arrow::MemoryPool* pool_;
  std::shared_ptr<const plan::ScanFilter> filter_;
  std::vector<int> filter_index_;  // per column: its position in filter_->columns(), or -1
  // The reader of the file outlives its row group's and columns' readers.
  std::unique_ptr<parquet::ParquetFileReader> file_reader_;
  std::shared_ptr<parquet::RowGroupReader> row_group_;
  int64_t rows_left_ = 0;
  bool opened_ = false;
  bool closed_ = false;
  // The current piece of a string column.
  std::vector<int16_t> levels_;
  std::vector<parquet::ByteArray> values_;
  std::vector<std::string_view> views_;
  std::vector<std::uint8_t> piece_valid_;
  bool piece_nulls_ = false;
};

}  // namespace

std::optional<FilteredColumn> FilteredColumnOf(const parquet::FileMetaData& metadata, int leaf,
                                               const arrow::DataType& storage,
                                               const arrow::DataType& engine) {
  if (leaf < 0 || leaf >= metadata.num_columns()) {
    return std::nullopt;
  }
  const parquet::ColumnDescriptor* descr = metadata.schema()->Column(leaf);
  if (descr->max_repetition_level() != 0 || descr->max_definition_level() > 1) {
    return std::nullopt;
  }
  const parquet::Type::type physical = descr->physical_type();
  const auto of = [&](Physical p) {
    return FilteredColumn{.leaf = leaf, .physical = p, .engine = engine.GetSharedPtr()};
  };
  const arrow::Type::type s = storage.id();
  switch (engine.id()) {
    case arrow::Type::INT16:
    case arrow::Type::INT32:
    case arrow::Type::UINT16:
      if (s == engine.id() && physical == parquet::Type::INT32) {
        return of(Physical::kInt32);
      }
      break;
    case arrow::Type::DATE32:
      if ((s == arrow::Type::DATE32 || s == arrow::Type::INT32 || s == arrow::Type::UINT16) &&
          physical == parquet::Type::INT32) {
        return of(Physical::kInt32);
      }
      break;
    case arrow::Type::INT64:
      if (s == arrow::Type::INT64 && physical == parquet::Type::INT64) {
        return of(Physical::kInt64);
      }
      break;
    case arrow::Type::DOUBLE:
      if (s == arrow::Type::FLOAT && physical == parquet::Type::FLOAT) {
        return of(Physical::kFloat);
      }
      if (s == arrow::Type::DOUBLE && physical == parquet::Type::DOUBLE) {
        return of(Physical::kDouble);
      }
      break;
    case arrow::Type::BINARY:
      if ((s == arrow::Type::BINARY || s == arrow::Type::STRING) &&
          physical == parquet::Type::BYTE_ARRAY) {
        return of(Physical::kByteArray);
      }
      break;
    default:
      break;
  }
  return std::nullopt;
}

arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> MakeFilteredScan(
    Segment segment, std::vector<FilteredColumn> columns, std::shared_ptr<arrow::Schema> schema,
    int64_t batch_size, arrow::MemoryPool* pool, std::shared_ptr<const plan::ScanFilter> filter) {
  if (batch_size < 1) {
    return arrow::Status::Invalid("the scan batch size must be positive, not ", batch_size);
  }
  if (segment.row_groups.size() != 1) {
    return arrow::Status::Invalid("a filtered scan reads one row group");
  }
  std::vector<bool> seen(columns.size(), false);
  for (const int column : filter->columns()) {
    if (column < 0 || std::cmp_greater_equal(column, columns.size()) ||
        seen[static_cast<std::size_t>(column)]) {
      return arrow::Status::Invalid("a scan filter's column ", column, " is not a scanned field");
    }
    seen[static_cast<std::size_t>(column)] = true;
  }
  return std::make_unique<FilteredScanReader>(std::move(segment), std::move(columns),
                                              std::move(schema), batch_size, pool,
                                              std::move(filter));
}

}  // namespace antb1::io
