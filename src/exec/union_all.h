#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/type_fwd.h>

#include "antb1/exec/operator.h"
#include "antb1/plan/types.h"

namespace antb1::exec {

// UNION ALL (docs/adr/0036-union-all.md): every batch of inputs[0] until its end, then every batch
// of inputs[1], and so on. Each batch is rebuilt in this operator's schema -- input k's columns
// columns[k], in order, under fields named as input 0 names them, of the union's types and all
// nullable, since inputs disagree on names and nullability and the operators above compare schemas
// -- with its selection kept as it is: no row is copied.
//
// An input is opened when the union first needs it and closed at its end, before the next one
// opens: one input's resources at a time (its builds, its parts in flight), and none of an input
// the consumer stops before -- a met LIMIT never opens the rest, and their errors are never raised.
// Errors: the first in input order. Every Next runs on the consumer's thread; an input that is a
// part pipeline runs its own parts on the executor, as it would without the union.
class UnionAllOperator final : public Operator {
 public:
  // Invalid for fewer than two inputs, for a column list per input that does not hold one column
  // per type, and for an input's column outside its schema or not of its type's Arrow type
  // (plan::ToArrow).
  static arrow::Result<std::unique_ptr<UnionAllOperator>> Make(
      std::vector<std::unique_ptr<Operator>> inputs, std::vector<std::vector<int>> columns,
      const std::vector<plan::LogicalType>& types);

  [[nodiscard]] const std::shared_ptr<arrow::Schema>& output_schema() const override {
    return schema_;
  }
  // Records the context: the first Next opens input 0.
  arrow::Status Open(ExecContext& ctx) override;
  // Invalid for a batch whose column is not of the union's type at its position.
  arrow::Result<Batch> Next() override;
  // Closes the input that is open, if any.
  arrow::Status Close() override;

 private:
  UnionAllOperator(std::vector<std::unique_ptr<Operator>> inputs,
                   std::vector<std::vector<int>> columns, std::shared_ptr<arrow::Schema> schema);

  std::vector<std::unique_ptr<Operator>> inputs_;
  std::vector<std::vector<int>> columns_;
  std::shared_ptr<arrow::Schema> schema_;
  ExecContext ctx_;          // a copy: the inputs are opened after Open has returned
  std::size_t current_ = 0;  // the input being read
  bool opened_ = false;
  bool input_open_ = false;  // set before the input's Open, so that Close closes one that failed
};

}  // namespace antb1::exec
