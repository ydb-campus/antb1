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
  explicit Specification(arrow::MemoryPool* pool) : ids(pool), groups(pool) {}
  std::vector<std::size_t> calls;
  PoolVector<std::uint32_t> ids;  // input row -> group
  PoolVector<RowRef> groups;      // group -> finalized chunk/row
  std::vector<Results> results;
};

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
  for (const auto& call : calls_) {
    if (call.kind != plan::WindowCall::Kind::kAggregate ||
        call.frame != plan::WindowCall::Frame::kWholePartition || !call.frame_omitted ||
        !call.order_by.empty() || call.aggregate == plan::AggKind::kCountDistinct) {
      return plan::UnsupportedError("W3–W10: deferred window execution", call.over_span);
    }
    if (call.arguments.size() != (call.aggregate == plan::AggKind::kCountStar ? 0U : 1U)) {
      return arrow::Status::Invalid("window: wrong aggregate argument count");
    }
    if (!call.arguments.empty() && call.arguments.front().type == plan::LogicalType::kBoolean) {
      return plan::UnsupportedError("W11b: BOOLEAN window arguments are not supported", call.span);
    }
  }
  if (ctx.batch_size <= 0) {
    return arrow::Status::Invalid("window: batch size must be positive");
  }
  try {
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
      return SamePartition(calls_[spec.calls.front()], calls_[i]);
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
      for (std::size_t i = 0; i < count; ++i) {
        s.refs[i] = spec.groups[spec.ids[s.offset + i]];
      }
      for (std::size_t i = 0; i < spec.calls.size(); ++i) {
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
