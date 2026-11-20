#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/stl_allocator.h>
#include <arrow/type_fwd.h>

#include "antb1/exec/grouped_aggregate_state.h"
#include "antb1/plan/logical_plan.h"

namespace antb1::exec {

class BatchAggregateState;
class MemoryReservation;

// The ordered state updates of an ordinary fixed-key aggregation. The states are owned and
// resized by GroupTable and finalized by their usual Finalize and CheckFinalize methods.
class GroupedAggregateProgram {
 public:
  static arrow::Result<GroupedAggregateProgram> Make(
      std::span<const plan::AggregateCall> calls,
      std::span<const std::unique_ptr<GroupedAggregateState>> states, arrow::MemoryPool* pool);

  [[nodiscard]] bool supports_updates() const { return supported_; }

  // The input batch and program must outlive this binding. Consume reads the original value
  // arrays at the chunk offset. All states have been resized to include the key table's IDs.
  class BoundBatch {
   public:
    arrow::Status Consume(std::size_t offset, std::span<const std::uint32_t> ids) const;

   private:
    friend class GroupedAggregateProgram;
    BoundBatch(const GroupedAggregateProgram& program,
               std::span<const std::shared_ptr<arrow::Array>> columns, std::size_t rows)
        : program_(program), columns_(columns), rows_(rows) {}
    const GroupedAggregateProgram& program_;
    std::span<const std::shared_ptr<arrow::Array>> columns_;
    std::size_t rows_;
  };

  arrow::Result<BoundBatch> Bind(const arrow::RecordBatch& rows) const;
  // Precharge overlap when per-group buffers grow. The same reservation tracks the table's
  // state capacities between calls.
  arrow::Status Resize(std::uint32_t groups, MemoryReservation& memory) const;
  // Typed states of the same program, in source-group order. Source state is read-only.
  arrow::Status MergeGroups(const GroupedAggregateProgram& source,
                            std::span<const std::uint32_t> from,
                            std::span<const std::uint32_t> to) const;

 private:
  struct Instruction {
    BatchAggregateState* state;
    int column;  // -1 for COUNT(*)
    std::optional<plan::LogicalType> input;
    plan::AggKind kind;
  };
  explicit GroupedAggregateProgram(arrow::MemoryPool* pool)
      : instructions_(arrow::stl::allocator<Instruction>(pool)) {}
  std::vector<Instruction, arrow::stl::allocator<Instruction>> instructions_;
  bool supported_ = true;
};

}  // namespace antb1::exec
