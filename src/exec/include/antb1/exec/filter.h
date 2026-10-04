#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <arrow/datum.h>
#include <arrow/record_batch.h>
#include <arrow/result.h>
#include <arrow/scalar.h>
#include <arrow/type.h>

#include "antb1/exec/like.h"
#include "antb1/exec/operator.h"
#include "antb1/plan/logical_plan.h"

namespace antb1::exec {

// One predicate prepared for an input schema. Evaluate gives SQL's three-valued result per row
// (NULL where the comparison has a NULL operand); with `kleene`, kFalse and kIsNotNull also give
// NULL for a NULL column, as inside an expression (a filter rejects NULL and false alike). A
// DECIMAL compares with a DECIMAL of another scale or an integer exactly by value, and with a
// DOUBLE (a column, a constant, an IN list) in DOUBLE after DuckDB's conversion (ADR 0021).
class PredicateEvaluator {
 public:
  static arrow::Result<PredicateEvaluator> Make(const plan::Predicate& predicate,
                                                const arrow::Schema& schema);
  arrow::Result<arrow::Datum> Evaluate(const arrow::RecordBatch& batch, arrow::MemoryPool* pool,
                                       bool kleene) const;

 private:
  // How a comparison evaluates, decided by Make from the types.
  enum class Mode : std::uint8_t {
    kArrow,           // Arrow's comparison kernels on the values as they are
    kColumnToDouble,  // kCompare, kIn, kNotIn: the DECIMAL column converted to DOUBLE
    kInDouble,        // kCompareColumns: the side that is not DOUBLE converted to DOUBLE
    kExact,           // kCompareColumns: a DECIMAL with another type of number, by value
  };

  plan::Predicate predicate_;
  Mode mode_ = Mode::kArrow;
  int column_ = -1;                                     // -1: none (kFalse without a column)
  int other_ = -1;                                      // kCompareColumns only
  std::shared_ptr<arrow::Scalar> constant_;             // kCompare only
  std::optional<LikePattern> pattern_;                  // kLike and kNotLike only
  std::vector<std::shared_ptr<arrow::Scalar>> values_;  // kIn and kNotIn only
};

// Keeps the rows for which every predicate is true, without copying data: each input batch gets a
// selection (the AND of the comparisons, computed with Arrow's comparison kernels, LikePattern for
// [NOT] LIKE, and and_kleene; a NULL comparison rejects the row). Batches without a selected row
// are skipped, and a batch whose rows all pass keeps no selection. A folded FALSE predicate ends
// the stream without reading input. Output: the input columns.
class FilterOperator final : public Operator {
 public:
  FilterOperator(std::unique_ptr<Operator> input, std::vector<plan::Predicate> predicates);

  [[nodiscard]] const std::shared_ptr<arrow::Schema>& output_schema() const override {
    return input_->output_schema();
  }
  arrow::Status Open(ExecContext& ctx) override;
  arrow::Result<Batch> Next() override;
  arrow::Status Close() override { return input_->Close(); }

 private:
  // The selection of one batch (with NULLs, before normalization).
  arrow::Result<std::shared_ptr<arrow::Array>> Evaluate(const arrow::RecordBatch& batch) const;

  std::unique_ptr<Operator> input_;
  std::vector<plan::Predicate> predicates_;
  std::vector<PredicateEvaluator> evaluators_;  // from Open (none when never_true_)
  bool never_true_ = false;
  arrow::MemoryPool* pool_ = arrow::default_memory_pool();
};

}  // namespace antb1::exec
