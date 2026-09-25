#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/compute/initialize.h>
#include <gtest/gtest.h>

#include "antb1/exec/operator.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/table.h"

// Helpers of the exec unit tests: arrays, an in-memory plan::Table and a scripted source operator.

namespace antb1::exec::testing {

// Operators call Arrow compute kernels, which need arrow::compute::Initialize() (the engine calls
// it in Session::Make).
class ExecTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { ASSERT_TRUE(arrow::compute::Initialize().ok()); }
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

// An in-memory table: fixed batches, re-sliced to the scan's batch size; counts its scans.
class MemoryTable final : public plan::Table {
 public:
  MemoryTable(std::shared_ptr<arrow::Schema> schema, arrow::RecordBatchVector batches)
      : schema_(std::move(schema)), batches_(std::move(batches)) {}

  const std::shared_ptr<arrow::Schema>& schema() const override { return schema_; }
  std::optional<int64_t> exact_row_count() const override {
    int64_t rows = 0;
    for (const auto& b : batches_) {
      rows += b->num_rows();
    }
    return rows;
  }
  arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> Scan(const std::vector<int>& fields,
                                                                int64_t batch_size) const override {
    ++scans_;
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
    for (const auto& b : batches_) {
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
  std::string Describe() const override { return "memory"; }

  [[nodiscard]] int scans() const { return scans_; }

 private:
  std::shared_ptr<arrow::Schema> schema_;
  arrow::RecordBatchVector batches_;
  mutable int scans_ = 0;
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
    return batches_[next_++];
  }
  arrow::Status Close() override {
    ++closes_;
    return arrow::Status::OK();
  }

  void FailAt(std::size_t index) { fail_at_ = index; }
  [[nodiscard]] int pulls() const { return pulls_; }
  [[nodiscard]] int opens() const { return opens_; }
  [[nodiscard]] int closes() const { return closes_; }

 private:
  std::shared_ptr<arrow::Schema> schema_;
  std::vector<Batch> batches_;
  std::size_t next_ = 0;
  std::optional<std::size_t> fail_at_;
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
