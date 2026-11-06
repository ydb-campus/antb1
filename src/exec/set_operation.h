#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/type_fwd.h>

#include "antb1/exec/memory_budget.h"
#include "antb1/exec/operator.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

#include "group_table.h"

namespace antb1::exec {

// UNION [DISTINCT], INTERSECT [ALL] and EXCEPT [ALL] of one or more inputs, one tree of them
// (plan::SetOperationNode, docs/adr/0039-set-operations.md), serial.
//
// One GroupTable keyed on every column holds the distinct rows. The inputs are read in written
// order, one at a time as UnionAllOperator reads them -- input k opens after input k - 1 has
// closed, so one input's resources at a time -- and each input in full: the operator is blocking,
// and the first input that fails decides the error. A starter's rows (plan::SetStarters) insert
// their groups; every other input only looks its rows up and adds none, since its rows can only
// remove a group or keep it. Per group, each input keeps a presence bit, or a count where the
// formula reads one (plan::SetCounted). After the last input the formula gives each group its
// copies: under a DISTINCT root the kept groups come out once, with their keys as first seen among
// the starters (in written order, then each input's batch order), selected over the table's chunks
// without a copy; under an ALL root each group comes out as many times as its copies, with its
// normalized keys (-0.0 as 0.0, every NaN as one NaN), as DuckDB prints the ALL forms.
//
// The table and the bits and counts are charged to the budget; nothing spills. Every Next runs on
// the consumer's thread; an input that is a part pipeline runs its own parts on the executor.
class SetOperationOperator final : public Operator {
 public:
  // Invalid without inputs, for a formula that does not check (plan::CheckSetFormula), for a column
  // list per input that does not hold one column per type, and for an input's column outside its
  // schema or not of its type's Arrow type, as UnionAllOperator::Make.
  static arrow::Result<std::unique_ptr<SetOperationOperator>> Make(
      std::vector<std::unique_ptr<Operator>> inputs, std::vector<std::vector<int>> columns,
      const std::vector<plan::LogicalType>& types, std::vector<plan::SetTerm> formula);

  [[nodiscard]] const std::shared_ptr<arrow::Schema>& output_schema() const override {
    return schema_;
  }
  // Records the context: the first Next reads the inputs.
  arrow::Status Open(ExecContext& ctx) override;
  arrow::Result<Batch> Next() override;
  // Closes the input that is open, if any, and gives the table back.
  arrow::Status Close() override;

  // The most rows of one output batch under an ALL root.
  static constexpr int64_t kMaxRepeatedRows = int64_t{64} * 1024;

 private:
  SetOperationOperator(std::vector<std::unique_ptr<Operator>> inputs,
                       std::vector<std::vector<int>> columns, std::vector<plan::LogicalType> types,
                       std::vector<plan::SetTerm> formula, std::shared_ptr<arrow::Schema> schema);

  // Reads every input into the table and the memberships, then computes each group's copies.
  arrow::Status Build();
  // The rows of input k's batch: its columns at the operator's positions, its selection applied.
  arrow::Result<std::shared_ptr<arrow::RecordBatch>> Rows(std::size_t k, const Batch& batch) const;
  // Grows every membership to the table's groups and charges the budget for them.
  arrow::Status Grow();
  // Each group's copies (formula), into copies_.
  arrow::Status Evaluate();
  arrow::Result<Batch> NextDistinct();
  arrow::Result<Batch> NextRepeated();

  std::vector<std::unique_ptr<Operator>> inputs_;
  std::vector<std::vector<int>> columns_;
  std::vector<plan::LogicalType> types_;
  std::vector<plan::SetTerm> formula_;
  std::vector<bool> starters_;
  std::vector<bool> counted_;
  bool all_root_ = false;  // the root is INTERSECT ALL or EXCEPT ALL: copies, normalized keys
  std::shared_ptr<arrow::Schema> schema_;

  ExecContext ctx_;  // a copy: the inputs are opened after Open has returned
  bool opened_ = false;
  bool built_ = false;
  std::size_t open_input_ = 0;  // the input that is open, when input_open_
  bool input_open_ = false;
  std::unique_ptr<GroupTable> table_;
  // Per input: a presence bit per group (bits_) or a count per group (counts_), by counted_.
  std::vector<std::vector<std::uint8_t>> bits_;
  std::vector<std::vector<int64_t>> counts_;
  std::vector<int64_t> copies_;  // per group, after Build
  MemoryReservation memory_;     // bits_, counts_ and copies_
  // The output's progress: the next group (DISTINCT: the first group of the next chunk), and under
  // an ALL root the copies of that group still to emit and the table's normalized keys.
  std::uint32_t next_group_ = 0;
  int64_t pending_copies_ = 0;
  std::shared_ptr<arrow::RecordBatch> uniques_;
};

}  // namespace antb1::exec
