#include "window.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/util/byte_size.h>

#include "antb1/common/narrow.h"
#include "antb1/exec/memory_budget.h"
#include "antb1/exec/sort.h"
#include "antb1/plan/sql_status.h"
#include "antb1/plan/types.h"

#include "gather.h"
#include "group_table.h"
#include "pool_vector.h"
#include "window_prefix.h"

namespace antb1::exec {
namespace {

using RowRef = SortBuffer::RowRef;
constexpr std::uint32_t kChunkRows = 64 * 1024;

arrow::Status Charge(MemoryReservation& memory, std::size_t count, std::size_t width) {
  const auto remaining =
      static_cast<std::size_t>(std::numeric_limits<int64_t>::max() - memory.bytes());
  if (count > remaining / width) {
    return arrow::Status::CapacityError("window container size overflow");
  }
  return memory.Resize(memory.bytes() + Narrow<int64_t>(count * width));
}

// Charge container growth before allocating it, including the brief coexistence of the old
// and new storage. Capacity is implementation-defined, so account for the actual allocation too.
template <class T>
arrow::Status Reserve(std::vector<T>& values, std::size_t size, MemoryReservation& memory) {
  if (size <= values.capacity()) {
    return arrow::Status::OK();
  }
  const std::size_t old = values.capacity();
  const std::size_t capacity = std::max(size, old + (old / 2));
  ARROW_RETURN_NOT_OK(Charge(memory, capacity, sizeof(T)));
  values.reserve(capacity);
  ARROW_RETURN_NOT_OK(Charge(memory, values.capacity() - capacity, sizeof(T)));
  return memory.Resize(memory.bytes() - Narrow<int64_t>(old * sizeof(T)));
}

bool SamePartition(const plan::WindowCall& a, const plan::WindowCall& b) {
  return std::ranges::equal(a.partition_by, b.partition_by,
                            [](const plan::BoundColumn& x, const plan::BoundColumn& y) {
                              return x.index == y.index && x.type == y.type;
                            });
}

bool SameSpecification(const plan::WindowCall& a, const plan::WindowCall& b) {
  const auto whole = [](const plan::WindowCall& call) {
    return call.frame == plan::WindowCall::Frame::kWholePartition;
  };
  return whole(a) == whole(b) && SamePartition(a, b) &&
         std::ranges::equal(a.order_by, b.order_by, [](const auto& x, const auto& y) {
           return x.column.index == y.column.index && x.column.type == y.column.type &&
                  x.descending == y.descending && x.nulls_first == y.nulls_first;
         });
}

// Arrow 26's ArrayData copy constructor is noexcept although it copies vectors. Array::Slice
// and RecordBatch::Slice use that constructor, terminating on a heap failure. Copy the buffer
// pointers as arguments to Make instead, where a failure can reach Next's error boundary.
arrow::Result<std::shared_ptr<arrow::Array>> SliceColumn(const arrow::Array& array, int64_t offset,
                                                         int64_t length) {
  const auto& data = *array.data();
  if (offset > std::numeric_limits<int64_t>::max() - data.offset) {
    return arrow::Status::CapacityError("window array offset overflow");
  }
  return arrow::MakeArray(arrow::ArrayData::Make(
      data.type, length, data.buffers, data.child_data, data.dictionary,
      array.null_count() == 0 ? 0 : arrow::kUnknownNullCount, data.offset + offset));
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> SliceBatch(
    const std::shared_ptr<arrow::RecordBatch>& batch, int64_t start, int64_t length) {
  length = std::min(length, batch->num_rows() - start);
  if (start == 0 && length == batch->num_rows()) {
    return batch;
  }
  arrow::ArrayVector columns;
  columns.reserve(static_cast<std::size_t>(batch->num_columns()));
  for (const auto& column : batch->columns()) {
    ARROW_ASSIGN_OR_RAISE(auto slice, SliceColumn(*column, start, length));
    columns.push_back(std::move(slice));
  }
  return arrow::RecordBatch::Make(batch->schema(), length, std::move(columns));
}

struct Results {
  std::vector<std::shared_ptr<arrow::Array>> chunks;
  std::vector<const arrow::Array*> arrays;
};

struct Specification {
  explicit Specification(arrow::MemoryPool* pool) : ids(pool), groups(pool), positions(pool) {}
  std::vector<std::size_t> calls;
  PoolVector<std::uint32_t> ids;  // input row -> group
  PoolVector<RowRef> groups;      // group -> finalized chunk/row
  PoolVector<RowRef> positions;   // input row -> running result chunk/row
  std::vector<Results> results;
  std::shared_ptr<arrow::Array> ranks;  // shared by identical RANK calls, in input row order
};

struct OrderedLayout {
  explicit OrderedLayout(arrow::MemoryPool* pool)
      : permutation(pool), offsets(pool), starts(pool) {}
  PoolVector<RowRef> permutation;
  PoolVector<std::size_t> offsets;
  PoolVector<std::uint8_t> starts;
  std::shared_ptr<arrow::Array> ranks;
};

arrow::Result<OrderedLayout> OrderRows(const plan::WindowCall& call,
                                       const arrow::RecordBatchVector& input, std::size_t rows,
                                       bool rank, const ExecContext& ctx) {
  OrderedLayout layout(ctx.pool);
  int64_t* ranks = nullptr;
  if (rank) {
    if (rows > static_cast<std::size_t>(std::numeric_limits<int64_t>::max()) / sizeof(int64_t)) {
      return arrow::Status::CapacityError("window rank result size overflow");
    }
    ARROW_ASSIGN_OR_RAISE(auto buffer,
                          arrow::AllocateBuffer(Narrow<int64_t>(rows * sizeof(int64_t)), ctx.pool));
    ranks = buffer->mutable_data_as<int64_t>();
    layout.ranks = std::make_shared<arrow::Int64Array>(Narrow<int64_t>(rows), std::move(buffer));
  }
  MemoryReservation scratch;
  scratch.Reset(ctx.budget);
  std::vector<plan::SortKey> keys;
  ARROW_RETURN_NOT_OK(Reserve(keys, call.partition_by.size() + call.order_by.size(), scratch));
  for (const auto& column : call.partition_by) {
    keys.push_back({.column = {.index = column.index, .type = column.type}});
  }
  ARROW_ASSIGN_OR_RAISE(const auto partition,
                        RowComparator::Make(input.front()->schema(), keys, &scratch));
  for (const auto& key : call.order_by) {
    keys.push_back({.column = {.index = key.column.index, .type = key.column.type},
                    .descending = key.descending,
                    .nulls_first = key.nulls_first});
  }
  ARROW_ASSIGN_OR_RAISE(const auto order,
                        RowComparator::Make(input.front()->schema(), keys, &scratch));
  std::vector<RowComparator::KeyArrays> arrays;
  ARROW_RETURN_NOT_OK(Reserve(arrays, input.size(), scratch));
  ARROW_RETURN_NOT_OK(layout.offsets.Resize(input.size()));
  ARROW_RETURN_NOT_OK(layout.permutation.Resize(rows));
  ARROW_RETURN_NOT_OK(layout.starts.Resize(rows));
  std::size_t offset = 0;
  for (std::size_t chunk = 0; chunk < input.size(); ++chunk) {
    const auto count = static_cast<std::size_t>(input[chunk]->num_rows());
    const auto chunk_id = TryNarrow<std::uint32_t>(chunk);
    if (!chunk_id || count > std::numeric_limits<std::uint32_t>::max()) {
      return arrow::Status::CapacityError("window sort row reference overflow");
    }
    layout.offsets[chunk] = offset;
    arrays.emplace_back();
    ARROW_RETURN_NOT_OK(Reserve(arrays.back(), keys.size(), scratch));
    for (const auto& key : keys) {
      arrays.back().push_back(input[chunk]->column(key.column.index).get());
    }
    for (std::size_t row = 0; row < count; ++row) {
      layout.permutation[offset++] = {.chunk = *chunk_id, .row = Narrow<std::uint32_t>(row)};
    }
  }
  // Row identity stabilizes traversal without stable_sort's unbudgeted allocation. It does
  // not participate in partition/peer comparisons, including when RANK shares this layout.
  if (!keys.empty()) {
    std::sort(layout.permutation.data(), layout.permutation.data() + rows, [&](RowRef a, RowRef b) {
      const int compare = order.Compare(arrays[a.chunk], a.row, arrays[b.chunk], b.row);
      if (compare != 0) {
        return compare < 0;
      }
      return a.chunk < b.chunk || (a.chunk == b.chunk && a.row < b.row);
    });
  }
  std::size_t partition_start = 0;
  int64_t current_rank = 1;
  for (std::size_t i = 0; i < rows; ++i) {
    const RowRef current = layout.permutation[i];
    bool start = i == 0;
    if (i != 0) {
      const RowRef previous = layout.permutation[i - 1];
      start = partition.Compare(arrays[previous.chunk], previous.row, arrays[current.chunk],
                                current.row) != 0;
      if (start) {
        partition_start = i;
        current_rank = 1;
      } else if (ranks != nullptr && order.Compare(arrays[previous.chunk], previous.row,
                                                   arrays[current.chunk], current.row) != 0) {
        current_rank = Narrow<int64_t>(i - partition_start + 1);
      }
    }
    layout.starts[i] = start ? 1 : 0;
    if (ranks != nullptr) {
      ranks[layout.offsets[current.chunk] + current.row] = current_rank;
    }
  }
  return layout;
}

arrow::Status PrepareOrdered(Specification& spec, const std::vector<plan::WindowCall>& calls,
                             const arrow::RecordBatchVector& input, std::size_t rows,
                             MemoryReservation& memory, const ExecContext& ctx) {
  const bool rank = std::ranges::any_of(
      spec.calls, [&](std::size_t i) { return calls[i].kind == plan::WindowCall::Kind::kRank; });
  const bool prefix = std::ranges::any_of(spec.calls, [&](std::size_t i) {
    return calls[i].frame == plan::WindowCall::Frame::kRowsPrefix;
  });
  const auto& first = calls[spec.calls.front()];
  if (!prefix && first.order_by.empty()) {
    // Unordered RANK needs neither partitions nor a permutation.
    arrow::Int64Builder builder(ctx.pool);
    ARROW_RETURN_NOT_OK(builder.Reserve(Narrow<int64_t>(rows)));
    for (std::size_t i = 0; i < rows; ++i) {
      builder.UnsafeAppend(1);
    }
    ARROW_ASSIGN_OR_RAISE(spec.ranks, builder.Finish());
    return arrow::Status::OK();
  }
  ARROW_ASSIGN_OR_RAISE(auto layout, OrderRows(first, input, rows, rank, ctx));
  spec.ranks = std::move(layout.ranks);
  if (!prefix) {
    return arrow::Status::OK();
  }
  ARROW_RETURN_NOT_OK(spec.positions.Resize(rows));
  ARROW_RETURN_NOT_OK(Reserve(spec.results, spec.calls.size(), memory));
  spec.results.resize(spec.calls.size());
  MemoryReservation scratch;
  scratch.Reset(ctx.budget);
  std::vector<WindowPrefixState> states;
  std::vector<WindowPrefixState> saved;
  std::vector<PoolVector<const arrow::Array*>> arguments;
  arrow::ArrayVector columns;
  ARROW_RETURN_NOT_OK(Reserve(states, spec.calls.size(), scratch));
  ARROW_RETURN_NOT_OK(Reserve(saved, spec.calls.size(), scratch));
  ARROW_RETURN_NOT_OK(Reserve(arguments, spec.calls.size(), scratch));
  ARROW_RETURN_NOT_OK(Reserve(columns, spec.calls.size(), scratch));
  states.resize(spec.calls.size());
  // Cache argument arrays once for every output range and capacity retry. The retained input
  // owns the arrays; prefix loops avoid RecordBatch's locking, reference-counted column accessor.
  for (const auto index : spec.calls) {
    auto& arrays = arguments.emplace_back(ctx.pool);
    const auto& call = calls[index];
    if (!call.arguments.empty()) {
      ARROW_RETURN_NOT_OK(arrays.Resize(input.size()));
      const auto column = Narrow<std::size_t>(call.arguments.front().index);
      for (std::size_t k = 0; k < input.size(); ++k) {
        arrays[k] = input[k]->columns()[column].get();
      }
    }
  }
  std::uint32_t chunk = 0;
  for (std::size_t begin = 0; begin < rows; ++chunk) {
    auto count = std::min<std::size_t>(kChunkRows, rows - begin);
    saved = states;
    while (true) {
      columns.clear();
      states = saved;
      arrow::Status status;
      for (std::size_t i = 0; i < spec.calls.size(); ++i) {
        const auto& call = calls[spec.calls[i]];
        if (call.kind == plan::WindowCall::Kind::kRank) {
          columns.push_back(nullptr);
          continue;
        }
        auto result = WindowPrefixes(call, {arguments[i].data(), arguments[i].size()},
                                     {layout.permutation.data() + begin, count},
                                     {layout.starts.data() + begin, count}, states[i], ctx.pool);
        if (!result.ok()) {
          status = result.status();
          break;
        }
        columns.push_back(*std::move(result));
      }
      if (status.ok()) {
        break;
      }
      if (!status.IsCapacityError() || count == 1) {
        return status;
      }
      count /= 2;
    }
    for (std::size_t i = 0; i < count; ++i) {
      const RowRef row = layout.permutation[begin + i];
      spec.positions[layout.offsets[row.chunk] + row.row] = {.chunk = chunk,
                                                             .row = Narrow<std::uint32_t>(i)};
    }
    for (std::size_t i = 0; i < columns.size(); ++i) {
      if (columns[i] == nullptr) {
        continue;
      }
      auto& result = spec.results[i];
      ARROW_RETURN_NOT_OK(Reserve(result.chunks, result.chunks.size() + 1, memory));
      ARROW_RETURN_NOT_OK(Reserve(result.arrays, result.arrays.size() + 1, memory));
      result.arrays.push_back(columns[i].get());
      result.chunks.push_back(std::move(columns[i]));
    }
    begin += count;
    if (begin != rows && chunk == std::numeric_limits<std::uint32_t>::max()) {
      return arrow::Status::CapacityError("window prefix result reference overflow");
    }
  }
  return arrow::Status::OK();
}

}  // namespace

struct WindowOperator::State {
  explicit State(ExecContext& context) : ctx(context), refs(context.pool) {
    memory.Reset(ctx.budget);
  }
  ExecContext ctx;
  MemoryReservation memory;
  arrow::RecordBatchVector input;
  std::vector<Specification> specifications;
  PoolVector<RowRef> refs;
  std::size_t rows = 0;
  std::size_t chunk = 0;
  std::size_t row = 0;
  std::size_t offset = 0;
  bool prepared = false;
};

WindowOperator::WindowOperator(std::unique_ptr<Operator> input, std::vector<plan::WindowCall> calls)
    : input_(std::move(input)), calls_(std::move(calls)) {
  auto fields = input_->output_schema()->fields();
  for (const auto& call : calls_) {
    fields.push_back(arrow::field(call.name, plan::ToArrow(call.type)));
  }
  schema_ = arrow::schema(std::move(fields));
}

WindowOperator::~WindowOperator() = default;

arrow::Status WindowOperator::Open(ExecContext& ctx) {
  ARROW_RETURN_NOT_OK(Close());
  done_ = false;
  try {
    const auto validate_column = [&](const plan::BoundColumn& column) {
      const auto& schema = input_->output_schema();
      if (column.index < 0 || column.index >= schema->num_fields() ||
          !schema->field(column.index)->type()->Equals(plan::ToArrow(column.type))) {
        return arrow::Status::Invalid("window: bound column does not match its input schema");
      }
      return arrow::Status::OK();
    };
    for (const auto& call : calls_) {
      if (call.kind == plan::WindowCall::Kind::kRank) {
        if (call.frame || !call.frame_omitted) {
          return plan::UnsupportedError("W8: explicit ranking frames are not supported",
                                        call.over_span);
        }
        if (!call.arguments.empty() || call.type != plan::LogicalType::kBigInt) {
          return arrow::Status::Invalid("window: invalid bound RANK call");
        }
      } else {
        const bool whole = call.frame == plan::WindowCall::Frame::kWholePartition &&
                           call.frame_omitted && call.order_by.empty();
        const bool prefix =
            call.frame == plan::WindowCall::Frame::kRowsPrefix && !call.frame_omitted;
        if ((!whole && !prefix) || call.aggregate == plan::AggKind::kCountDistinct) {
          return plan::UnsupportedError("W6–W10: deferred window execution", call.over_span);
        }
        if (call.arguments.size() != (call.aggregate == plan::AggKind::kCountStar ? 0U : 1U)) {
          return arrow::Status::Invalid("window: wrong aggregate argument count");
        }
      }
      if (!call.arguments.empty() && call.arguments.front().type == plan::LogicalType::kBoolean) {
        return plan::UnsupportedError("W11b: BOOLEAN window arguments are not supported",
                                      call.span);
      }
      if (std::ranges::any_of(
              call.partition_by,
              [](const auto& key) { return key.type == plan::LogicalType::kBoolean; }) ||
          std::ranges::any_of(call.order_by, [](const auto& key) {
            return key.column.type == plan::LogicalType::kBoolean;
          })) {
        return plan::UnsupportedError("W11b: BOOLEAN window keys are not supported",
                                      call.over_span);
      }
      for (const auto& argument : call.arguments) {
        ARROW_RETURN_NOT_OK(validate_column(argument));
      }
      for (const auto& key : call.partition_by) {
        ARROW_RETURN_NOT_OK(validate_column(key));
      }
      for (const auto& key : call.order_by) {
        ARROW_RETURN_NOT_OK(validate_column(key.column));
      }
      if (call.frame == plan::WindowCall::Frame::kRowsPrefix) {
        if (call.aggregate == plan::AggKind::kSumCounts ||
            call.aggregate == plan::AggKind::kSingleValue) {
          return arrow::Status::Invalid("window: internal aggregate is not a prefix function");
        }
        ARROW_RETURN_NOT_OK(
            MakeGroupedAggregateState(
                call.aggregate,
                call.arguments.empty() ? std::nullopt : std::optional(call.arguments.front().type),
                call.type, ctx.pool)
                .status());
      }
    }
    if (ctx.batch_size <= 0) {
      return arrow::Status::Invalid("window: batch size must be positive");
    }
    state_ = std::make_unique<State>(ctx);
    input_open_ = true;  // even a partially opened upstream must be closed on failure
    arrow::Status status = input_->Open(ctx);
    if (status.ok()) {
      return status;
    }
    const arrow::Status closed = Close();
    return status & closed;
  } catch (const std::bad_alloc&) {
    const arrow::Status closed = Close();
    return arrow::Status::OutOfMemory("opening a window") & closed;
  }
}

arrow::Status WindowOperator::Prepare() {
  State& s = *state_;
  while (true) {
    ARROW_ASSIGN_OR_RAISE(const Batch batch, input_->Next());
    if (batch.end()) {
      break;
    }
    ARROW_ASSIGN_OR_RAISE(auto rows, Materialize(batch, s.ctx.pool));
    if (rows->num_rows() == 0) {
      continue;
    }
    const auto count = static_cast<std::size_t>(rows->num_rows());
    if (count > static_cast<std::size_t>(std::numeric_limits<int64_t>::max()) - s.rows) {
      return arrow::Status::CapacityError("window row count overflow");
    }
    // Sources may hand us buffers allocated outside the query pool. Conservatively charge
    // retained input as well as pooled allocations, as other blocking operators do; no size cap.
    ARROW_RETURN_NOT_OK(
        Charge(s.memory, static_cast<std::size_t>(arrow::util::TotalBufferSize(*rows)), 1));
    ARROW_RETURN_NOT_OK(Reserve(s.input, s.input.size() + 1, s.memory));
    s.input.push_back(std::move(rows));
    s.rows += count;
  }
  input_open_ = false;
  ARROW_RETURN_NOT_OK(input_->Close());
  if (s.rows == 0) {
    s.prepared = true;
    return arrow::Status::OK();
  }
  for (std::size_t i = 0; i < calls_.size(); ++i) {
    auto at = std::ranges::find_if(s.specifications, [&](const Specification& spec) {
      return SameSpecification(calls_[spec.calls.front()], calls_[i]);
    });
    if (at == s.specifications.end()) {
      ARROW_RETURN_NOT_OK(Reserve(s.specifications, s.specifications.size() + 1, s.memory));
      s.specifications.emplace_back(s.ctx.pool);
      at = s.specifications.end() - 1;
    }
    ARROW_RETURN_NOT_OK(Reserve(at->calls, at->calls.size() + 1, s.memory));
    at->calls.push_back(i);
  }
  for (Specification& spec : s.specifications) {
    if (calls_[spec.calls.front()].frame != plan::WindowCall::Frame::kWholePartition) {
      ARROW_RETURN_NOT_OK(PrepareOrdered(spec, calls_, s.input, s.rows, s.memory, s.ctx));
      continue;
    }
    MemoryReservation scratch;
    scratch.Reset(s.ctx.budget);
    std::vector<plan::AggregateCall> aggregates;
    ARROW_RETURN_NOT_OK(Reserve(aggregates, spec.calls.size(), scratch));
    for (const std::size_t i : spec.calls) {
      const auto& call = calls_[i];
      aggregates.push_back(
          {.kind = call.aggregate,
           .arg = call.arguments.empty() ? std::nullopt : std::optional(call.arguments.front()),
           .type = call.type,
           .id = call.id,
           .span = call.span});
    }
    ARROW_ASSIGN_OR_RAISE(
        auto table,
        GroupTable::Make(calls_[spec.calls.front()].partition_by, std::move(aggregates),
                         input_->output_schema()->num_fields(), s.ctx.pool, s.ctx.budget,
                         /*first_keys=*/false));
    ARROW_RETURN_NOT_OK(spec.ids.Resize(s.rows));
    std::vector<std::uint32_t> ids;
    ARROW_RETURN_NOT_OK(Reserve(ids, std::min<std::size_t>(s.rows, kChunkRows), scratch));
    // GroupTable's no-key path also temporarily holds one group ID per input row.
    ARROW_RETURN_NOT_OK(Charge(scratch, ids.capacity(), sizeof(std::uint32_t)));
    std::size_t offset = 0;
    for (const auto& batch : s.input) {
      for (int64_t start = 0; start < batch->num_rows(); start += kChunkRows) {
        ARROW_ASSIGN_OR_RAISE(const auto piece, SliceBatch(batch, start, kChunkRows));
        ARROW_RETURN_NOT_OK(table->Consume(*piece, &ids));
        std::ranges::copy(ids, spec.ids.data() + offset);
        offset += ids.size();
      }
    }
    ARROW_RETURN_NOT_OK(table->CheckFinalize());
    ARROW_RETURN_NOT_OK(spec.groups.Resize(table->num_groups()));
    ARROW_RETURN_NOT_OK(Reserve(spec.results, spec.calls.size(), s.memory));
    spec.results.resize(spec.calls.size());
    // Finalize in bounded ranges. VARCHAR results can exceed a binary array's offset width
    // even with few groups; retry a smaller range without retaining partially finalized columns.
    arrow::ArrayVector columns;
    ARROW_RETURN_NOT_OK(Reserve(columns, spec.calls.size(), scratch));
    for (std::uint32_t begin = 0; begin < table->num_groups();) {
      auto count = std::min(kChunkRows, table->num_groups() - begin);
      while (true) {
        columns.clear();
        arrow::Status status;
        for (std::size_t i = 0; i < spec.calls.size(); ++i) {
          auto result = table->state(i).Finalize(begin, begin + count, s.ctx.pool);
          if (!result.ok()) {
            status = result.status();
            break;
          }
          columns.push_back(*std::move(result));
        }
        if (status.ok()) {
          break;
        }
        if (!status.IsCapacityError() || count == 1) {
          return status;
        }
        count /= 2;
      }
      const auto chunk = Narrow<std::uint32_t>(spec.results.front().chunks.size());
      for (std::uint32_t row = 0; row < count; ++row) {
        spec.groups[begin + row] = {.chunk = chunk, .row = row};
      }
      for (std::size_t i = 0; i < columns.size(); ++i) {
        Results& results = spec.results[i];
        ARROW_RETURN_NOT_OK(Reserve(results.chunks, results.chunks.size() + 1, s.memory));
        ARROW_RETURN_NOT_OK(Reserve(results.arrays, results.arrays.size() + 1, s.memory));
        results.arrays.push_back(columns[i].get());
        results.chunks.push_back(std::move(columns[i]));
      }
      begin += count;
    }
    // table and its aggregate states die here; only finalized chunks and row mappings remain.
  }
  s.prepared = true;
  return arrow::Status::OK();
}

arrow::Result<Batch> WindowOperator::NextBatch() {
  State& s = *state_;
  if (!s.prepared) {
    ARROW_RETURN_NOT_OK(Prepare());
  }
  if (s.chunk == s.input.size()) {
    return Batch{};
  }
  const auto& input = s.input[s.chunk];
  auto count =
      std::min({static_cast<std::size_t>(input->num_rows()) - s.row,
                static_cast<std::size_t>(s.ctx.batch_size), static_cast<std::size_t>(kChunkRows)});
  while (true) {
    ARROW_RETURN_NOT_OK(s.refs.Resize(count));
    MemoryReservation scratch;
    scratch.Reset(s.ctx.budget);
    arrow::ArrayVector columns;
    ARROW_RETURN_NOT_OK(Reserve(columns, static_cast<std::size_t>(schema_->num_fields()), scratch));
    columns.resize(static_cast<std::size_t>(schema_->num_fields()));
    const auto width = static_cast<std::size_t>(input->num_columns());
    for (std::size_t i = 0; i < width; ++i) {
      ARROW_ASSIGN_OR_RAISE(columns[i],
                            SliceColumn(*input->column(Narrow<int>(i)), Narrow<int64_t>(s.row),
                                        Narrow<int64_t>(count)));
    }
    arrow::Status status;
    for (const auto& spec : s.specifications) {
      if (spec.ranks) {
        ARROW_ASSIGN_OR_RAISE(const auto ranks, SliceColumn(*spec.ranks, Narrow<int64_t>(s.offset),
                                                            Narrow<int64_t>(count)));
        for (const auto call : spec.calls) {
          if (calls_[call].kind == plan::WindowCall::Kind::kRank) {
            columns[width + call] = ranks;
          }
        }
        if (spec.positions.empty()) {
          continue;
        }
      }
      for (std::size_t i = 0; i < count; ++i) {
        s.refs[i] = spec.positions.empty() ? spec.groups[spec.ids[s.offset + i]]
                                           : spec.positions[s.offset + i];
      }
      for (std::size_t i = 0; i < spec.calls.size(); ++i) {
        if (calls_[spec.calls[i]].kind == plan::WindowCall::Kind::kRank) {
          continue;
        }
        ARROW_ASSIGN_OR_RAISE(
            auto builder,
            arrow::MakeBuilder(plan::ToArrow(calls_[spec.calls[i]].type), s.ctx.pool));
        status = GatherRows<false, RowRef>(spec.results[i].arrays, std::span(s.refs.data(), count),
                                           *builder);
        if (!status.ok()) {
          break;
        }
        ARROW_ASSIGN_OR_RAISE(columns[width + spec.calls[i]], builder->Finish());
      }
      if (!status.ok()) {
        break;
      }
    }
    if (!status.ok()) {
      if (status.IsCapacityError() && count > 1) {
        count /= 2;
        continue;
      }
      return status;
    }
    auto batch = arrow::RecordBatch::Make(schema_, Narrow<int64_t>(count), std::move(columns));
    s.offset += count;
    s.row += count;
    if (std::cmp_equal(s.row, input->num_rows())) {
      s.input[s.chunk++].reset();
      s.row = 0;
    }
    return Batch{.data = std::move(batch), .selection = {}};
  }
}

arrow::Result<Batch> WindowOperator::Next() {
  if (state_ == nullptr) {
    return done_ ? arrow::Result<Batch>(Batch{})
                 : arrow::Status::Invalid("window: Next() before Open() or after Close()");
  }
  try {
    auto result = NextBatch();
    if (result.ok() && !result->end()) {
      return result;
    }
    const arrow::Status status = result.status();
    const arrow::Status closed = Close();
    done_ = status.ok() && closed.ok();
    ARROW_RETURN_NOT_OK(status & closed);
    return Batch{};
  } catch (const std::bad_alloc&) {
    const arrow::Status closed = Close();
    return arrow::Status::OutOfMemory("evaluating a window") & closed;
  }
}

arrow::Status WindowOperator::Close() {
  arrow::Status status;
  if (input_open_) {
    input_open_ = false;
    status = input_->Close();
  }
  state_.reset();
  done_ = false;
  return status;
}

}  // namespace antb1::exec
