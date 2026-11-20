#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <span>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/compute/api_vector.h>

#include "antb1/common/check.h"
#include "antb1/common/narrow.h"
#include "antb1/plan/types.h"

#include "group_table.h"

namespace antb1::exec {

const StoredKeyTable* GroupTable::StoredKeys() const {
  return frozen_ ? frozen_->keys : stored_keys_;
}

arrow::Status GroupTable::ConsumeProgram(const arrow::RecordBatch& rows,
                                         const arrow::compute::ExecBatch& keys,
                                         std::span<const std::uint32_t> selection) {
  ANTB1_CHECK(program_.has_value());
  ARROW_ASSIGN_OR_RAISE(const auto bound, program_->Bind(rows));
  std::uint32_t groups = num_groups_;
  // For DOUBLE keys remember only the rows that introduced groups, so the output preserves
  // first-seen -0 and NaN payloads. Every other eligible key can be emitted from the index.
  arrow::Int64Builder first_rows(pool_);
  const auto update = [&](std::size_t offset, std::span<const std::uint32_t> ids,
                          std::uint32_t after) {
    if (after != groups) {
      if (KeepsKeyChunks()) {
        ARROW_RETURN_NOT_OK(first_rows.Reserve(after - groups));
        for (std::size_t i = 0; i < ids.size(); ++i) {
          if (ids[i] == groups) {
            first_rows.UnsafeAppend(selection.empty() ? Narrow<std::int64_t>(offset + i)
                                                      : selection[offset + i]);
            ++groups;
          }
        }
        ANTB1_DCHECK(groups == after);
      }
      ARROW_RETURN_NOT_OK(ResizeStates(after));
      groups = after;
    }
    return selection.empty() ? bound.Consume(offset, ids)
                             : bound.ConsumeSelected(selection.subspan(offset, ids.size()), ids);
  };
  ARROW_RETURN_NOT_OK(selection.empty() ? stored_keys_->ConsumeChunks(keys, update)
                                        : stored_keys_->ConsumeSelected(keys, selection, update));
  if (groups > num_groups_) {
    arrow::ArrayVector first;
    if (KeepsKeyChunks()) {
      ARROW_ASSIGN_OR_RAISE(const auto positions, first_rows.Finish());
      first.reserve(keys_.size());
      for (const auto& key : keys_) {
        ARROW_ASSIGN_OR_RAISE(
            auto taken,
            arrow::compute::Take(rows.column(key.index), positions,
                                 arrow::compute::TakeOptions::NoBoundsCheck(), kernels_.get()));
        first.push_back(taken.make_array());
      }
    }
    ARROW_RETURN_NOT_OK(AddGroups(groups, std::move(first)));
  }
  return Account();
}

std::size_t GroupTable::num_partitions() const {
  return frozen_ ? frozen_->partitions.size() : partition_groups_.size();
}

std::span<const std::uint32_t> GroupTable::partition_groups(std::size_t partition) const {
  if (frozen_) {
    return frozen_->partitions.at(partition);
  }
  return partition_groups_.at(partition);
}

arrow::Status GroupTable::FreezePartitions() {
  FrozenGroups frozen{
      .owner = nullptr,
      .keys = stored_keys_,
      .partitions = decltype(frozen.partitions)(arrow::stl::allocator<StoredPartition>(pool_))};
  frozen.partitions.reserve(partitions_);
  for (std::size_t i = 0; i < partitions_; ++i) {
    frozen.partitions.emplace_back(arrow::stl::allocator<std::uint32_t>(pool_));
  }
  // Only IDs cross the handoff. The source index and typed states stay owned by the finished
  // part. Different lanes can read them concurrently without touching the source's probe scratch.
  std::array<std::uint64_t, 256> hashes;
  for (std::uint32_t first = 0; first < num_groups_;) {
    const auto count = std::min(num_groups_ - first, Narrow<std::uint32_t>(hashes.size()));
    const auto chunk = std::span(hashes).first(count);
    stored_keys_->RoutingHashes(first, chunk);
    for (const auto hash : chunk) {
      frozen.partitions[PartitionOf(hash)].push_back(first++);
    }
  }
  frozen.owner = std::move(key_table_);
  frozen_.emplace(std::move(frozen));
  stored_keys_ = nullptr;
  return arrow::Status::OK();
}

// The matched layouts use identical destination calls for each 1,024 source groups.
// A whole lane supplies consecutive IDs, while an unsplit table supplies its frozen ID list.
arrow::Status GroupTable::MergeStoredChunks(
    const GroupTable& source, std::optional<std::span<const std::uint32_t>> selection) {
  std::array<std::uint32_t, 1024> consecutive;
  const auto length = selection ? selection->size() : source.num_groups_;
  for (std::size_t begin = 0; begin < length; begin += consecutive.size()) {
    const auto n = std::min(consecutive.size(), length - begin);
    std::span<const std::uint32_t> from;
    if (selection) {
      from = selection->subspan(begin, n);
    } else {
      const auto chunk = std::span(consecutive).first(n);
      std::ranges::iota(chunk, Narrow<std::uint32_t>(begin));
      from = chunk;
    }
    ARROW_RETURN_NOT_OK(MergeStoredGroups(source, from));
  }
  return arrow::Status::OK();
}

arrow::Status GroupTable::MergeStoredGroups(const GroupTable& source,
                                            std::span<const std::uint32_t> from) {
  std::uint32_t groups = num_groups_;
  // The source groups are unique. A new ID identifies the source row whose first-seen DOUBLE
  // representation must be copied. Fixed keys with identical identity and output need no copy.
  std::vector<std::uint32_t, arrow::stl::allocator<std::uint32_t>> first{
      arrow::stl::allocator<std::uint32_t>(pool_)};
  ARROW_RETURN_NOT_OK(stored_keys_->MergeStored(
      *source.StoredKeys(), from,
      [&](std::size_t offset, std::span<const std::uint32_t> ids, std::uint32_t after) {
        const auto selected = from.subspan(offset, ids.size());
        if (groups != after) {
          if (KeepsKeyChunks() && source.KeepsKeyChunks()) {
            for (std::size_t i = 0; i < ids.size(); ++i) {
              if (ids[i] == groups) {
                first.push_back(selected[i]);
                ++groups;
              }
            }
            ANTB1_DCHECK(groups == after);
          }
          ARROW_RETURN_NOT_OK(ResizeStates(after));
          groups = after;
        }
        return program_->MergeGroups(*source.program_, selected, ids);
      }));
  if (!first.empty()) {
    ARROW_RETURN_NOT_OK(AddPartKeyRows(source, first));
  } else if (groups > num_groups_) {
    arrow::ArrayVector columns;
    if (KeepsKeyChunks()) {
      // Explicit callers can omit first-seen keys on a source with DOUBLEs. That source offers
      // its normalized spelling, just as the ordinary merge of its Uniques() would.
      ARROW_ASSIGN_OR_RAISE(auto snapshot,
                            stored_keys_->StoredUniques(num_groups_, groups - num_groups_));
      for (const auto& value : snapshot.batch.values) {
        columns.push_back(value.make_array());
      }
    }
    ARROW_RETURN_NOT_OK(AddGroups(groups, std::move(columns)));
  }
  return Account();
}

namespace {

// RecordBatch::Make owns the boxed array vector and makes a second vector of ArrayData
// handles. The public batch may outlive the table; individual columns retain their own owners.
struct OutputMetadata {
  MemoryReservation vectors;
  std::shared_ptr<arrow::RecordBatch> batch;

  arrow::Status Charge(std::size_t boxed, std::size_t data) {
    constexpr auto kMax = static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max());
    constexpr auto kBoxed = sizeof(std::shared_ptr<arrow::Array>);
    constexpr auto kData = sizeof(std::shared_ptr<arrow::ArrayData>);
    if (boxed > kMax / kBoxed || data > (kMax - (boxed * kBoxed)) / kData) {
      return arrow::Status::CapacityError("output column metadata size overflow");
    }
    return vectors.Resize(Narrow<std::int64_t>((boxed * kBoxed) + (data * kData)));
  }
};

}  // namespace

arrow::Result<std::shared_ptr<arrow::RecordBatch>> GroupTable::NextOutputChunk(
    const std::shared_ptr<arrow::Schema>& schema) {
  if (stored_output_ ? next_group_ == num_groups_ : next_chunk_ >= chunk_groups_.size()) {
    return nullptr;
  }
  auto end = next_chunk_;
  const auto rows = OutputRows(end);
  const auto width = keys_.size() + states_.size();
  std::shared_ptr<OutputMetadata> output;
  if (program_) {
    output = std::allocate_shared<OutputMetadata>(arrow::stl::allocator<OutputMetadata>(pool_));
    output->vectors.Reset(memory_.budget());
    ARROW_RETURN_NOT_OK(output->Charge(width, width));
  }
  arrow::ArrayVector columns;
  columns.reserve(width);
  if (output) {
    ARROW_RETURN_NOT_OK(output->Charge(columns.capacity(), width));
  }
  if (stored_output_) {
    ARROW_ASSIGN_OR_RAISE(auto snapshot, StoredKeys()->StoredUniques(next_group_, rows));
    for (const auto& datum : snapshot.batch.values) {
      columns.push_back(datum.make_array());
    }
  } else {
    ARROW_RETURN_NOT_OK(AppendFirstKeyColumns(end, columns));
  }
  for (const auto& state : states_) {
    ARROW_ASSIGN_OR_RAISE(auto column, state->Finalize(next_group_, next_group_ + rows, pool_));
    columns.push_back(std::move(column));
  }
  auto batch = arrow::RecordBatch::Make(schema, rows, std::move(columns));
  if (output) {
    ARROW_RETURN_NOT_OK(
        output->Charge(batch->columns().capacity(), batch->column_data().capacity()));
    output->batch = batch;
    batch = std::shared_ptr<arrow::RecordBatch>(std::move(output), batch.get());
    if (!stored_output_ && !frozen_) {
      for (auto& chunks : first_keys_) {
        std::fill_n(chunks.begin() + static_cast<std::ptrdiff_t>(next_chunk_), end - next_chunk_,
                    nullptr);
      }
    }
  }
  next_chunk_ = end;
  next_group_ += rows;
  return batch;
}

}  // namespace antb1::exec
