#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/compute/api_vector.h>

#include "antb1/common/check.h"
#include "antb1/common/narrow.h"
#include "antb1/exec/memory_budget.h"

#include "double_key.h"
#include "group_table.h"
#include "group_vector.h"

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

// Usually the original arrays already contain canonical values at all selected positions.
// If an accepted -0 or NaN needs a rewrite, keep only the selected grouping keys together in
// source order. The aggregate payload and the first-seen spellings stay in the original batch.
arrow::Result<std::optional<arrow::compute::ExecBatch>> CompactDoubleKeys(
    const arrow::compute::ExecBatch& keys, std::span<const std::uint32_t> rows,
    arrow::compute::ExecContext* ctx, MemoryBudget* budget, MemoryReservation& metadata) {
  const bool needs_change = std::ranges::any_of(keys.values, [&](const arrow::Datum& key) {
    return key.type()->id() == arrow::Type::DOUBLE &&
           NeedsDoubleKeyNormalization(arrow::DoubleArray(key.array()), rows);
  });
  if (!needs_change) {
    return std::nullopt;
  }
  metadata.Reset(budget);
  const auto charge = [&](std::size_t count, std::size_t width) {
    const auto available =
        static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max() - metadata.bytes());
    if (count > available / width) {
      return arrow::Status::CapacityError("compact grouping key metadata size overflow");
    }
    return metadata.Resize(metadata.bytes() + Narrow<std::int64_t>(count * width));
  };
  // Each compact fixed-width column keeps an ArrayData and two buffer pointers. The pool
  // already accounts for the values and validity bytes. Keep the metadata charge through use.
  constexpr auto kBufferPtr = sizeof(std::shared_ptr<arrow::Buffer>);
  ARROW_RETURN_NOT_OK(charge(keys.values.size(), sizeof(arrow::ArrayData) + (2 * kBufferPtr)));
  std::vector<arrow::Datum> compact;
  ARROW_RETURN_NOT_OK(ReserveGroupVector(compact, keys.values.size(), metadata));
  const auto length = Narrow<std::int64_t>(rows.size());
  const arrow::UInt32Array indices(length, arrow::Buffer::Wrap(rows.data(), length));
  for (const auto& key : keys.values) {
    if (key.type()->id() == arrow::Type::DOUBLE) {
      ARROW_ASSIGN_OR_RAISE(
          auto column, SelectDoubleKeys(arrow::DoubleArray(key.array()), rows, ctx->memory_pool()));
      compact.emplace_back(std::move(column));
    } else {
      ARROW_ASSIGN_OR_RAISE(
          auto column, arrow::compute::Take(*key.make_array(), indices,
                                            arrow::compute::TakeOptions::NoBoundsCheck(), ctx));
      compact.emplace_back(std::move(column));
    }
    const auto capacity = compact.back().array()->buffers.capacity();
    if (capacity > 2) {
      ARROW_RETURN_NOT_OK(charge(capacity - 2, kBufferPtr));
    }
  }
  return arrow::compute::ExecBatch(std::move(compact), length);
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
          const auto selected = Narrow<std::size_t>(mask->true_count());
          if (selected == 0) {
            return arrow::Status::OK();
          }
          ARROW_ASSIGN_OR_RAISE(auto keys, InputKeys(batch, /*normalize_doubles=*/false));
          const auto size = Narrow<std::size_t>(batch.num_rows());
          PoolVector<std::uint32_t> indices(pool_);
          ARROW_RETURN_NOT_OK(indices.Resize(std::min(selected, kDispatchRows)));
          for (std::size_t i = 0; i < size; i += kDispatchRows) {
            const auto n = SelectRows(mask, i, std::min(size - i, kDispatchRows), indices.data());
            if (n > 0) {
              const std::span<const std::uint32_t> source_rows(indices.data(), n);
              MemoryReservation metadata;
              ARROW_ASSIGN_OR_RAISE(
                  auto compact,
                  CompactDoubleKeys(keys, source_rows, kernels_.get(), memory_.budget(), metadata));
              if (compact) {
                ARROW_RETURN_NOT_OK(ConsumeProgram(batch, *compact, {}, source_rows));
              } else {
                ARROW_RETURN_NOT_OK(ConsumeProgram(batch, keys, source_rows, source_rows));
              }
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
  const auto kept = selection != nullptr ? Narrow<std::size_t>(selection->true_count()) : size;
  if (kept == 0) {
    return arrow::Status::OK();
  }
  const auto width = std::min(kept, kDispatchRows);
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
  ARROW_ASSIGN_OR_RAISE(const auto raw_keys, prototype_->InputKeys(rows, selection == nullptr));
  if (dispatch_elapsed != nullptr) {
    *dispatch_elapsed += Clock::now() - before_keys;
  }
  auto* const selected = rows_.data();
  auto* const order = selected + width;
  const auto lane_mask = lanes_.size() - 1;
  for (std::size_t i = 0; i < size; i += kDispatchRows) {
    const auto start = dispatch_elapsed != nullptr ? Clock::now() : Clock::time_point{};
    const auto n = SelectRows(selection, i, std::min(size - i, kDispatchRows), selected);
    if (n == 0) {
      if (dispatch_elapsed != nullptr) {
        *dispatch_elapsed += Clock::now() - start;
      }
      continue;
    }
    MemoryReservation metadata;
    std::optional<arrow::compute::ExecBatch> compact;
    PoolVector<std::uint32_t> source_order(prototype_->pool_);
    std::span<const std::uint32_t> key_rows(selected, n);
    if (selection != nullptr) {
      ARROW_ASSIGN_OR_RAISE(compact,
                            CompactDoubleKeys(raw_keys, key_rows, prototype_->kernels_.get(),
                                              prototype_->memory_.budget(), metadata));
    }
    if (compact) {
      ARROW_RETURN_NOT_OK(source_order.Resize(n));
      std::ranges::iota(std::span(source_order.data(), n), 0U);
      key_rows = {source_order.data(), n};
    }
    const auto& keys = compact ? *compact : raw_keys;
    const std::span hashes(hashes_.data(), n);
    ARROW_RETURN_NOT_OK(SelectedGroupHashes(keys, key_rows, hashes));
    std::array<std::size_t, GroupTable::kPartitions + 1> starts{};
    for (const auto hash : hashes) {
      ++starts[(hash & lane_mask) + 1];
    }
    std::partial_sum(starts.begin(),
                     starts.begin() + static_cast<std::ptrdiff_t>(lanes_.size() + 1),
                     starts.begin());
    auto next = starts;
    for (std::size_t row = 0; row < n; ++row) {
      order[next[hashes[row] & lane_mask]++] = key_rows[row];
    }
    std::span<const std::uint32_t> input_order(order, n);
    if (compact) {
      for (std::size_t row = 0; row < n; ++row) {
        source_order[row] = selected[order[row]];
      }
      input_order = {source_order.data(), n};
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
        ARROW_RETURN_NOT_OK(CreateLane(lane));
      }
      const auto before = table->num_groups();
      ARROW_RETURN_NOT_OK(table->ConsumeProgram(rows, keys, {order + starts[lane], count},
                                                input_order.subspan(starts[lane], count)));
      groups_ += table->num_groups() - before;
      if (groups_ > options_.max_groups) {
        return arrow::Status::OutOfMemory("a GROUP BY of more than ", options_.max_groups,
                                          " groups");
      }
    }
  }
  return arrow::Status::OK();
}

arrow::Status PartitionedGroupTables::CreateLane(std::size_t lane) {
  auto& table = lanes_[lane];
  ANTB1_DCHECK(table == nullptr);
  if (spare_) {
    table = std::move(spare_);
  } else {
    ARROW_ASSIGN_OR_RAISE(
        table, GroupTable::MakeLocal(prototype_->keys_, prototype_->aggregates_, input_width_,
                                     prototype_->pool_, prototype_->memory_.budget(),
                                     prototype_->keep_first_keys_, lanes_.size(), options_));
  }
  return arrow::Status::OK();
}

arrow::Status PartitionedGroupTables::Seed(const GroupTable& source) {
  ANTB1_DCHECK(groups_ == 0);
  ANTB1_DCHECK(source.num_partitions() == lanes_.size());
  for (std::size_t lane = 0; lane < lanes_.size(); ++lane) {
    const auto groups = source.partition_groups(lane);
    if (!groups.empty()) {
      ARROW_RETURN_NOT_OK(CreateLane(lane));
      ARROW_RETURN_NOT_OK(lanes_[lane]->MergePartition(source, lane));
      ANTB1_DCHECK(lanes_[lane]->num_groups() == groups.size());
    }
  }
  groups_ = source.num_groups();
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

arrow::Result<std::unique_ptr<CardinalityLocalGroups>> CardinalityLocalGroups::Make(
    const std::vector<plan::BoundColumn>& keys, const std::vector<plan::AggregateCall>& calls,
    int input_width, arrow::MemoryPool* pool, MemoryBudget* budget, bool first_keys,
    std::size_t partitions, const KeyTableOptions& options, std::uint32_t threshold) {
  return KeyTableNoBadAlloc([&] -> arrow::Result<std::unique_ptr<CardinalityLocalGroups>> {
    if (!std::has_single_bit(partitions) || partitions > GroupTable::kPartitions) {
      return arrow::Status::Invalid("unsupported local group partition count: ", partitions);
    }
    MemoryReservation metadata;
    metadata.Reset(budget == nullptr ? dynamic_cast<MemoryBudget*>(pool) : budget);
    ARROW_RETURN_NOT_OK(metadata.Resize(sizeof(CardinalityLocalGroups)));
    // An extra floating-point addition during migration could change a DOUBLE sum's bits.
    // A NULL group, integer/decimal SUM and AVG, COUNT and fixed-width extrema all transfer
    // exactly by merging the sole source state into a new group.
    const bool can_promote = std::ranges::none_of(calls, [](const plan::AggregateCall& call) {
      return (call.kind == plan::AggKind::kSum || call.kind == plan::AggKind::kAvg) && call.arg &&
             call.arg->type == plan::LogicalType::kDouble;
    });
    auto out = std::unique_ptr<CardinalityLocalGroups>(new CardinalityLocalGroups(
        std::move(metadata), input_width, options, threshold, can_promote));
    ARROW_ASSIGN_OR_RAISE(out->single_,
                          GroupTable::MakeLocal(keys, calls, input_width, pool, budget, first_keys,
                                                partitions, options));
    return out;
  });
}

arrow::Status CardinalityLocalGroups::Promote() {
  ARROW_RETURN_NOT_OK(single_->Partition());
  ARROW_ASSIGN_OR_RAISE(auto lanes, PartitionedGroupTables::Make(
                                        single_->keys_, single_->aggregates_, input_width_,
                                        single_->pool_, single_->memory_.budget(),
                                        single_->keep_first_keys_, single_->partitions_, options_));
  // Keep the frozen source charged and alive through the last transfer. A refusal destroys the
  // temporary lanes and the caller retains the error instead of publishing a partial group set.
  ARROW_RETURN_NOT_OK(lanes->Seed(*single_));
  promoted_groups_ = single_->num_groups();
  partitioned_ = std::move(lanes);
  single_.reset();
  return arrow::Status::OK();
}

arrow::Status CardinalityLocalGroups::Consume(const arrow::RecordBatch& rows,
                                              const arrow::BooleanArray* selection,
                                              std::chrono::nanoseconds* dispatch_elapsed,
                                              std::chrono::nanoseconds* promotion_elapsed) {
  ARROW_RETURN_NOT_OK(error_);
  if (!single_ && !partitioned_) {
    return arrow::Status::Invalid("update of a finished cardinality-based grouping");
  }
  error_ = KeyTableNoBadAlloc([&] {
    return VisitInput(
        rows, selection,
        [&](const arrow::RecordBatch& batch, const arrow::BooleanArray* mask) -> arrow::Status {
          if (single_ && can_promote_ && single_->num_groups() > threshold_) {
            if (batch.num_rows() == 0) {
              return arrow::Status::OK();
            }
            if (mask != nullptr) {
              std::int64_t row = 0;
              while (row < mask->length() && (!mask->IsValid(row) || !mask->Value(row))) {
                ++row;
              }
              if (row == mask->length()) {
                return arrow::Status::OK();
              }
            }
            using Clock = std::chrono::steady_clock;
            const auto start = promotion_elapsed != nullptr ? Clock::now() : Clock::time_point{};
            ARROW_RETURN_NOT_OK(Promote());
            if (promotion_elapsed != nullptr) {
              *promotion_elapsed += Clock::now() - start;
            }
          }
          return partitioned_ ? partitioned_->Consume(batch, mask, dispatch_elapsed)
                              : single_->ConsumeSelected(batch, mask);
        });
  });
  return error_;
}

std::uint64_t CardinalityLocalGroups::num_groups() const {
  if (single_) {
    return single_->num_groups();
  }
  return partitioned_ ? partitioned_->num_groups() : finished_groups_;
}

arrow::Status CardinalityLocalGroups::Finish(std::unique_ptr<GroupTable>& single,
                                             std::unique_ptr<PartitionedGroupTables>& partitioned) {
  ARROW_RETURN_NOT_OK(error_);
  if (!single_ && !partitioned_) {
    return arrow::Status::Invalid("cardinality-based grouping already finished");
  }
  error_ = partitioned_ ? partitioned_->Finish() : single_->Partition();
  ARROW_RETURN_NOT_OK(error_);
  finished_groups_ = num_groups();
  single = std::move(single_);
  partitioned = std::move(partitioned_);
  return arrow::Status::OK();
}

}  // namespace antb1::exec
