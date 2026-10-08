#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/compute/api_vector.h>
#include <arrow/compute/exec.h>
#include <arrow/compute/initialize.h>
#include <arrow/util/bit_util.h>
#include <arrow/util/cancel.h>
#include <arrow/util/functional.h>
#include <arrow/util/thread_pool.h>
#include <gtest/gtest.h>

#include "antb1/exec/operator.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/table.h"

// Helpers of the exec unit tests: arrays, an in-memory plan::Table, a scripted source operator, an
// executor whose Submit throws and a pool that fails on workers.

namespace antb1::exec::testing {

// Operators call Arrow compute kernels, which need arrow::compute::Initialize() (the engine calls
// it in Session::Make).
class ExecTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { ASSERT_TRUE(arrow::compute::Initialize().ok()); }
};

// Runs every task on `pool`, except that spawn number `throw_at` (from 0) throws std::bad_alloc, as
// Arrow's Submit may when it cannot allocate the task, or `error` if given (e.g. the
// std::system_error of a thread pool that cannot start a worker): that task never runs.
// `before_throw` (if any) is called just before the throw.
class ThrowingExecutor final : public arrow::internal::Executor {
 public:
  ThrowingExecutor(arrow::internal::Executor* pool, int throw_at,
                   std::function<void()> before_throw = {}, std::exception_ptr error = nullptr)
      : pool_(pool),
        throw_at_(throw_at),
        before_throw_(std::move(before_throw)),
        error_(std::move(error)) {}

  int GetCapacity() override { return pool_->GetCapacity(); }
  // The spawns so far, the one that threw included.
  [[nodiscard]] int spawns() const { return spawns_.load(); }

 protected:
  arrow::Status SpawnReal(arrow::internal::TaskHints hints, arrow::internal::FnOnce<void()> task,
                          arrow::StopToken stop_token, StopCallback&& stop_callback) override {
    if (spawns_++ == throw_at_) {
      if (before_throw_) {
        before_throw_();
      }
      if (error_ != nullptr) {
        std::rethrow_exception(error_);
      }
      throw std::bad_alloc();
    }
    return pool_->Spawn(hints, std::move(task), std::move(stop_token), std::move(stop_callback));
  }

 private:
  arrow::internal::Executor* pool_;
  int throw_at_;
  std::function<void()> before_throw_;
  std::exception_ptr error_;
  std::atomic<int> spawns_ = 0;
};

// A pool that fails every allocation off the thread that made it (the consumer's): every part or
// batch computed on a worker runs out of memory, and only one computed again alone succeeds.
class WorkerFailingPool final : public arrow::MemoryPool {
 public:
  explicit WorkerFailingPool(arrow::MemoryPool* backend) : backend_(backend) {}

  arrow::Status Allocate(int64_t size, int64_t alignment, uint8_t** out) override {
    if (std::this_thread::get_id() != owner_) {
      failures_.fetch_add(1);
      return arrow::Status::OutOfMemory("worker allocation");
    }
    return backend_->Allocate(size, alignment, out);
  }
  arrow::Status Reallocate(int64_t old_size, int64_t new_size, int64_t alignment,
                           uint8_t** ptr) override {
    if (std::this_thread::get_id() != owner_) {
      failures_.fetch_add(1);
      return arrow::Status::OutOfMemory("worker allocation");
    }
    return backend_->Reallocate(old_size, new_size, alignment, ptr);
  }
  void Free(uint8_t* buffer, int64_t size, int64_t alignment) override {
    backend_->Free(buffer, size, alignment);
  }
  int64_t bytes_allocated() const override { return backend_->bytes_allocated(); }
  int64_t total_bytes_allocated() const override { return backend_->total_bytes_allocated(); }
  int64_t num_allocations() const override { return backend_->num_allocations(); }
  std::string backend_name() const override { return backend_->backend_name(); }

  [[nodiscard]] int failures() const { return failures_.load(); }

 private:
  arrow::MemoryPool* backend_;
  std::thread::id owner_ = std::this_thread::get_id();
  std::atomic<int> failures_ = 0;
};

template <class Builder, class T>
std::shared_ptr<arrow::Array> ArrayOf(const std::shared_ptr<arrow::DataType>& type,
                                      const std::vector<std::optional<T>>& values) {
  Builder builder(type, arrow::default_memory_pool());
  for (const auto& v : values) {
    EXPECT_TRUE((v.has_value() ? builder.Append(*v) : builder.AppendNull()).ok());
  }
  return builder.Finish().ValueOrDie();
}

inline std::shared_ptr<arrow::Array> Int64s(const std::vector<std::optional<int64_t>>& values) {
  return ArrayOf<arrow::Int64Builder>(arrow::int64(), values);
}

inline std::shared_ptr<arrow::Array> Int16s(const std::vector<std::optional<int16_t>>& values) {
  return ArrayOf<arrow::Int16Builder>(arrow::int16(), values);
}

inline std::shared_ptr<arrow::Array> Int32s(const std::vector<std::optional<int32_t>>& values) {
  return ArrayOf<arrow::Int32Builder>(arrow::int32(), values);
}

inline std::shared_ptr<arrow::Array> UInt16s(const std::vector<std::optional<uint16_t>>& values) {
  return ArrayOf<arrow::UInt16Builder>(arrow::uint16(), values);
}

// DATE values: days since 1970-01-01.
inline std::shared_ptr<arrow::Array> Dates(const std::vector<std::optional<int32_t>>& days) {
  return ArrayOf<arrow::Date32Builder>(arrow::date32(), days);
}

// TIMESTAMP values: microseconds since 1970-01-01 00:00:00.
inline std::shared_ptr<arrow::Array> Timestamps(const std::vector<std::optional<int64_t>>& micros) {
  return ArrayOf<arrow::TimestampBuilder>(arrow::timestamp(arrow::TimeUnit::MICRO), micros);
}

// DECIMAL(precision, scale) values (HUGEINT: 38, 0), unscaled, in decimal digits.
inline std::shared_ptr<arrow::Array> Decimals(
    int32_t precision, int32_t scale, const std::vector<std::optional<std::string>>& values) {
  std::vector<std::optional<arrow::Decimal128>> unscaled;
  unscaled.reserve(values.size());
  for (const auto& v : values) {
    unscaled.push_back(v.has_value() ? std::optional(arrow::Decimal128(*v)) : std::nullopt);
  }
  return ArrayOf<arrow::Decimal128Builder>(arrow::decimal128(precision, scale), unscaled);
}

inline std::shared_ptr<arrow::Array> Strings(
    const std::vector<std::optional<std::string>>& values) {
  return ArrayOf<arrow::BinaryBuilder>(arrow::binary(), values);
}

inline std::shared_ptr<arrow::BooleanArray> Bools(const std::vector<std::optional<bool>>& values) {
  return std::static_pointer_cast<arrow::BooleanArray>(
      ArrayOf<arrow::BooleanBuilder>(arrow::boolean(), values));
}

// The single int64 value of a one-row, one-column table (or std::nullopt for NULL).
inline std::optional<int64_t> SingleInt64(const arrow::Table& table) {
  EXPECT_EQ(table.num_rows(), 1);
  const auto chunk = table.column(0)->chunk(0);
  if (chunk->IsNull(0)) {
    return std::nullopt;
  }
  return std::static_pointer_cast<arrow::Int64Array>(chunk)->Value(0);
}

// The int64 values of column 0 of a table, in order (NULL as std::nullopt).
inline std::vector<std::optional<int64_t>> Int64Column(const arrow::Table& table, int column = 0) {
  std::vector<std::optional<int64_t>> out;
  for (const auto& chunk : table.column(column)->chunks()) {
    const auto& ints = static_cast<const arrow::Int64Array&>(*chunk);
    for (int64_t i = 0; i < ints.length(); ++i) {
      out.push_back(ints.IsNull(i) ? std::nullopt : std::optional(ints.Value(i)));
    }
  }
  return out;
}

// Reads prepared batches.
class VectorReader final : public arrow::RecordBatchReader {
 public:
  VectorReader(std::shared_ptr<arrow::Schema> schema, arrow::RecordBatchVector batches)
      : schema_(std::move(schema)), batches_(std::move(batches)) {}

  using arrow::RecordBatchReader::ReadNext;
  [[nodiscard]] std::shared_ptr<arrow::Schema> schema() const override { return schema_; }
  arrow::Status ReadNext(std::shared_ptr<arrow::RecordBatch>* batch) override {
    *batch = next_ < batches_.size() ? batches_[next_++] : nullptr;
    return arrow::Status::OK();
  }

 private:
  std::shared_ptr<arrow::Schema> schema_;
  arrow::RecordBatchVector batches_;
  std::size_t next_ = 0;
};

// An in-memory table: fixed batches, re-sliced to the scan's batch size; counts its scans. With
// `split`, every batch is a part (plan::Table::ScanPart); the table records which parts were
// scanned, can make one part fail, and applies scan filters as a Parquet table does (VARCHAR
// columns as views, a few rows at a time). Safe to scan from several threads.
class MemoryTable final : public plan::Table {
 public:
  MemoryTable(std::shared_ptr<arrow::Schema> schema, arrow::RecordBatchVector batches,
              bool split = false)
      : schema_(std::move(schema)), batches_(std::move(batches)), split_(split) {}

  const std::shared_ptr<arrow::Schema>& schema() const override { return schema_; }
  std::optional<int64_t> exact_row_count() const override {
    int64_t rows = 0;
    for (const auto& b : batches_) {
      rows += b->num_rows();
    }
    return rows;
  }
  int64_t num_parts() const override { return split_ ? static_cast<int64_t>(batches_.size()) : 1; }
  std::optional<int64_t> part_rows(int64_t part) const override {
    if (!split_) {
      return plan::Table::part_rows(part);
    }
    return part >= 0 && part < num_parts() ? std::optional(Batch(part)->num_rows()) : std::nullopt;
  }
  std::string Describe() const override { return "memory"; }

  [[nodiscard]] int scans() const { return scans_; }
  // The parts ScanPart was called for, sorted.
  [[nodiscard]] std::vector<int64_t> scanned_parts() const {
    const std::scoped_lock lock(mutex_);
    std::vector<int64_t> out = scanned_parts_;
    std::ranges::sort(out);
    return out;
  }
  // ScanPart(part) fails with an IOError naming the part (each part this is called for).
  void FailPart(int64_t part) { failing_parts_.push_back(part); }
  // ScanPart of `field` (with any other fields) fails with an IOError naming the field.
  void FailField(int field) { failing_field_ = field; }
  // With `split`: exact statistics of the BIGINT columns (plan::Table::part_stats), unless turned
  // off here.
  void set_stats(bool stats) { stats_ = stats; }
  // With `split`: whether the table applies scan filters (on unless turned off here), and how
  // many filtered scans of a part it made.
  void set_scan_filter(bool on) { scan_filter_ = on; }
  bool supports_scan_filter(const std::vector<int>& /*fields*/) const override {
    return split_ && scan_filter_;
  }
  [[nodiscard]] int filtered_scans() const { return filtered_scans_; }
  std::optional<plan::PartStats> part_stats(int64_t part, int field) const override {
    if (!split_ || !stats_ || part < 0 || part >= num_parts() || field < 0 ||
        field >= schema_->num_fields() ||
        schema_->field(field)->type()->id() != arrow::Type::INT64) {
      return std::nullopt;
    }
    const auto& values = static_cast<const arrow::Int64Array&>(*Batch(part)->column(field));
    plan::PartStats stats{
        .min = std::nullopt, .max = std::nullopt, .null_count = 0, .rows = values.length()};
    for (int64_t i = 0; i < values.length(); ++i) {
      if (values.IsNull(i)) {
        ++stats.null_count;
        continue;
      }
      const Int128 v = values.Value(i);
      stats.min = stats.min.has_value() ? std::min(*stats.min, v) : v;
      stats.max = stats.max.has_value() ? std::max(*stats.max, v) : v;
    }
    return stats;
  }

 protected:
  arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> DoScan(
      const std::vector<int>& fields, int64_t batch_size,
      arrow::MemoryPool* /*pool*/) const override {
    ++scans_;
    return Read(batches_, fields, batch_size);
  }
  arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> DoScanPart(
      int64_t part, const std::vector<int>& fields, int64_t batch_size,
      arrow::MemoryPool* pool) const override {
    if (!split_) {
      return plan::Table::DoScanPart(part, fields, batch_size, pool);
    }
    if (part < 0 || part >= num_parts()) {
      return arrow::Status::Invalid("no part ", part);
    }
    {
      const std::scoped_lock lock(mutex_);
      scanned_parts_.push_back(part);
    }
    if (std::ranges::find(failing_parts_, part) != failing_parts_.end()) {
      return arrow::Status::IOError("part ", part, " is broken");
    }
    if (failing_field_.has_value() && std::ranges::find(fields, *failing_field_) != fields.end()) {
      return arrow::Status::IOError("field ", *failing_field_, " is broken");
    }
    return Read({Batch(part)}, fields, batch_size);
  }
  arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> DoScanPartFiltered(
      int64_t part, const std::vector<int>& fields, int64_t batch_size, arrow::MemoryPool* pool,
      const std::shared_ptr<const plan::ScanFilter>& filter, bool positions) const override {
    if (!split_ || !scan_filter_) {
      return plan::Table::DoScanPartFiltered(part, fields, batch_size, pool, filter, positions);
    }
    ++filtered_scans_;
    ARROW_ASSIGN_OR_RAISE(auto reader, DoScanPart(part, fields, batch_size, pool));
    auto schema = reader->schema();
    if (positions) {
      ARROW_ASSIGN_OR_RAISE(
          schema, schema->AddField(schema->num_fields(),
                                   arrow::field("position", arrow::int64(), /*nullable=*/false)));
    }
    arrow::RecordBatchVector out;
    int64_t first = 0;  // the position in the part of the batch's first row
    while (true) {
      std::shared_ptr<arrow::RecordBatch> batch;
      ARROW_RETURN_NOT_OK(reader->ReadNext(&batch));
      if (batch == nullptr) {
        break;
      }
      const int64_t rows = batch->num_rows();
      if (positions) {
        arrow::Int64Builder all(pool);
        for (int64_t i = 0; i < rows; ++i) {
          ARROW_RETURN_NOT_OK(all.Append(first + i));
        }
        ARROW_ASSIGN_OR_RAISE(const auto column, all.Finish());
        ARROW_ASSIGN_OR_RAISE(
            batch, batch->AddColumn(batch->num_columns(), schema->fields().back(), column));
      }
      first += rows;
      ARROW_ASSIGN_OR_RAISE(auto selected, arrow::AllocateEmptyBitmap(rows, pool));
      arrow::bit_util::SetBitsTo(selected->mutable_data(), 0, rows, true);
      for (std::size_t k = 0; k < filter->columns().size(); ++k) {
        const int column = static_cast<int>(k);
        const auto& values = batch->column(filter->columns()[k]);
        if (values->type_id() != arrow::Type::BINARY) {
          ARROW_RETURN_NOT_OK(filter->Apply(
              column,
              plan::ScanValues{.array = values, .strings = {}, .validity = nullptr, .rows = rows},
              0, selected->mutable_data()));
          continue;
        }
        const auto& strings = static_cast<const arrow::BinaryArray&>(*values);
        constexpr int64_t kPiece = 3;  // rows per call, as a Parquet table applies a page piece
        for (int64_t start = 0; start < rows; start += kPiece) {
          const int64_t n = std::min(kPiece, rows - start);
          std::vector<std::string_view> views;
          std::vector<std::uint8_t> valid(1, 0);
          for (int64_t i = 0; i < n; ++i) {
            views.push_back(strings.IsNull(start + i) ? std::string_view()
                                                      : strings.GetView(start + i));
            if (strings.IsValid(start + i)) {
              arrow::bit_util::SetBit(valid.data(), i);
            }
          }
          ARROW_RETURN_NOT_OK(filter->Apply(
              column,
              plan::ScanValues{.array = nullptr,
                               .strings = std::span<const std::string_view>(views),
                               .validity = strings.null_count() > 0 ? valid.data() : nullptr,
                               .rows = n},
              start, selected->mutable_data()));
        }
      }
      const auto mask = std::make_shared<arrow::BooleanArray>(rows, std::move(selected));
      // The rows that pass are copied into `pool`, as a Parquet table copies them.
      arrow::compute::ExecContext kernels(pool);
      ARROW_ASSIGN_OR_RAISE(
          const arrow::Datum kept,
          arrow::compute::Filter(batch, mask, arrow::compute::FilterOptions::Defaults(), &kernels));
      if (kept.record_batch()->num_rows() > 0) {
        out.push_back(kept.record_batch());
      }
    }
    return std::make_unique<VectorReader>(std::move(schema), std::move(out));
  }

 private:
  [[nodiscard]] const std::shared_ptr<arrow::RecordBatch>& Batch(int64_t part) const {
    return batches_[static_cast<std::size_t>(part)];
  }

  arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> Read(
      const arrow::RecordBatchVector& batches, const std::vector<int>& fields,
      int64_t batch_size) const {
    arrow::FieldVector out_fields;
    for (const int f : fields) {
      if (f < 0 || f >= schema_->num_fields()) {
        return arrow::Status::Invalid("no field ", f);
      }
      // The batches' own fields: a test may give them other names or types than schema().
      out_fields.push_back(batches_.empty() ? schema_->field(f) : batches_[0]->schema()->field(f));
    }
    auto out_schema = arrow::schema(out_fields);
    arrow::RecordBatchVector out;
    for (const auto& b : batches) {
      for (int64_t start = 0; start < b->num_rows(); start += batch_size) {
        const auto slice = b->Slice(start, batch_size);
        arrow::ArrayVector columns;
        for (const int f : fields) {
          columns.push_back(slice->column(f));
        }
        out.push_back(arrow::RecordBatch::Make(out_schema, slice->num_rows(), columns));
      }
    }
    return std::make_unique<VectorReader>(std::move(out_schema), std::move(out));
  }

  std::shared_ptr<arrow::Schema> schema_;
  arrow::RecordBatchVector batches_;
  bool split_ = false;
  std::vector<int64_t> failing_parts_;
  std::optional<int> failing_field_;
  bool stats_ = true;
  bool scan_filter_ = true;
  mutable std::atomic<int> filtered_scans_ = 0;
  mutable std::atomic<int> scans_ = 0;
  mutable std::mutex mutex_;
  mutable std::vector<int64_t> scanned_parts_;
};

// A source operator that emits prepared batches (with optional selections) and counts the calls.
class ScriptedSource final : public Operator {
 public:
  ScriptedSource(std::shared_ptr<arrow::Schema> schema, std::vector<Batch> batches)
      : schema_(std::move(schema)), batches_(std::move(batches)) {}

  [[nodiscard]] const std::shared_ptr<arrow::Schema>& output_schema() const override {
    return schema_;
  }
  arrow::Status Open(ExecContext& /*ctx*/) override {
    ++opens_;
    next_ = 0;
    return arrow::Status::OK();
  }
  arrow::Result<Batch> Next() override {
    ++pulls_;
    if (fail_at_ == next_) {
      return arrow::Status::IOError("scripted failure");
    }
    if (next_ >= batches_.size()) {
      return Batch{};
    }
    if (hand_over_) {
      return std::move(batches_[next_++]);
    }
    return batches_[next_++];
  }
  arrow::Status Close() override {
    ++closes_;
    return arrow::Status::OK();
  }

  void FailAt(std::size_t index) { fail_at_ = index; }
  // Next() hands each batch on and keeps no reference to it (a later run ends at its first Next(),
  // since a moved-from Batch has no data): a test can then see when the consumer releases the
  // batches.
  void HandOver() { hand_over_ = true; }
  [[nodiscard]] int pulls() const { return pulls_; }
  [[nodiscard]] int opens() const { return opens_; }
  [[nodiscard]] int closes() const { return closes_; }

 private:
  std::shared_ptr<arrow::Schema> schema_;
  std::vector<Batch> batches_;
  std::size_t next_ = 0;
  std::optional<std::size_t> fail_at_;
  bool hand_over_ = false;
  int pulls_ = 0;
  int opens_ = 0;
  int closes_ = 0;
};

inline plan::BoundColumn Column(int index, std::string name, plan::LogicalType type) {
  return plan::BoundColumn{.index = index, .name = std::move(name), .type = type};
}

inline plan::Predicate Compare(plan::BoundColumn column, plan::CompareOp op,
                               plan::Constant constant) {
  return plan::Predicate{.kind = plan::Predicate::Kind::kCompare,
                         .column = std::move(column),
                         .op = op,
                         .constant = std::move(constant),
                         .span = {}};
}

inline plan::Constant BigInt(int64_t v) {
  return plan::Constant{.type = plan::LogicalType::kBigInt, .value = Int128{v}};
}

}  // namespace antb1::exec::testing
