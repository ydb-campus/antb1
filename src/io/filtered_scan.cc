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
#include <type_traits>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/util/bit_run_reader.h>
#include <arrow/util/bit_util.h>
#include <arrow/util/bitmap_ops.h>
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
// values of its non-NULL rows, densely, in a buffer of the scan's pool that an array may take over
// (FixedArray).
template <class Value>
struct Decoded {
  std::vector<int16_t> levels;
  std::shared_ptr<arrow::ResizableBuffer> values;
  int64_t rows = 0;
  int64_t values_read = 0;
};

// Reads `rows` rows of a column (a flat leaf with max definition level `max_def`).
template <class Reader, class Value>
arrow::Status ReadRows(Reader& reader, int16_t max_def, int64_t rows, Decoded<Value>& out,
                       arrow::MemoryPool* pool) {
  if (max_def > 0) {
    out.levels.resize(static_cast<std::size_t>(rows));
  }
  const int64_t bytes = rows * static_cast<int64_t>(sizeof(Value));
  if (out.values == nullptr || out.values.use_count() > 1) {  // the last batch's array holds it
    ARROW_ASSIGN_OR_RAISE(out.values, arrow::AllocateResizableBuffer(bytes, pool));
  } else {
    ARROW_RETURN_NOT_OK(out.values->Resize(bytes, /*shrink_to_fit=*/false));
  }
  out.rows = 0;
  out.values_read = 0;
  auto* values = out.values->template mutable_data_as<Value>();
  while (out.rows < rows) {
    int64_t read = 0;
    const int64_t levels =
        reader.ReadBatch(rows - out.rows, max_def > 0 ? out.levels.data() + out.rows : nullptr,
                         nullptr, values + out.values_read, &read);
    if (levels <= 0) {
      return arrow::Status::IOError("a column chunk holds fewer rows than its row group");
    }
    out.rows += levels;
    out.values_read += read;
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

// Whether the engine type `id` stores a physical `Value` as it is.
template <class Value>
bool SameRepresentation(arrow::Type::type id) {
  if constexpr (std::is_same_v<Value, int32_t>) {
    return id == arrow::Type::INT32 || id == arrow::Type::DATE32;
  } else if constexpr (std::is_same_v<Value, int64_t>) {
    return id == arrow::Type::INT64;
  } else if constexpr (std::is_same_v<Value, double>) {
    return id == arrow::Type::DOUBLE;
  } else {
    return false;
  }
}

// Stores the rows of `decoded` (all, or with `selected` only those whose bit is set) as `Out`
// values from `out` on; `valid` (nullptr: the column has no NULLs) gets their validity. Returns the
// number of NULLs stored.
template <class Out, class Value>
int64_t StoreRows(const Decoded<Value>& decoded, int16_t max_def, const std::uint8_t* selected,
                  Out* out, std::uint8_t* valid) {
  const auto* values = decoded.values->template data_as<Value>();
  if (max_def == 0 || decoded.values_read == decoded.rows) {  // no NULL: value i is row i
    if (selected == nullptr) {
      for (int64_t row = 0; row < decoded.rows; ++row) {
        out[row] = static_cast<Out>(values[row]);
      }
    } else {
      int64_t at = 0;
      arrow::internal::VisitSetBitRunsVoid(selected, 0, decoded.rows,
                                           [&](int64_t position, int64_t length) {
                                             for (int64_t i = 0; i < length; ++i) {
                                               out[at + i] = static_cast<Out>(values[position + i]);
                                             }
                                             at += length;
                                           });
    }
    return 0;
  }
  int64_t at = 0;
  int64_t next_value = 0;
  int64_t nulls = 0;
  for (int64_t row = 0; row < decoded.rows; ++row) {
    const bool present = decoded.levels[static_cast<std::size_t>(row)] == max_def;
    if (selected == nullptr || BitAt(selected, row)) {
      if (present) {
        out[at] = static_cast<Out>(values[next_value]);
        SetBitAt(valid, at);
      } else {
        out[at] = Out{};
        ++nulls;
      }
      ++at;
    }
    next_value += present ? 1 : 0;
  }
  return nulls;
}

// The engine array of `decoded` (every row, NULLs where the levels say so); with `selected`, only
// the rows whose bit is set (`count` of them).
template <class Value>
arrow::Result<std::shared_ptr<arrow::Array>> FixedArray(
    const Decoded<Value>& decoded, int16_t max_def, const std::shared_ptr<arrow::DataType>& type,
    const std::uint8_t* selected, int64_t count, arrow::MemoryPool* pool) {
  const int64_t length = selected == nullptr ? decoded.rows : count;
  const int width = WidthOf(*type);
  if (selected == nullptr && (max_def == 0 || decoded.values_read == decoded.rows) &&
      SameRepresentation<Value>(type->id())) {
    // Every row, none NULL, stored as the engine stores it: the decoded values are the array.
    return arrow::MakeArray(arrow::ArrayData::Make(type, length, {nullptr, decoded.values}, 0));
  }
  ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::Buffer> values,
                        arrow::AllocateBuffer(length * width, pool));
  std::shared_ptr<arrow::Buffer> validity;
  std::uint8_t* valid = nullptr;
  if (max_def > 0 && decoded.values_read < decoded.rows) {
    ARROW_ASSIGN_OR_RAISE(validity, arrow::AllocateEmptyBitmap(length, pool));
    valid = validity->mutable_data();
  }
  int64_t nulls = 0;
  switch (type->id()) {
    case arrow::Type::INT16:
      nulls = StoreRows(decoded, max_def, selected, values->mutable_data_as<int16_t>(), valid);
      break;
    case arrow::Type::UINT16:
      nulls = StoreRows(decoded, max_def, selected, values->mutable_data_as<uint16_t>(), valid);
      break;
    case arrow::Type::INT32:
    case arrow::Type::DATE32:
      nulls = StoreRows(decoded, max_def, selected, values->mutable_data_as<int32_t>(), valid);
      break;
    case arrow::Type::INT64:
      nulls = StoreRows(decoded, max_def, selected, values->mutable_data_as<int64_t>(), valid);
      break;
    case arrow::Type::DOUBLE:
      nulls = StoreRows(decoded, max_def, selected, values->mutable_data_as<double>(), valid);
      break;
    default:
      return arrow::Status::Invalid("a filtered scan cannot store ", type->ToString());
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
  // The last batch read, of the column's physical type (kept: the next batch reuses its memory).
  Decoded<int32_t> int32s;
  Decoded<int64_t> int64s;
  Decoded<float> floats;
  Decoded<double> doubles;
};

class FilteredScanReader final : public arrow::RecordBatchReader {
 public:
  FilteredScanReader(Segment segment, std::vector<FilteredColumn> columns,
                     std::shared_ptr<arrow::Schema> schema, int64_t batch_size,
                     arrow::MemoryPool* pool, std::shared_ptr<const plan::ScanFilter> filter,
                     bool positions)
      : segment_(std::move(segment)),
        schema_(std::move(schema)),
        batch_size_(batch_size),
        pool_(pool),
        filter_(std::move(filter)),
        positions_(positions) {
    for (FilteredColumn& column : columns) {
      columns_.push_back(ColumnState{.column = std::move(column),
                                     .max_def = 0,
                                     .reader = nullptr,
                                     .int32s = {},
                                     .int64s = {},
                                     .floats = {},
                                     .doubles = {}});
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
  // The column readers go before the file reader they read from.
  ~FilteredScanReader() override { static_cast<void>(Close()); }

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
      state.int32s = {};  // the decoded buffers go back to the pool
      state.int64s = {};
      state.floats = {};
      state.doubles = {};
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
      const int64_t first = next_row_;
      next_row_ += rows;
      ARROW_ASSIGN_OR_RAISE(*out, ReadBatch(rows, first));
      if (*out != nullptr) {
        return arrow::Status::OK();
      }
    }
    return arrow::Status::OK();
  }

  // The next `rows` rows, from row `first` of the row group on: the filter's columns first, then
  // the rows that pass of every column; nullptr when none passes.
  arrow::Result<std::shared_ptr<arrow::RecordBatch>> ReadBatch(int64_t rows, int64_t first) {
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
        candidates[p] = std::make_unique<Candidates>(pool_);
        ARROW_RETURN_NOT_OK(candidates[p]->Reserve(rows));
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
    if (positions_) {
      ARROW_ASSIGN_OR_RAISE(auto position, Positions(selected, rows, count, first));
      arrays.push_back(std::move(position));
    }
    return arrow::RecordBatch::Make(schema_, count, std::move(arrays));
  }

  // The positions in the row group of the `count` selected rows of a batch of `rows` rows whose
  // first row is at `first`.
  arrow::Result<std::shared_ptr<arrow::Array>> Positions(const std::uint8_t* selected, int64_t rows,
                                                         int64_t count, int64_t first) const {
    ARROW_ASSIGN_OR_RAISE(
        std::shared_ptr<arrow::Buffer> values,
        arrow::AllocateBuffer(count * static_cast<int64_t>(sizeof(int64_t)), pool_));
    auto* out = values->mutable_data_as<int64_t>();
    int64_t at = 0;
    arrow::internal::VisitSetBitRunsVoid(selected, 0, rows, [&](int64_t row, int64_t length) {
      for (int64_t i = 0; i < length; ++i) {
        out[at++] = first + row + i;
      }
    });
    return arrow::MakeArray(
        arrow::ArrayData::Make(arrow::int64(), count, {nullptr, std::move(values)}, 0));
  }

  // The values a filter's string column kept for the rows still selected after its own
  // predicates: the rows (in order) and their values.
  struct Candidates {
    explicit Candidates(arrow::MemoryPool* pool) : values(pool) {}

    // Room for `rows` values.
    arrow::Status Reserve(int64_t count) {
      rows.reserve(static_cast<std::size_t>(count));
      return values.Reserve(count);
    }

    std::vector<int64_t> rows;
    arrow::BinaryBuilder values;  // in the query's pool, so that the budget sees the bytes

    // The values of the rows selected in the end.
    arrow::Result<std::shared_ptr<arrow::Array>> Finish(const std::uint8_t* selected, int64_t count,
                                                        arrow::MemoryPool* pool) {
      ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::Array> kept, values.Finish());
      if (std::cmp_equal(rows.size(), count)) {
        return kept;  // no later column rejected any of them
      }
      const auto& strings = static_cast<const arrow::BinaryArray&>(*kept);
      arrow::BinaryBuilder builder(pool);
      ARROW_RETURN_NOT_OK(builder.Reserve(count));
      for (std::size_t i = 0; i < rows.size(); ++i) {
        if (!BitAt(selected, rows[i])) {
          continue;
        }
        const auto at = static_cast<int64_t>(i);
        ARROW_RETURN_NOT_OK(strings.IsNull(at) ? builder.AppendNull()
                                               : builder.Append(strings.GetView(at)));
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
      ARROW_RETURN_NOT_OK(kept.values.ReserveData(SelectedBytes(selected, done)));
      for (int64_t i = 0; i < piece; ++i) {
        if (BitAt(selected, done + i)) {
          kept.rows.push_back(done + i);
          if (piece_nulls_ && !BitAt(piece_valid_.data(), i)) {
            kept.values.UnsafeAppendNull();
          } else {
            kept.values.UnsafeAppend(views_[static_cast<std::size_t>(i)]);
          }
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
      ARROW_RETURN_NOT_OK(builder.ReserveData(SelectedBytes(selected, done)));
      for (int64_t i = 0; i < piece; ++i) {
        if (!BitAt(selected, done + i)) {
          continue;
        }
        if (piece_nulls_ && !BitAt(piece_valid_.data(), i)) {
          builder.UnsafeAppendNull();
        } else {
          builder.UnsafeAppend(views_[static_cast<std::size_t>(i)]);
        }
      }
      done += piece;
    }
    return builder.Finish();
  }

  // The bytes of the values of the piece in views_ whose rows (from `first` on) are selected.
  [[nodiscard]] int64_t SelectedBytes(const std::uint8_t* selected, int64_t first) const {
    int64_t bytes = 0;
    for (std::size_t i = 0; i < views_.size(); ++i) {
      if (BitAt(selected, first + static_cast<int64_t>(i))) {
        bytes += static_cast<int64_t>(views_[i].size());  // a NULL row's view is empty
      }
    }
    return bytes;
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
        ARROW_RETURN_NOT_OK(ReadRows(static_cast<parquet::Int32Reader&>(*state.reader),
                                     state.max_def, rows, state.int32s, pool_));
        return FixedArray(state.int32s, state.max_def, type, selected, count, pool_);
      }
      case Physical::kInt64: {
        ARROW_RETURN_NOT_OK(ReadRows(static_cast<parquet::Int64Reader&>(*state.reader),
                                     state.max_def, rows, state.int64s, pool_));
        return FixedArray(state.int64s, state.max_def, type, selected, count, pool_);
      }
      case Physical::kFloat: {
        ARROW_RETURN_NOT_OK(ReadRows(static_cast<parquet::FloatReader&>(*state.reader),
                                     state.max_def, rows, state.floats, pool_));
        return FixedArray(state.floats, state.max_def, type, selected, count, pool_);
      }
      case Physical::kDouble: {
        ARROW_RETURN_NOT_OK(ReadRows(static_cast<parquet::DoubleReader&>(*state.reader),
                                     state.max_def, rows, state.doubles, pool_));
        return FixedArray(state.doubles, state.max_def, type, selected, count, pool_);
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
    const arrow::ArrayData& data = *array.data();
    const std::uint8_t* in = data.buffers[1]->data() + (data.offset * width);
    const std::uint8_t* in_valid = data.buffers[0] == nullptr ? nullptr : data.buffers[0]->data();
    int64_t at = 0;
    // Runs of selected rows: one copy each, of the values and of their validity.
    arrow::internal::VisitSetBitRunsVoid(
        selected, 0, array.length(), [&](int64_t row, int64_t length) {
          std::memcpy(values->mutable_data() + (at * width), in + (row * width),
                      static_cast<std::size_t>(length * width));
          if (validity != nullptr) {
            arrow::internal::CopyBitmap(in_valid, data.offset + row, length,
                                        validity->mutable_data(), at);
          }
          at += length;
        });
    const int64_t null_count =
        validity == nullptr ? 0 : count - arrow::internal::CountSetBits(validity->data(), 0, count);
    if (null_count == 0) {
      validity = nullptr;
    }
    return arrow::MakeArray(arrow::ArrayData::Make(state.column.engine, /*length=*/count,
                                                   {validity, values}, null_count));
  }

  static arrow::Status Skip(ColumnState& state, int64_t rows) {
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
    if (!status.IsIOError() || status.message().contains(segment_.path)) {
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
  bool positions_ = false;         // a last column with the rows' positions in the row group
  std::vector<int> filter_index_;  // per column: its position in filter_->columns(), or -1
  // The reader of the file outlives its row group's and columns' readers (Close, the destructor).
  std::unique_ptr<parquet::ParquetFileReader> file_reader_;
  std::shared_ptr<parquet::RowGroupReader> row_group_;
  int64_t rows_left_ = 0;
  int64_t next_row_ = 0;  // the position in the row group of the next batch's first row
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
    int64_t batch_size, arrow::MemoryPool* pool, std::shared_ptr<const plan::ScanFilter> filter,
    bool positions) {
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
  if (positions) {
    ARROW_ASSIGN_OR_RAISE(
        schema, schema->AddField(schema->num_fields(),
                                 arrow::field("position", arrow::int64(), /*nullable=*/false)));
  }
  return std::make_unique<FilteredScanReader>(std::move(segment), std::move(columns),
                                              std::move(schema), batch_size, pool,
                                              std::move(filter), positions);
}

}  // namespace antb1::io
