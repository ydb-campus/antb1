#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <span>
#include <utility>
#include <vector>

#include <arrow/api.h>

#include "antb1/common/check.h"
#include "antb1/common/narrow.h"
#include "antb1/exec/memory_budget.h"

#include "group_table.h"

namespace antb1::exec {
namespace {

// Bounded row indices are relative to the current record batch. Split very large batches so
// that even a normalized DOUBLE key column and rows with a 64-bit Arrow offset stay bounded.
constexpr std::size_t kDispatchRows = 4096;

template <class Consume>
arrow::Status VisitInput(const arrow::RecordBatch& rows, const arrow::BooleanArray* selection,
                         const Consume& consume) {
  if (selection != nullptr && selection->length() != rows.num_rows()) {
    return arrow::Status::Invalid("group selection length differs from its input");
  }
  constexpr auto kMaxRows = static_cast<std::int64_t>(GroupTable::kMaxMergeChunk);
  if (rows.num_rows() <= kMaxRows) {
    return consume(rows, selection);
  }
  for (std::int64_t start = 0; start < rows.num_rows();) {
    const auto count = std::min(kMaxRows, rows.num_rows() - start);
    const auto mask = selection ? selection->Slice(start, count) : nullptr;
    ARROW_RETURN_NOT_OK(
        consume(*rows.Slice(start, count), static_cast<const arrow::BooleanArray*>(mask.get())));
    start += count;
  }
  return arrow::Status::OK();
}

// Fill only source indices. Payloads keep their original Arrow owners and offsets.
std::size_t SelectRows(const arrow::BooleanArray* selection, std::size_t start, std::size_t count,
                       std::uint32_t* out) {
  std::size_t n = 0;
  for (std::size_t i = start; i < start + count; ++i) {
    const auto row = static_cast<std::int64_t>(i);
    if (selection == nullptr || (selection->IsValid(row) && selection->Value(row))) {
      out[n++] = static_cast<std::uint32_t>(i);
    }
  }
  return n;
}

}  // namespace

arrow::Status GroupTable::ConsumeSelected(const arrow::RecordBatch& rows,
                                          const arrow::BooleanArray* selection) {
  if (selection == nullptr && rows.num_rows() <= static_cast<std::int64_t>(kMaxMergeChunk)) {
    return Consume(rows);
  }
  return Update([&] {
    if (!program_) {
      return arrow::Status::Invalid("selected group updates require a fixed-key aggregate program");
    }
    return VisitInput(
        rows, selection,
        [&](const arrow::RecordBatch& batch, const arrow::BooleanArray* mask) -> arrow::Status {
          if (mask == nullptr) {
            return ConsumeRows(batch, nullptr);
          }
          ARROW_ASSIGN_OR_RAISE(auto keys, NormalizedKeys(batch));
          const auto size = Narrow<std::size_t>(batch.num_rows());
          PoolVector<std::uint32_t> indices(pool_);
          ARROW_RETURN_NOT_OK(indices.Resize(std::min(size, kDispatchRows)));
          for (std::size_t i = 0; i < size; i += kDispatchRows) {
            const auto n = SelectRows(mask, i, std::min(size - i, kDispatchRows), indices.data());
            if (n > 0) {
              ARROW_RETURN_NOT_OK(ConsumeProgram(batch, keys, std::span(indices.data(), n)));
            }
          }
          return arrow::Status::OK();
        });
  });
}

arrow::Result<std::unique_ptr<PartitionedGroupTables>> PartitionedGroupTables::Make(
    const std::vector<plan::BoundColumn>& keys, const std::vector<plan::AggregateCall>& calls,
    int input_width, arrow::MemoryPool* pool, MemoryBudget* budget, bool first_keys,
    std::size_t partitions, const KeyTableOptions& options) {
  return KeyTableNoBadAlloc([&] -> arrow::Result<std::unique_ptr<PartitionedGroupTables>> {
    if (!std::has_single_bit(partitions) || partitions > GroupTable::kPartitions) {
      return arrow::Status::Invalid("unsupported local group partition count: ", partitions);
    }
    if (budget == nullptr) {
      budget = dynamic_cast<MemoryBudget*>(pool);
    }
    MemoryReservation metadata;
    metadata.Reset(budget);
    ARROW_RETURN_NOT_OK(metadata.Resize(sizeof(PartitionedGroupTables)));
    auto bundle = std::unique_ptr<PartitionedGroupTables>(
        new PartitionedGroupTables(input_width, pool, options, std::move(metadata)));
    bundle->lanes_.resize(partitions);
    ARROW_ASSIGN_OR_RAISE(bundle->spare_,
                          GroupTable::MakeLocal(keys, calls, input_width, pool, budget, first_keys,
                                                partitions, options));
    bundle->prototype_ = bundle->spare_.get();
    return bundle;
  });
}

arrow::Result<std::unique_ptr<GroupTable>> GroupTable::MakeLocal(
    const std::vector<plan::BoundColumn>& keys, const std::vector<plan::AggregateCall>& calls,
    int input_width, arrow::MemoryPool* pool, MemoryBudget* budget, bool first_keys,
    std::size_t partitions, const KeyTableOptions& options) {
  return KeyTableNoBadAlloc([&] -> arrow::Result<std::unique_ptr<GroupTable>> {
    // Only type and input positions are used by an execution table. Avoid copying potentially
    // long SQL names into every lane. Keep these per-lane descriptors charged through handoff.
    MemoryReservation metadata;
    metadata.Reset(budget == nullptr ? dynamic_cast<MemoryBudget*>(pool) : budget);
    const auto bytes = sizeof(GroupTable) + sizeof(arrow::compute::ExecContext) +
                       (keys.size() * sizeof(plan::BoundColumn)) +
                       (calls.size() * (sizeof(plan::AggregateCall) +
                                        sizeof(std::unique_ptr<GroupedAggregateState>)));
    ARROW_RETURN_NOT_OK(metadata.Resize(Narrow<std::int64_t>(bytes)));
    std::vector<plan::BoundColumn> fields;
    fields.reserve(keys.size());
    for (const auto& key : keys) {
      auto& field = fields.emplace_back();
      field.index = key.index;
      field.type = key.type;
    }
    std::vector<plan::AggregateCall> aggregates;
    aggregates.reserve(calls.size());
    for (const auto& call : calls) {
      auto& aggregate = aggregates.emplace_back();
      aggregate.kind = call.kind;
      aggregate.type = call.type;
      if (call.arg) {
        aggregate.arg.emplace();
        aggregate.arg->index = call.arg->index;
        aggregate.arg->type = call.arg->type;
      }
    }
    auto charged = options;
    charged.table_metadata = &metadata;
    const auto extra = ((fields.capacity() - keys.size()) * sizeof(plan::BoundColumn)) +
                       ((aggregates.capacity() - calls.size()) * sizeof(plan::AggregateCall));
    ARROW_RETURN_NOT_OK(metadata.Resize(metadata.bytes() + Narrow<std::int64_t>(extra)));
    ARROW_ASSIGN_OR_RAISE(
        auto table,
        GroupTable::Make(std::move(fields), std::move(aggregates), input_width, pool, budget,
                         first_keys, partitions, charged, GroupTableMode::kBatch));
    if (!table->supports_local_dispatch()) {
      return arrow::Status::Invalid("local group dispatch requires a fixed-key aggregate program");
    }
    table->local_metadata_.emplace(std::move(metadata));
    return table;
  });
}

arrow::Status PartitionedGroupTables::Consume(const arrow::RecordBatch& rows,
                                              const arrow::BooleanArray* selection,
                                              std::chrono::nanoseconds* dispatch_elapsed) {
  ARROW_RETURN_NOT_OK(error_);
  if (prototype_ == nullptr) {
    return arrow::Status::Invalid("update of a finished local group partition");
  }
  error_ = KeyTableNoBadAlloc([&] {
    return VisitInput(rows, selection,
                      [&](const arrow::RecordBatch& batch, const arrow::BooleanArray* mask) {
                        return Dispatch(batch, mask, dispatch_elapsed);
                      });
  });
  return error_;
}

arrow::Status PartitionedGroupTables::Dispatch(const arrow::RecordBatch& rows,
                                               const arrow::BooleanArray* selection,
                                               std::chrono::nanoseconds* dispatch_elapsed) {
  const auto size = Narrow<std::size_t>(rows.num_rows());
  if (size == 0) {
    return arrow::Status::OK();
  }
  const auto width = std::min(size, kDispatchRows);
  // Stage replacement scratch before releasing its old buffer so the budget sees the overlap.
  if (hashes_.size() < width) {
    PoolVector<std::uint64_t> hashes(prototype_->pool_);
    PoolVector<std::uint32_t> indices(prototype_->pool_);
    ARROW_RETURN_NOT_OK(hashes.Resize(width));
    ARROW_RETURN_NOT_OK(indices.Resize(2 * width));
    hashes_ = std::move(hashes);
    rows_ = std::move(indices);
  }
  using Clock = std::chrono::steady_clock;
  const auto before_keys = dispatch_elapsed != nullptr ? Clock::now() : Clock::time_point{};
  ARROW_ASSIGN_OR_RAISE(const auto keys, prototype_->NormalizedKeys(rows));
  if (dispatch_elapsed != nullptr) {
    *dispatch_elapsed += Clock::now() - before_keys;
  }
  auto* const selected = rows_.data();
  auto* const order = selected + width;
  const auto lane_mask = lanes_.size() - 1;
  for (std::size_t i = 0; i < size; i += width) {
    const auto start = dispatch_elapsed != nullptr ? Clock::now() : Clock::time_point{};
    const auto n = SelectRows(selection, i, std::min(size - i, width), selected);
    if (n == 0) {
      if (dispatch_elapsed != nullptr) {
        *dispatch_elapsed += Clock::now() - start;
      }
      continue;
    }
    const std::span hashes(hashes_.data(), n);
    ARROW_RETURN_NOT_OK(SelectedGroupHashes(keys, {selected, n}, hashes));
    std::array<std::size_t, GroupTable::kPartitions + 1> starts{};
    for (const auto hash : hashes) {
      ++starts[(hash & lane_mask) + 1];
    }
    std::partial_sum(starts.begin(),
                     starts.begin() + static_cast<std::ptrdiff_t>(lanes_.size() + 1),
                     starts.begin());
    auto next = starts;
    for (std::size_t row = 0; row < n; ++row) {
      order[next[hashes[row] & lane_mask]++] = selected[row];
    }
    if (dispatch_elapsed != nullptr) {
      *dispatch_elapsed += Clock::now() - start;
    }
    for (std::size_t lane = 0; lane < lanes_.size(); ++lane) {
      const auto count = starts[lane + 1] - starts[lane];
      if (count == 0) {
        continue;
      }
      auto& table = lanes_[lane];
      if (!table) {
        if (spare_) {
          table = std::move(spare_);
        } else {
          ARROW_ASSIGN_OR_RAISE(
              table, GroupTable::MakeLocal(prototype_->keys_, prototype_->aggregates_, input_width_,
                                           prototype_->pool_, prototype_->memory_.budget(),
                                           prototype_->keep_first_keys_, lanes_.size(), options_));
        }
      }
      const auto before = table->num_groups();
      ARROW_RETURN_NOT_OK(table->ConsumeProgram(rows, keys, {order + starts[lane], count}));
      groups_ += table->num_groups() - before;
      if (groups_ > options_.max_groups) {
        return arrow::Status::OutOfMemory("a GROUP BY of more than ", options_.max_groups,
                                          " groups");
      }
    }
  }
  return arrow::Status::OK();
}

arrow::Status PartitionedGroupTables::Finish() {
  ARROW_RETURN_NOT_OK(error_);
  hashes_.Reset();
  rows_.Reset();
  prototype_ = nullptr;
  spare_.reset();
  return arrow::Status::OK();
}

std::unique_ptr<const GroupTable> PartitionedGroupTables::ReleasePartition(std::size_t lane) {
  ANTB1_CHECK(prototype_ == nullptr);
  return std::move(lanes_.at(lane));
}

}  // namespace antb1::exec
