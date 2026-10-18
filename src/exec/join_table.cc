#include "antb1/exec/join_table.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <numeric>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/compute/api_vector.h>
#include <arrow/compute/exec.h>

#include "antb1/common/check.h"
#include "antb1/common/narrow.h"
#include "antb1/exec/memory_budget.h"
#include "antb1/exec/operator.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

#include "group_table.h"
#include "partition_lanes.h"
#include "row_mask.h"

namespace antb1::exec {

// A join's partition count is its own: kJoinPartitions sizes std::arrays, and nothing has measured
// it against GroupTable::kPartitions, which is now 32. The two are independent -- a join partitions
// its build rows, a GROUP BY its groups -- so there is no longer an invariant to assert here.

namespace {

// The most rows a build holds: rows are referenced by 32-bit positions.
constexpr std::uint64_t kMaxRows = std::numeric_limits<std::uint32_t>::max();
// The low bits of a hash pick its partition, the bits above them its bucket.
constexpr unsigned kPartitionBits = 6;
static_assert((std::size_t{1} << kPartitionBits) == kJoinPartitions);
// How sparse the direct layout's index may be, in slots of 4 bytes per row of the build, and how
// much sparser it may be when the query can afford the memory. A filtered build keeps the whole key
// range it was selected from, so the move the join order makes to carry fewer rows -- reduce a
// relation before it joins -- is exactly what makes a key sparse, on the column it was densest on.
// The index stays the cheaper read far past the first bound and past any cache: at SF 100 a build
// of 40,045 rows spanning 1,000,000 (25 slots a row) answers 80M probes in 52ms direct against
// 884ms hashed, and one of 22.8M rows spanning 600M (26 slots a row, a 2.4 GB index) makes its
// query 17% faster. One dependent load beats a hash, a bucket load and a key compare whether or not
// either side fits in cache (ADR 0026's C5a).
constexpr std::uint64_t kDirectSlotsPerRow = 8;
// Past that the index is gigabytes the hashed layout would not spend, so the sparser rule is only
// taken when the budget can carry it: a build that would fail its budget on an index is better
// hashed than failed. At most an eighth of the limit and inside what is free (Affordable), and no
// bound at all when the query has no limit.
constexpr std::uint64_t kSparseDirectSlotsPerRow = 32;
constexpr int64_t kSparseDirectLimitShare = 8;

arrow::Status TooManyRows() {
  return arrow::Status::OutOfMemory("a join build of more than ", kMaxRows, " rows");
}

// body(), with std::bad_alloc (a container outside the budget's view) as OutOfMemory: no exception
// leaves the join table.
template <class Body>
auto NoBadAlloc(const char* what, const Body& body) -> decltype(body()) {
  try {
    return body();
  } catch (const std::bad_alloc&) {
    return arrow::Status::OutOfMemory("out of memory in ", what);
  }
}

// The bytes of a key value of `type`: its width, 0 for VARCHAR (binary).
int KeyWidth(const arrow::DataType& type) {
  return type.id() == arrow::Type::BINARY ? 0 : type.byte_width();
}

// Calls visit(T{}) with the C type of a direct key's values.
template <class Visit>
void VisitDirectType(plan::LogicalType type, const Visit& visit) {
  switch (type.id()) {
    case plan::LogicalType::kSmallInt:
      visit(std::int16_t{});
      break;
    case plan::LogicalType::kInteger:
    case plan::LogicalType::kDate:
      visit(std::int32_t{});
      break;
    case plan::LogicalType::kUSmallInt:
      visit(std::uint16_t{});
      break;
    default:  // BIGINT, TIMESTAMP
      visit(std::int64_t{});
      break;
  }
}

// Frees the memory of `values` (clear() keeps it).
template <class T>
void Free(std::vector<T>& values) {
  std::vector<T>().swap(values);
}

// Room for one more element in `values`, reserved on `memory` before it is allocated.
template <class T>
arrow::Status Room(std::vector<T>& values, MemoryReservation& memory) {
  if (values.size() < values.capacity()) {
    return arrow::Status::OK();
  }
  const std::size_t capacity = std::max<std::size_t>(16, 2 * values.capacity());
  ARROW_RETURN_NOT_OK(
      memory.Resize(memory.bytes() + Narrow<int64_t>((capacity - values.capacity()) * sizeof(T))));
  values.reserve(capacity);
  return arrow::Status::OK();
}

// Whether a sparse direct index of `bytes` is one this query may take: no more than a share of the
// limit, and inside what is still free. The second is what makes the first a bound at all -- a
// share of the limit alone is a share each of any number of joins building at once, which together
// are not bounded by it -- and it is the same reading of the same budget as OneAtATime's. The
// answer can go stale between asking and reserving, as OneAtATime's can; the reservation is the
// gate, and declining here is only what turns a build that would fail on its index into one that
// hashes instead.
bool Affordable(const MemoryBudget* budget, int64_t bytes) {
  if (budget == nullptr) {
    return true;
  }
  const std::optional<int64_t> limit = budget->limit();
  return !limit.has_value() ||
         (bytes <= *limit / kSparseDirectLimitShare && budget->bytes_allocated() + bytes <= *limit);
}

// Whether a build's partitions are built one at a time: the budget is under pressure, or would be
// with `extra` more bytes in use (what building every partition at once adds).
bool OneAtATime(const MemoryBudget* budget, int64_t extra) {
  if (budget == nullptr) {
    return false;
  }
  const std::optional<int64_t> limit = budget->limit();
  return limit.has_value() && budget->bytes_allocated() + extra > *limit / 2;
}

// The positions of the rows of `mask` (`rows` of them), as Take indices.
arrow::Result<std::shared_ptr<arrow::Array>> Positions(const RowMask& mask, int64_t rows,
                                                       arrow::MemoryPool* pool) {
  ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::Buffer> buffer,
                        arrow::AllocateBuffer(rows * int64_t{sizeof(int64_t)}, pool));
  const std::span<int64_t> positions = buffer->mutable_span_as<int64_t>();
  std::size_t next = 0;
  mask.ForEachRun([&](int64_t position, int64_t count) {
    for (int64_t row = position; row < position + count; ++row) {
      positions[next++] = row;
    }
  });
  return std::make_shared<arrow::Int64Array>(rows, std::move(buffer));
}

// The hashes (KeyHashes) of the rows of a RowMask: hashes[i] is the mask's i-th row's when
// `compact`, else hashes[row] is row `row`'s for every row.
struct RowHashes {
  std::vector<std::uint64_t> hashes;
  bool compact = false;
};

// The hashes of the rows of `mask` (`rows` of them) over `keys`, reserved on `scratch`. When the
// mask keeps at most a quarter of the rows, only those rows' keys are hashed, through a copy of
// them (one Take per key), which leaves every hash as it is.
arrow::Result<RowHashes> HashRows(std::span<const std::shared_ptr<arrow::Array>> keys,
                                  const RowMask& mask, int64_t rows, arrow::MemoryPool* pool,
                                  MemoryReservation& scratch) {
  const int64_t length = mask.length();
  const bool compact = rows <= length / 4;
  ARROW_RETURN_NOT_OK(scratch.Resize(scratch.bytes() +
                                     ((compact ? rows : length) * int64_t{sizeof(std::uint64_t)})));
  std::vector<arrow::Datum> columns;
  columns.reserve(keys.size());
  if (!compact) {
    for (const std::shared_ptr<arrow::Array>& key : keys) {
      columns.emplace_back(key);
    }
    ARROW_ASSIGN_OR_RAISE(std::vector<std::uint64_t> hashes,
                          KeyHashes(arrow::compute::ExecBatch(std::move(columns), length)));
    return RowHashes{.hashes = std::move(hashes), .compact = false};
  }
  ARROW_ASSIGN_OR_RAISE(const std::shared_ptr<arrow::Array> positions, Positions(mask, rows, pool));
  arrow::compute::ExecContext kernels(pool);
  for (const std::shared_ptr<arrow::Array>& key : keys) {
    ARROW_ASSIGN_OR_RAISE(
        arrow::Datum taken,
        arrow::compute::Take(key, positions, arrow::compute::TakeOptions::NoBoundsCheck(),
                             &kernels));
    columns.push_back(std::move(taken));
  }
  ARROW_ASSIGN_OR_RAISE(std::vector<std::uint64_t> hashes,
                        KeyHashes(arrow::compute::ExecBatch(std::move(columns), rows)));
  return RowHashes{.hashes = std::move(hashes), .compact = true};
}

// Calls visit(row, hash) for every row of `mask`, in order.
template <class Visit>
void ForEachRowHash(const RowMask& mask, const RowHashes& hashed, const Visit& visit) {
  std::size_t i = 0;
  mask.ForEachRun([&](int64_t position, int64_t count) {
    for (int64_t row = position; row < position + count; ++row) {
      const std::size_t at = hashed.compact ? i : static_cast<std::size_t>(row);
      ++i;
      visit(row, hashed.hashes[at]);
    }
  });
}

}  // namespace

// ---- JoinBuildSpec ----

JoinBuildSpec::JoinBuildSpec(std::shared_ptr<arrow::Schema> schema,
                             std::vector<plan::BoundColumn> keys, bool direct_candidate,
                             bool keep_null_keys)
    : schema_(std::move(schema)),
      keys_(std::move(keys)),
      direct_candidate_(direct_candidate),
      keep_null_keys_(keep_null_keys) {}

arrow::Result<std::shared_ptr<const JoinBuildSpec>> JoinBuildSpec::Make(
    std::shared_ptr<arrow::Schema> schema, std::vector<plan::BoundColumn> keys,
    bool keep_null_keys) {
  return NoBadAlloc("a join build", [&] -> arrow::Result<std::shared_ptr<const JoinBuildSpec>> {
    if (schema == nullptr) {
      return arrow::Status::Invalid("a join build without a schema");
    }
    if (keys.empty()) {
      return arrow::Status::Invalid("a join build without keys");
    }
    for (const plan::BoundColumn& key : keys) {
      if (key.index < 0 || key.index >= schema->num_fields()) {
        return arrow::Status::Invalid("join key outside its input");
      }
      if (key.type == plan::LogicalType::kDouble || key.type == plan::LogicalType::kBoolean) {
        return arrow::Status::Invalid("a join key of type ", plan::ToString(key.type));
      }
      const std::shared_ptr<arrow::DataType>& type = schema->field(key.index)->type();
      if (!type->Equals(plan::ToArrow(key.type))) {
        return arrow::Status::Invalid("join key of type ", type->ToString(), " declared as ",
                                      plan::ToString(key.type));
      }
    }
    bool direct = false;
    if (keys.size() == 1) {
      switch (keys.front().type.id()) {
        case plan::LogicalType::kSmallInt:
        case plan::LogicalType::kInteger:
        case plan::LogicalType::kBigInt:
        case plan::LogicalType::kUSmallInt:
        case plan::LogicalType::kDate:
        case plan::LogicalType::kTimestamp:
          direct = true;
          break;
        default:
          break;
      }
    }
    return std::shared_ptr<const JoinBuildSpec>(
        new JoinBuildSpec(std::move(schema), std::move(keys), direct, keep_null_keys));
  });
}

arrow::Result<std::shared_ptr<const JoinBuildSpec>> JoinBuildSpec::Keyless(
    std::shared_ptr<arrow::Schema> schema) {
  if (schema == nullptr) {
    return arrow::Status::Invalid("a join build without a schema");
  }
  return NoBadAlloc("a join build", [&] -> arrow::Result<std::shared_ptr<const JoinBuildSpec>> {
    return std::shared_ptr<const JoinBuildSpec>(new JoinBuildSpec(std::move(schema), {},
                                                                  /*direct_candidate=*/false,
                                                                  /*keep_null_keys=*/false));
  });
}

// ---- JoinBuildPart ----

JoinBuildPart::JoinBuildPart(std::shared_ptr<const JoinBuildSpec> spec, MemoryBudget* budget)
    : spec_(std::move(spec)), budget_(budget) {
  ANTB1_CHECK(spec_ != nullptr);
  memory_.Reset(budget);
}

JoinBuildPart::~JoinBuildPart() = default;

int64_t JoinBuildPart::MemoryUsage(std::size_t capacity, std::size_t null_capacity) const {
  return Narrow<int64_t>((capacity * sizeof(Piece)) +
                         (null_capacity * sizeof(std::shared_ptr<arrow::RecordBatch>))) +
         hash_bytes_;
}

arrow::Status JoinBuildPart::Append(const Batch& batch, arrow::MemoryPool* pool) {
  const std::size_t retained = null_pieces_.size();
  arrow::Status status = NoBadAlloc("a join build", [&] { return AppendRows(batch, pool); });
  if (!status.ok()) {
    // The rows whose keys are NULL are retained before the keyed ones, so a failure among those
    // leaves a batch of this very call behind -- and the part must be unchanged. Dropping it also
    // gives its Arrow buffers back, which the reservation below does not cover: they are charged to
    // the pool by the Take that made them.
    null_pieces_.resize(retained);
    // Gives back what the batch reserved (shrinking never fails); its rows were not kept. Both
    // capacities, or the reservation keeps the room the batch asked for in the other store.
    status &= memory_.Resize(MemoryUsage(pieces_.capacity(), null_pieces_.capacity()));
  }
  return status;
}

arrow::Status JoinBuildPart::AppendNullKeys(const Batch& batch, int64_t remaining,
                                            arrow::MemoryPool* pool) {
  if (!spec_->keep_null_keys() || remaining == 0) {
    return arrow::Status::OK();  // not asked for, or every selected row has its keys
  }
  const arrow::RecordBatch& data = *batch.data;
  const int64_t length = data.num_rows();
  // The rows that are selected, exactly: RowMask folds a selection's values and its validity, so a
  // NULL in a selection counts as false here just as it does in Batch::selected_rows. Walking the
  // selected rows and testing the keys costs O(selected); walking the gaps between the key mask's
  // own runs would cost O(length) and would have to re-derive what a selection's NULL means.
  //
  // The walk is a bitmap read per row, measured at 0.415 ns on this host, and it only happens when
  // a key really has a NULL -- `remaining` is 0 otherwise, which is the common case, including for
  // every TPC-H-derived query. The build it walks is also the small side by construction, since the
  // planner only moves to this side when the other is several times larger: 2.9 ms for a 7.1M-row
  // build, 0.01% of the query that build belongs to. Reading each key's validity bitmap in runs
  // instead would be faster still and is not worth the arithmetic in a path this exact.
  ARROW_ASSIGN_OR_RAISE(
      const RowMask selected,
      RowMask::Make(std::span<const arrow::Array* const>{}, batch.selection.get(), length, pool));
  ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::Buffer> index_buffer,
                        arrow::AllocateBuffer(remaining * int64_t{sizeof(std::uint32_t)}, pool));
  const std::span<std::uint32_t> indices = index_buffer->mutable_span_as<std::uint32_t>();
  int64_t found = 0;
  selected.ForEachRun([&](int64_t position, int64_t count) {
    for (int64_t row = position; row < position + count && found < remaining; ++row) {
      const bool null_key = std::ranges::any_of(spec_->keys(), [&](const plan::BoundColumn& key) {
        return data.column(key.index)->IsNull(row);
      });
      if (null_key) {
        indices[static_cast<std::size_t>(found)] = Narrow<std::uint32_t>(row);
        ++found;
      }
    }
  });
  // `remaining` came from the mask, which counts the same rows: a mismatch is a programming error
  // in one of the two, and emitting a short batch would silently drop build rows.
  ANTB1_CHECK(found == remaining);
  const std::size_t capacity = null_pieces_.size() < null_pieces_.capacity()
                                   ? null_pieces_.capacity()
                                   : std::max<std::size_t>(4, 2 * null_pieces_.capacity());
  ARROW_RETURN_NOT_OK(memory_.Resize(MemoryUsage(pieces_.capacity(), capacity)));
  null_pieces_.reserve(capacity);
  arrow::compute::ExecContext kernels(pool);
  ARROW_ASSIGN_OR_RAISE(
      const arrow::Datum taken,
      arrow::compute::Take(batch.data,
                           std::make_shared<arrow::UInt32Array>(remaining, index_buffer),
                           arrow::compute::TakeOptions::NoBoundsCheck(), &kernels));
  null_pieces_.push_back(taken.record_batch());
  return arrow::Status::OK();
}

arrow::Status JoinBuildPart::AppendRows(const Batch& batch, arrow::MemoryPool* pool) {
  if (batch.end()) {
    return arrow::Status::Invalid("the end of a join build input appended as rows");
  }
  const arrow::RecordBatch& data = *batch.data;
  if (!data.schema()->Equals(*spec_->schema(), /*check_metadata=*/false)) {
    return arrow::Status::Invalid("join build input with another schema");
  }
  const int64_t length = data.num_rows();
  if (batch.selection != nullptr && batch.selection->length() != length) {
    return arrow::Status::Invalid("a join build selection of ", batch.selection->length(),
                                  " rows over ", length, " rows");
  }
  const int64_t selected = batch.selected_rows();
  if (selected == 0) {
    return arrow::Status::OK();
  }
  if (std::cmp_greater(length, kMaxRows)) {
    return TooManyRows();
  }
  std::vector<std::shared_ptr<arrow::Array>> keys;
  std::vector<const arrow::Array*> key_arrays;
  keys.reserve(spec_->keys().size());
  key_arrays.reserve(spec_->keys().size());
  for (const plan::BoundColumn& key : spec_->keys()) {
    keys.push_back(data.column(key.index));
    key_arrays.push_back(keys.back().get());
  }
  ARROW_ASSIGN_OR_RAISE(const RowMask mask,
                        RowMask::Make(key_arrays, batch.selection.get(), length, pool));
  const int64_t kept = mask.CountRows();
  if (kept == 0) {
    // Every selected row has a NULL key. The counters move only after the last fallible call, so a
    // failed retention leaves the part unchanged.
    ARROW_RETURN_NOT_OK(AppendNullKeys(batch, selected, pool));
    input_rows_ += selected;
    null_key_rows_ += selected;
    return arrow::Status::OK();
  }
  // Before the keyed rows, so that a failure here leaves no piece behind either. The rows with a
  // NULL key are the selected ones the mask dropped; `kept == selected` means there are none and
  // nothing is walked.
  ARROW_RETURN_NOT_OK(AppendNullKeys(batch, selected - kept, pool));
  MemoryReservation scratch;  // the hashes of the batch, given back on return
  scratch.Reset(budget_);
  ARROW_ASSIGN_OR_RAISE(const RowHashes hashed, HashRows(keys, mask, kept, pool, scratch));

  // The rows in partition order (a stable counting sort), with one Take of every column.
  Piece piece;
  ForEachRowHash(mask, hashed, [&piece](int64_t /*row*/, std::uint64_t hash) {
    ++piece.start[(hash % kJoinPartitions) + 1];
  });
  std::partial_sum(piece.start.begin(), piece.start.end(), piece.start.begin());
  const std::size_t capacity = pieces_.size() < pieces_.capacity()
                                   ? pieces_.capacity()
                                   : std::max<std::size_t>(4, 2 * pieces_.capacity());
  const int64_t hash_bytes = kept * int64_t{sizeof(std::uint64_t)};
  ARROW_RETURN_NOT_OK(memory_.Resize(MemoryUsage(capacity, null_pieces_.capacity()) + hash_bytes));
  pieces_.reserve(capacity);
  piece.hashes.resize(static_cast<std::size_t>(kept));
  ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::Buffer> index_buffer,
                        arrow::AllocateBuffer(kept * int64_t{sizeof(std::uint32_t)}, pool));
  const std::span<std::uint32_t> indices = index_buffer->mutable_span_as<std::uint32_t>();
  std::array<std::uint32_t, kJoinPartitions> next{};
  std::ranges::copy(std::span(piece.start).first<kJoinPartitions>(), next.begin());
  ForEachRowHash(mask, hashed, [&](int64_t row, std::uint64_t hash) {
    const std::uint32_t at = next[hash % kJoinPartitions]++;
    indices[at] = Narrow<std::uint32_t>(row);
    piece.hashes[at] = hash;
  });
  arrow::compute::ExecContext kernels(pool);
  ARROW_ASSIGN_OR_RAISE(
      const arrow::Datum taken,
      arrow::compute::Take(batch.data, std::make_shared<arrow::UInt32Array>(kept, index_buffer),
                           arrow::compute::TakeOptions::NoBoundsCheck(), &kernels));
  piece.rows = taken.record_batch();

  // The smallest and largest key of a direct candidate (every kept key is a value).
  int64_t lowest = 0;
  int64_t highest = 0;
  if (spec_->direct_candidate()) {
    const plan::BoundColumn& key = spec_->keys().front();
    const arrow::ArrayData& values = *piece.rows->column_data(key.index);
    VisitDirectType(key.type, [&]<class T>(T /*type*/) {
      const std::span<const T> taken_keys(values.GetValues<T>(1), static_cast<std::size_t>(kept));
      const auto [low, high] = std::ranges::minmax(taken_keys);
      lowest = low;
      highest = high;
    });
    if (num_rows_ > 0) {
      lowest = std::min(lowest, min_key_);
      highest = std::max(highest, max_key_);
    }
  }
  min_key_ = lowest;
  max_key_ = highest;
  pieces_.push_back(std::move(piece));
  hash_bytes_ += hash_bytes;
  input_rows_ += selected;
  null_key_rows_ += selected - kept;
  num_rows_ += kept;
  return arrow::Status::OK();
}

// ---- JoinTable ----

arrow::Result<std::unique_ptr<JoinMarks>> JoinMarks::Make(int64_t rows, MemoryBudget* budget) {
  ANTB1_CHECK(rows >= 0);
  auto marks = std::unique_ptr<JoinMarks>(new JoinMarks());
  marks->memory_.Reset(budget);
  const int64_t words = (rows + 63) / 64;
  ARROW_RETURN_NOT_OK(marks->memory_.Resize(words * static_cast<int64_t>(sizeof(std::uint64_t))));
  // Value-initialized, so every bit starts clear.
  marks->words_ = std::vector<std::atomic<std::uint64_t>>(static_cast<std::size_t>(words));
  marks->rows_ = rows;
  return marks;
}

void JoinMarks::Mark(std::uint32_t row) {
  ANTB1_CHECK(std::cmp_less(row, rows_));
  std::atomic<std::uint64_t>& word = words_[row / 64];
  const std::uint64_t bit = std::uint64_t{1} << (row % 64);
  // Most probe rows of a right-direction join share a build row with an earlier one -- that is why
  // the join builds on this side -- so the load spares the read-modify-write almost every time.
  // Measured 2026-10-15 against marking unconditionally: 10x to 69x faster where the build holds 1
  // to 64 rows and 4 to 8 threads mark it, because once a bit is set every later mark is a shared
  // read that invalidates nobody's cache line, where an unconditional fetch_or takes the line
  // exclusive and ping-pongs it. A small build is the common case here, since the planner only
  // moves to this side when the other one is at least `build_side_ratio` times larger. The cost is
  // a load that misses where marks rarely repeat: on Q21 at SF 100 that is about 2%, inside the
  // noise band, at both 4 and 8 threads.
  if ((word.load(kOrder) & bit) == 0) {
    word.fetch_or(bit, kOrder);
  }
}

bool JoinMarks::marked(std::uint32_t row) const {
  ANTB1_CHECK(std::cmp_less(row, rows_));
  return (words_[row / 64].load(kOrder) & (std::uint64_t{1} << (row % 64))) != 0;
}

JoinTable::JoinTable(std::shared_ptr<const JoinBuildSpec> spec, MemoryBudget* budget)
    : spec_(std::move(spec)), budget_(budget) {
  memory_.Reset(budget);
  for (Directory& directory : directories_) {
    directory.memory.Reset(budget);
  }
}

JoinTable::~JoinTable() = default;

JoinTable::KeyView JoinTable::ViewOf(const arrow::ArrayData& data, int width) {
  if (width > 0) {
    return KeyView{.values = data.GetValues<std::uint8_t>(1, data.offset * width),
                   .offsets = nullptr};
  }
  return KeyView{.values = data.GetValues<std::uint8_t>(2, 0),
                 .offsets = data.GetValues<std::int32_t>(1)};
}

std::span<const JoinTable::KeyView> JoinTable::ChunkKeys(std::uint32_t chunk) const {
  const std::size_t keys = widths_.size();
  return std::span<const KeyView>(chunk_keys_).subspan(chunk * keys, keys);
}

bool JoinTable::KeysEqual(std::span<const KeyView> keys, int64_t row, JoinRowRef ref) const {
  const std::span<const KeyView> build = ChunkKeys(ref.chunk);
  const int64_t build_row = ref.row;
  for (std::size_t k = 0; k < keys.size(); ++k) {
    const KeyView& a = keys[k];
    const KeyView& b = build[k];
    const int width = widths_[k];
    if (width > 0) {  // the same type on both sides: equal values have equal bytes
      if (std::memcmp(a.values + (row * width), b.values + (build_row * width),
                      static_cast<std::size_t>(width)) != 0) {
        return false;
      }
      continue;
    }
    const std::int32_t a_begin = a.offsets[row];
    const std::int32_t a_length = a.offsets[row + 1] - a_begin;
    const std::int32_t b_begin = b.offsets[build_row];
    const std::int32_t b_length = b.offsets[build_row + 1] - b_begin;
    if (a_length != b_length) {
      return false;
    }
    if (a_length > 0 && std::memcmp(a.values + a_begin, b.values + b_begin,
                                    static_cast<std::size_t>(a_length)) != 0) {
      return false;
    }
  }
  return true;
}

int64_t JoinTable::MemoryUsage() const {
  return Narrow<int64_t>((widths_.capacity() * sizeof(int)) +
                         (chunks_.capacity() * sizeof(std::shared_ptr<arrow::RecordBatch>)) +
                         (null_chunks_.capacity() * sizeof(std::shared_ptr<arrow::RecordBatch>)) +
                         (chunk_keys_.capacity() * sizeof(KeyView)) +
                         (rows_.capacity() * sizeof(JoinRowRef)) +
                         (offsets_.capacity() * sizeof(std::uint32_t)));
}

arrow::Status JoinTable::Build(std::span<const std::shared_ptr<const JoinBuildPart>> parts,
                               const Segments& segments, int64_t min_key, int64_t max_key,
                               arrow::internal::Executor* executor) {
  const std::vector<plan::BoundColumn>& keys = spec_->keys();
  std::size_t num_chunks = 0;
  std::size_t num_rows = 0;
  std::size_t num_null_chunks = 0;
  for (const std::shared_ptr<const JoinBuildPart>& part : parts) {
    num_chunks += part->pieces_.size();
    num_rows += static_cast<std::size_t>(part->num_rows_);
    num_null_chunks += part->null_pieces_.size();
  }
  // Direct for one dense integer key; the span is counted in 64 bits, so that keys from INT64_MIN
  // to INT64_MAX do not overflow it.
  const std::uint64_t span =
      static_cast<std::uint64_t>(max_key) - static_cast<std::uint64_t>(min_key);
  // The second clause only multiplies and adds inside the first's bound, so neither overflows.
  const bool direct = spec_->direct_candidate() && num_rows > 0 &&
                      (span < kDirectSlotsPerRow * num_rows ||
                       (span < kSparseDirectSlotsPerRow * num_rows &&
                        Affordable(budget_, Narrow<int64_t>((span + 3) * sizeof(std::uint32_t)))));
  layout_ = direct ? Layout::kDirect : Layout::kHashed;
  // Everything but the hashed directories, reserved before it is allocated (the counting of the
  // direct layout needs one offset more than it keeps).
  const std::uint64_t offsets = direct ? span + 3 : 0;
  const std::uint64_t bytes = (keys.size() * sizeof(int)) +
                              (num_chunks * (sizeof(std::shared_ptr<arrow::RecordBatch>) +
                                             (keys.size() * sizeof(KeyView)))) +
                              (num_null_chunks * sizeof(std::shared_ptr<arrow::RecordBatch>)) +
                              (num_rows * sizeof(JoinRowRef)) + (offsets * sizeof(std::uint32_t));
  ARROW_RETURN_NOT_OK(memory_.Resize(Narrow<int64_t>(bytes)));
  widths_.reserve(keys.size());
  for (const plan::BoundColumn& key : keys) {
    widths_.push_back(KeyWidth(*spec_->schema()->field(key.index)->type()));
  }
  chunks_.reserve(num_chunks);
  null_chunks_.reserve(num_null_chunks);
  chunk_keys_.reserve(num_chunks * keys.size());
  for (const std::shared_ptr<const JoinBuildPart>& part : parts) {
    for (const JoinBuildPart::Piece& piece : part->pieces_) {
      chunks_.push_back(piece.rows);
      for (std::size_t k = 0; k < keys.size(); ++k) {
        chunk_keys_.push_back(ViewOf(*piece.rows->column_data(keys[k].index), widths_[k]));
      }
    }
    // In the same part-ordered walk, so that the rows a tail emits from them are the same rows in
    // the same order for any number of threads. They get no chunk number and no key view: nothing
    // indexes or probes them.
    for (const std::shared_ptr<arrow::RecordBatch>& rows : part->null_pieces_) {
      null_chunks_.push_back(rows);
    }
  }
  rows_.resize(num_rows);
  if (direct) {
    min_key_ = min_key;
    span_ = span;
    offsets_.assign(offsets, 0);
    ARROW_RETURN_NOT_OK(BuildDirect(segments, OneAtATime(budget_, 0) ? nullptr : executor));
  } else {
    ARROW_RETURN_NOT_OK(BuildHashed(segments, executor));
  }
  return memory_.Resize(MemoryUsage());
}

arrow::Status JoinTable::BuildDirect(const Segments& segments,
                                     arrow::internal::Executor* executor) {
  const plan::BoundColumn& key = spec_->keys().front();
  // Calls visit(index, ref) for every row of partition `partition`, in order, where `index` is its
  // key minus the smallest key. A key lies in one partition, so tasks of different partitions
  // touch different offsets and rows.
  const auto for_each_row = [this, &key, &segments](std::size_t partition, const auto& visit) {
    VisitDirectType(key.type, [&]<class T>(T /*type*/) {
      for (const Segment& segment : segments[partition]) {
        const T* values = chunks_[segment.chunk]->column_data(key.index)->GetValues<T>(1);
        for (std::uint32_t row = segment.begin; row < segment.end; ++row) {
          const int64_t value = values[row];
          visit(static_cast<std::uint64_t>(value) - static_cast<std::uint64_t>(min_key_),
                JoinRowRef{.chunk = segment.chunk, .row = row});
        }
      }
    });
  };
  // Each key's rows counted at offsets_[index + 2]; after the prefix sums offsets_[index + 1] is
  // where its rows start, and placing them moves it to where they end.
  std::array<bool, kJoinPartitions> repeats{};
  ARROW_RETURN_NOT_OK(ForEach(executor, kJoinPartitions, [&](std::size_t partition) {
    for_each_row(partition, [&](std::uint64_t index, JoinRowRef /*ref*/) {
      const std::uint32_t count = ++offsets_[index + 2];
      if (count > 1) {
        repeats[partition] = true;
      }
    });
    return arrow::Status::OK();
  }));
  std::partial_sum(offsets_.begin(), offsets_.end(), offsets_.begin());
  ARROW_RETURN_NOT_OK(ForEach(executor, kJoinPartitions, [&](std::size_t partition) {
    for_each_row(partition,
                 [&](std::uint64_t index, JoinRowRef ref) { rows_[offsets_[index + 1]++] = ref; });
    return arrow::Status::OK();
  }));
  offsets_.pop_back();  // the spare entry of the counting
  unique_ = std::ranges::none_of(repeats, std::identity{});
  return arrow::Status::OK();
}

arrow::Status JoinTable::BuildHashed(const Segments& segments,
                                     arrow::internal::Executor* executor) {
  // Partition p's rows go to rows_[first[p], first[p + 1]).
  std::array<std::uint32_t, kJoinPartitions> first{};
  std::uint64_t rows = 0;
  int64_t parallel_bytes = 0;  // what building every partition at once holds at its peak
  for (std::size_t p = 0; p < kJoinPartitions; ++p) {
    first[p] = Narrow<std::uint32_t>(rows);
    std::uint64_t partition_rows = 0;
    for (const Segment& segment : segments[p]) {
      partition_rows += segment.end - segment.begin;
    }
    rows += partition_rows;
    if (partition_rows > 0) {
      parallel_bytes +=
          Narrow<int64_t>((partition_rows * sizeof(std::uint64_t)) +
                          ((std::bit_ceil(partition_rows) + 2) * sizeof(std::uint32_t)));
    }
  }
  std::array<bool, kJoinPartitions> repeats{};
  ARROW_RETURN_NOT_OK(
      ForEach(OneAtATime(budget_, parallel_bytes) ? nullptr : executor, kJoinPartitions,
              [&](std::size_t p) { return BuildDirectory(p, segments[p], first[p], repeats[p]); }));
  unique_ = std::ranges::none_of(repeats, std::identity{});
  return arrow::Status::OK();
}

arrow::Status JoinTable::BuildDirectory(std::size_t partition, std::span<const Segment> segments,
                                        std::uint32_t first, bool& repeats) {
  std::uint32_t n = 0;
  for (const Segment& segment : segments) {
    n += segment.end - segment.begin;
  }
  if (n == 0) {
    return arrow::Status::OK();
  }
  Directory& directory = directories_[partition];
  const std::uint64_t buckets = std::bit_ceil(std::uint64_t{n});
  directory.mask = buckets - 1;
  const auto bucket_of = [&directory](std::uint64_t hash) {
    return (hash >> kPartitionBits) & directory.mask;
  };
  // The bucket starts (two entries more while they are counted) and, until the slots are made,
  // every entry's hash.
  MemoryReservation scratch;
  scratch.Reset(budget_);
  ARROW_RETURN_NOT_OK(
      directory.memory.Resize(Narrow<int64_t>((buckets + 2) * sizeof(std::uint32_t))));
  ARROW_RETURN_NOT_OK(scratch.Resize(Narrow<int64_t>(n * sizeof(std::uint64_t))));
  std::vector<std::uint32_t>& starts = directory.buckets;
  starts.assign(buckets + 2, 0);
  std::vector<std::uint64_t> hashes(n);
  for (const Segment& segment : segments) {
    const std::uint32_t length = segment.end - segment.begin;
    for (std::uint32_t i = 0; i < length; ++i) {
      ++starts[bucket_of(segment.hashes[i]) + 2];
    }
  }
  std::partial_sum(starts.begin(), starts.end(), starts.begin());
  // The entries in bucket order, each bucket's in (part, row) order (a stable counting sort).
  for (const Segment& segment : segments) {
    const std::uint32_t length = segment.end - segment.begin;
    for (std::uint32_t i = 0; i < length; ++i) {
      const std::uint64_t hash = segment.hashes[i];
      const std::uint32_t at = starts[bucket_of(hash) + 1]++;
      rows_[first + at] = JoinRowRef{.chunk = segment.chunk, .row = segment.begin + i};
      hashes[at] = hash;
    }
  }
  // starts[b] is now bucket b's first entry. Each key's entries together, and the keys counted.
  std::vector<std::uint32_t> heads;
  std::size_t keys = 0;
  for (std::uint64_t b = 0; b < buckets; ++b) {
    const std::uint32_t begin = starts[b];
    const std::uint32_t size = starts[b + 1] - begin;
    if (size <= 1) {
      keys += size;
      continue;
    }
    ARROW_ASSIGN_OR_RAISE(
        const std::size_t bucket_keys,
        GroupBucket(first + begin, std::span(hashes).subspan(begin, size), heads, scratch));
    keys += bucket_keys;
  }
  // One slot per key, bucket by bucket; starts[b] becomes bucket b's first slot.
  ARROW_RETURN_NOT_OK(
      directory.memory.Resize(directory.memory.bytes() + Narrow<int64_t>(keys * sizeof(KeySlot))));
  directory.slots.reserve(keys);
  std::uint32_t begin = 0;
  for (std::uint64_t b = 0; b < buckets; ++b) {
    const std::uint32_t end = starts[b + 1];
    starts[b] = Narrow<std::uint32_t>(directory.slots.size());
    for (std::uint32_t at = begin; at < end;) {
      const JoinRowRef key = rows_[first + at];
      std::uint32_t stop = at + 1;
      while (stop < end && hashes[stop] == hashes[at] &&
             KeysEqual(ChunkKeys(key.chunk), key.row, rows_[first + stop])) {
        ++stop;
      }
      if (stop - at > 1) {
        repeats = true;
      }
      directory.slots.push_back(
          KeySlot{.hash = hashes[at], .begin = first + at, .end = first + stop});
      at = stop;
    }
    begin = end;
  }
  starts[buckets] = Narrow<std::uint32_t>(directory.slots.size());
  starts.pop_back();
  return directory.memory.Resize(Narrow<int64_t>((starts.capacity() * sizeof(std::uint32_t)) +
                                                 (directory.slots.capacity() * sizeof(KeySlot))));
}

arrow::Result<std::size_t> JoinTable::GroupBucket(std::uint32_t first,
                                                  std::span<std::uint64_t> hashes,
                                                  std::vector<std::uint32_t>& heads,
                                                  MemoryReservation& scratch) {
  const std::span<JoinRowRef> rows = std::span(rows_).subspan(first, hashes.size());
  const auto same = [&](std::size_t a, std::size_t b) {
    return hashes[a] == hashes[b] && KeysEqual(ChunkKeys(rows[a].chunk), rows[a].row, rows[b]);
  };
  // The key of entry i among the keys seen before it (heads: their first entries, in the order
  // they appear), starting with the key of the entry before it; nullopt for a new key.
  const auto key_of = [&](std::size_t i, std::size_t previous) -> std::optional<std::size_t> {
    if (same(i, heads[previous])) {
      return previous;
    }
    for (std::size_t k = 0; k < heads.size(); ++k) {
      if (k != previous && same(i, heads[k])) {
        return k;
      }
    }
    return std::nullopt;
  };
  // Usually each key's entries are together already (distinct keys, or a key's rows in a row).
  heads.clear();
  ARROW_RETURN_NOT_OK(Room(heads, scratch));
  heads.push_back(0);
  std::size_t current = 0;
  bool together = true;
  for (std::size_t i = 1; i < rows.size() && together; ++i) {
    const std::optional<std::size_t> key = key_of(i, current);
    if (!key.has_value()) {
      ARROW_RETURN_NOT_OK(Room(heads, scratch));
      heads.push_back(Narrow<std::uint32_t>(i));
      current = heads.size() - 1;
    } else if (*key != current) {
      together = false;
    }
  }
  if (together) {
    return heads.size();
  }
  // Otherwise a stable counting sort of the entries by key, through copies of them.
  MemoryReservation sort;
  sort.Reset(budget_);
  ARROW_RETURN_NOT_OK(sort.Resize(Narrow<int64_t>(
      rows.size() * (sizeof(std::uint32_t) + sizeof(JoinRowRef) + sizeof(std::uint64_t)))));
  std::vector<std::uint32_t> keys(rows.size());
  heads.resize(1);
  current = 0;
  for (std::size_t i = 1; i < rows.size(); ++i) {
    const std::optional<std::size_t> key = key_of(i, current);
    if (key.has_value()) {
      current = *key;
    } else {
      ARROW_RETURN_NOT_OK(Room(heads, scratch));
      heads.push_back(Narrow<std::uint32_t>(i));
      current = heads.size() - 1;
    }
    keys[i] = Narrow<std::uint32_t>(current);
  }
  ARROW_RETURN_NOT_OK(
      sort.Resize(sort.bytes() + Narrow<int64_t>((heads.size() + 1) * sizeof(std::uint32_t))));
  std::vector<std::uint32_t> start(heads.size() + 1, 0);
  for (const std::uint32_t key : keys) {
    ++start[key + 1];
  }
  std::partial_sum(start.begin(), start.end(), start.begin());
  std::vector<JoinRowRef> sorted_rows(rows.size());
  std::vector<std::uint64_t> sorted_hashes(rows.size());
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const std::uint32_t at = start[keys[i]]++;
    sorted_rows[at] = rows[i];
    sorted_hashes[at] = hashes[i];
  }
  std::ranges::copy(sorted_rows, rows.begin());
  std::ranges::copy(sorted_hashes, hashes.begin());
  return heads.size();
}

arrow::Status JoinTable::Find(std::span<const std::shared_ptr<arrow::Array>> keys,
                              const arrow::BooleanArray* selection, arrow::MemoryPool* pool,
                              std::span<JoinMatches> out) const {
  return NoBadAlloc("a join probe", [&] { return DoFind(keys, selection, pool, out); });
}

arrow::Status JoinTable::DoFind(std::span<const std::shared_ptr<arrow::Array>> keys,
                                const arrow::BooleanArray* selection, arrow::MemoryPool* pool,
                                std::span<JoinMatches> out) const {
  const std::vector<plan::BoundColumn>& build_keys = spec_->keys();
  if (build_keys.empty()) {  // before any key is read: there is none
    return arrow::Status::Invalid("a probe of a join build without keys");
  }
  if (keys.size() != build_keys.size()) {
    return arrow::Status::Invalid("a join probe of ", keys.size(), " keys for ", build_keys.size(),
                                  " build keys");
  }
  const int64_t length = keys.front() == nullptr ? 0 : keys.front()->length();
  for (std::size_t k = 0; k < keys.size(); ++k) {
    if (keys[k] == nullptr || keys[k]->length() != length) {
      return arrow::Status::Invalid("join probe keys of different lengths");
    }
    const std::shared_ptr<arrow::DataType>& type =
        spec_->schema()->field(build_keys[k].index)->type();
    if (!keys[k]->type()->Equals(*type)) {
      return arrow::Status::Invalid("a join probe key of type ", keys[k]->type()->ToString(),
                                    " for a key of type ", type->ToString());
    }
  }
  if (selection != nullptr && selection->length() != length) {
    return arrow::Status::Invalid("a join probe selection of ", selection->length(), " rows over ",
                                  length, " rows");
  }
  if (std::cmp_less(out.size(), length)) {
    return arrow::Status::Invalid("room for the matches of ", out.size(), " of ", length,
                                  " probe rows");
  }
  const std::span<JoinMatches> matches = out.first(static_cast<std::size_t>(length));
  std::ranges::fill(matches, JoinMatches{});
  if (length == 0 || rows_.empty()) {
    return arrow::Status::OK();
  }
  std::vector<const arrow::Array*> arrays;
  arrays.reserve(keys.size());
  for (const std::shared_ptr<arrow::Array>& key : keys) {
    arrays.push_back(key.get());
  }
  ARROW_ASSIGN_OR_RAISE(const RowMask mask, RowMask::Make(arrays, selection, length, pool));
  if (layout_ == Layout::kDirect) {
    const arrow::ArrayData& data = *keys.front()->data();
    VisitDirectType(build_keys.front().type, [&]<class T>(T /*type*/) {
      const T* values = data.GetValues<T>(1);
      mask.ForEachRun([&](int64_t position, int64_t count) {
        for (int64_t row = position; row < position + count; ++row) {
          const int64_t value = values[row];
          const std::uint64_t index =
              static_cast<std::uint64_t>(value) - static_cast<std::uint64_t>(min_key_);
          if (index <= span_ && offsets_[index] != offsets_[index + 1]) {
            matches[static_cast<std::size_t>(row)] =
                JoinMatches{.begin = offsets_[index], .end = offsets_[index + 1]};
          }
        }
      });
    });
    return arrow::Status::OK();
  }
  const int64_t kept = mask.CountRows();
  if (kept == 0) {
    return arrow::Status::OK();
  }
  MemoryReservation scratch;  // a probe's own: counted, not charged
  ARROW_ASSIGN_OR_RAISE(const RowHashes hashed, HashRows(keys, mask, kept, pool, scratch));
  std::vector<KeyView> views;
  views.reserve(keys.size());
  for (std::size_t k = 0; k < keys.size(); ++k) {
    views.push_back(ViewOf(*keys[k]->data(), widths_[k]));
  }
  ForEachRowHash(mask, hashed, [&](int64_t row, std::uint64_t hash) {
    const Directory& directory = directories_[hash % kJoinPartitions];
    if (directory.slots.empty()) {
      return;
    }
    const std::uint64_t bucket = (hash >> kPartitionBits) & directory.mask;
    for (std::uint32_t s = directory.buckets[bucket]; s < directory.buckets[bucket + 1]; ++s) {
      const KeySlot& slot = directory.slots[s];
      if (slot.hash == hash && KeysEqual(views, row, rows_[slot.begin])) {
        matches[static_cast<std::size_t>(row)] = JoinMatches{.begin = slot.begin, .end = slot.end};
        return;
      }
    }
  });
  return arrow::Status::OK();
}

// ---- JoinTableBuilder ----

JoinTableBuilder::JoinTableBuilder(std::shared_ptr<const JoinBuildSpec> spec, int64_t num_parts,
                                   arrow::internal::Executor* executor, MemoryBudget* budget)
    : spec_(std::move(spec)), num_parts_(num_parts), executor_(executor), budget_(budget) {}

JoinTableBuilder::~JoinTableBuilder() = default;

arrow::Result<std::unique_ptr<JoinTableBuilder>> JoinTableBuilder::Make(
    std::shared_ptr<const JoinBuildSpec> spec, int64_t num_parts,
    arrow::internal::Executor* executor, int64_t max_pending, MemoryBudget* budget) {
  if (spec == nullptr) {
    return arrow::Status::Invalid("a join build without a spec");
  }
  if (num_parts < 0) {
    return arrow::Status::Invalid("a join build of ", num_parts, " parts");
  }
  return NoBadAlloc("a join build", [&] -> arrow::Result<std::unique_ptr<JoinTableBuilder>> {
    std::unique_ptr<JoinTableBuilder> builder(
        new JoinTableBuilder(std::move(spec), num_parts, executor, budget));
    builder->memory_.Reset(budget);
    int64_t bytes = 0;
    if (__builtin_mul_overflow(num_parts, int64_t{sizeof(std::shared_ptr<const JoinBuildPart>)},
                               &bytes)) {
      return arrow::Status::OutOfMemory("a join build of ", num_parts, " parts");
    }
    ARROW_RETURN_NOT_OK(builder->memory_.Resize(bytes));
    builder->out_of_memory_ = arrow::Status::OutOfMemory("out of memory in a join build");
    builder->parts_.resize(static_cast<std::size_t>(num_parts));
    for (MemoryReservation& memory : builder->segment_memory_) {
      memory.Reset(budget);
    }
    builder->lanes_ =
        std::make_unique<PartitionLanes>(kJoinPartitions, executor, max_pending, budget);
    return builder;
  });
}

arrow::Status JoinTableBuilder::Add(int64_t part, std::shared_ptr<const JoinBuildPart> rows) {
  return NoBadAlloc("a join build", [&] -> arrow::Status {
    // The build's failure comes first. A merge that failed on the executor is one as soon as it is
    // known, whatever this part is: Merged() waits for the other merges and keeps the failure.
    if (failed_.ok() && lanes_->failed()) {
      return Merged();
    }
    ARROW_RETURN_NOT_OK(failed_);
    if (finished_) {
      return arrow::Status::Invalid("join build part ", part, " added after the build finished");
    }
    if (part < 0 || part >= num_parts_) {
      return arrow::Status::Invalid("join build part ", part, " of ", num_parts_, " parts");
    }
    if (rows == nullptr || rows->spec() != spec_) {
      return arrow::Status::Invalid("join build part ", part, " of another build");
    }
    std::shared_ptr<const JoinBuildPart>& slot = parts_[static_cast<std::size_t>(part)];
    if (slot != nullptr) {
      return arrow::Status::Invalid("join build part ", part, " added twice");
    }
    slot = std::move(rows);
    // Each part is released once, in order: it counts as released before Release runs. A release
    // that fails fails the build, std::bad_alloc included (out_of_memory_ was made ahead: nothing
    // is allocated here), so no exception leaves a part to release again or the build going on.
    while (next_part_ < num_parts_ && parts_[static_cast<std::size_t>(next_part_)] != nullptr) {
      const int64_t releasing = next_part_++;
      try {
        failed_ = Release(releasing);
      } catch (const std::bad_alloc&) {
        failed_ = std::move(out_of_memory_);  // once: the build stops at its first failure
      }
      if (!failed_.ok()) {
        return Merged();  // a merge of an earlier part may have failed: it comes first
      }
    }
    return arrow::Status::OK();
  });
}

arrow::Status JoinTableBuilder::Release(int64_t part) {
  const std::shared_ptr<const JoinBuildPart>& rows = parts_[static_cast<std::size_t>(part)];
  const std::uint64_t num_rows = num_rows_ + static_cast<std::uint64_t>(rows->num_rows_);
  if (num_rows > kMaxRows) {
    return TooManyRows();
  }
  // Every piece has a row: the chunks are no more than the rows.
  const auto first_chunk = Narrow<std::uint32_t>(num_chunks_);
  // The merge function is made before any count changes.
  PartitionLanes::Merge merge;
  if (!rows->pieces_.empty()) {
    merge = [this, rows, first_chunk](std::size_t partition) {
      return Merge(partition, *rows, first_chunk);
    };
  }
  if (rows->num_rows_ > 0) {
    min_key_ = num_rows_ == 0 ? rows->min_key_ : std::min(min_key_, rows->min_key_);
    max_key_ = num_rows_ == 0 ? rows->max_key_ : std::max(max_key_, rows->max_key_);
  }
  num_rows_ = num_rows;
  input_rows_ += rows->input_rows_;
  null_key_rows_ += rows->null_key_rows_;
  num_chunks_ += rows->pieces_.size();
  if (!merge) {
    return arrow::Status::OK();
  }
  return lanes_->Add(part, std::move(merge));
}

arrow::Status JoinTableBuilder::Merge(std::size_t partition, const JoinBuildPart& part,
                                      std::uint32_t first_chunk) {
  std::vector<JoinTable::Segment>& segments = segments_[partition];
  std::size_t added = 0;
  for (const JoinBuildPart::Piece& piece : part.pieces_) {
    if (piece.start[partition + 1] > piece.start[partition]) {
      ++added;
    }
  }
  if (segments.size() + added > segments.capacity()) {
    const std::size_t capacity = std::max(segments.size() + added, 2 * segments.capacity());
    ARROW_RETURN_NOT_OK(
        segment_memory_[partition].Resize(Narrow<int64_t>(capacity * sizeof(JoinTable::Segment))));
    segments.reserve(capacity);
  }
  std::uint32_t chunk = first_chunk;
  for (const JoinBuildPart::Piece& piece : part.pieces_) {
    const std::uint32_t begin = piece.start[partition];
    const std::uint32_t end = piece.start[partition + 1];
    if (end > begin) {
      segments.push_back(JoinTable::Segment{
          .hashes = piece.hashes.data() + begin, .chunk = chunk, .begin = begin, .end = end});
    }
    ++chunk;
  }
  return arrow::Status::OK();
}

arrow::Status JoinTableBuilder::Merged() {
  return NoBadAlloc("a join build", [this] {
    // The failure of the earliest part decides, as in the serial order. A failed release hands no
    // later part to the merges, so a merge failure is of that part or an earlier one: it wins.
    arrow::Status merged = lanes_->Finish();
    if (!merged.ok()) {
      failed_ = std::move(merged);
    }
    return failed_;
  });
}

arrow::Result<std::shared_ptr<const KeyFilter>> JoinTableBuilder::MakeKeyFilter() const {
  // One key, and of a type a Parquet scan decodes to the same values the build hashed: the probe
  // side hashes with KeyHashes over the scan's own array, so the two agree only when the one key
  // column is there to hash. (The same restriction LeadingKeyField states for a sort key.)
  if (spec_ == nullptr || spec_->keys().size() != 1 ||
      !(plan::IsInteger(spec_->keys().front().type) ||
        spec_->keys().front().type == plan::LogicalType::kDate)) {
    return nullptr;
  }
  int64_t rows = 0;
  for (const std::shared_ptr<const JoinBuildPart>& part : parts_) {
    if (part != nullptr) {
      rows += part->num_rows();
    }
  }
  ARROW_ASSIGN_OR_RAISE(std::shared_ptr<KeyFilter> filter, KeyFilter::Make(rows, budget_));
  if (filter == nullptr) {
    return nullptr;  // no keys, or no room: the probe runs as it does without one
  }
  // In part order, though the order cannot matter: setting a bit twice sets it once, so the filter
  // is the same whatever order the parts arrived in or the merges ran in.
  for (const std::shared_ptr<const JoinBuildPart>& part : parts_) {
    if (part == nullptr) {
      continue;
    }
    for (const JoinBuildPart::Piece& piece : part->pieces_) {
      filter->Add(piece.hashes);
    }
  }
  return filter;
}

arrow::Result<std::shared_ptr<const JoinTable>> JoinTableBuilder::Finish() {
  if (finished_) {
    return arrow::Status::Invalid("a join build finished twice");
  }
  finished_ = true;
  arrow::Result<std::shared_ptr<const JoinTable>> table =
      NoBadAlloc("a join build", [this] -> arrow::Result<std::shared_ptr<const JoinTable>> {
        ARROW_RETURN_NOT_OK(Merged());
        if (next_part_ < num_parts_) {
          return arrow::Status::Invalid("join build part ", next_part_, " was not added");
        }
        std::shared_ptr<JoinTable> built(new JoinTable(spec_, budget_));
        built->input_rows_ = input_rows_;
        built->null_key_rows_ = null_key_rows_;
        ARROW_RETURN_NOT_OK(built->Build(parts_, segments_, min_key_, max_key_, executor_));
        return built;
      });
  // Whatever the outcome, the parts' hashes and the runs go; a table holds the parts' rows it
  // needs.
  Free(parts_);
  memory_.Release();
  for (std::size_t p = 0; p < kJoinPartitions; ++p) {
    Free(segments_[p]);
    segment_memory_[p].Release();
  }
  return table;
}

}  // namespace antb1::exec
