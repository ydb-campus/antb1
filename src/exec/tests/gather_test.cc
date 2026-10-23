// Gathering rows of several chunks into one array (gather.h): the arrays ChunkColumns takes once,
// each the array its chunk keeps (also for a chunk that boxes its columns only when asked), and
// GatherRows over them and over the chunks, for each type with a typed loop and for the slice path
// (BOOLEAN), with NULLs in the data and, with kWithNulls, refs that name no row: always the rows
// read one at a time.

#include "../gather.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <gtest/gtest.h>

#include "antb1/common/narrow.h"
#include "antb1/exec/join_table.h"
#include "antb1/exec/sort.h"

#include "exec_test_util.h"

namespace antb1::exec {
namespace {

using testing::ArrayOf;
using testing::Bools;
using testing::Dates;
using testing::Decimals;
using testing::Int16s;
using testing::Int32s;
using testing::Int64s;
using testing::Strings;
using testing::Timestamps;
using testing::UInt16s;
using Chunks = std::vector<std::shared_ptr<arrow::RecordBatch>>;

// A column of every type GatherRows has a typed loop for, then a BOOLEAN one (the slice path), with
// values made from `values` (NULL where it has none, all >= 0).
arrow::ArrayVector ColumnsOf(const std::vector<std::optional<int64_t>>& values) {
  std::vector<std::optional<int16_t>> smallints;
  std::vector<std::optional<int32_t>> ints;
  std::vector<std::optional<uint16_t>> usmallints;
  std::vector<std::optional<int64_t>> micros;
  std::vector<std::optional<double>> doubles;
  std::vector<std::optional<std::string>> decimals;
  std::vector<std::optional<std::string>> hugeints;
  std::vector<std::optional<std::string>> texts;
  std::vector<std::optional<bool>> flags;
  for (const std::optional<int64_t>& v : values) {
    if (!v.has_value()) {
      smallints.emplace_back();
      ints.emplace_back();
      usmallints.emplace_back();
      micros.emplace_back();
      doubles.emplace_back();
      decimals.emplace_back();
      hugeints.emplace_back();
      texts.emplace_back();
      flags.emplace_back();
      continue;
    }
    smallints.emplace_back(Narrow<int16_t>(*v));
    ints.emplace_back(Narrow<int32_t>(*v));
    usmallints.emplace_back(Narrow<uint16_t>(*v));
    micros.emplace_back(*v * 1'000'000);
    doubles.emplace_back(static_cast<double>(*v) + 0.25);
    decimals.emplace_back(std::to_string((*v * 100) + 1));
    hugeints.emplace_back(std::to_string(*v) + "000000000000000000000");  // past 18 digits
    texts.emplace_back(std::string(Narrow<std::size_t>(*v % 3), 'x') + std::to_string(*v));
    flags.emplace_back(*v % 2 == 0);
  }
  return {Int16s(smallints),
          Int32s(ints),
          Int64s(values),
          UInt16s(usmallints),
          Dates(ints),
          Timestamps(micros),
          ArrayOf<arrow::DoubleBuilder>(arrow::float64(), doubles),
          Decimals(18, 2, decimals),
          Decimals(38, 0, hugeints),
          Strings(texts),
          Bools(flags)};
}

// Three chunks of ColumnsOf's columns: 3 rows, none, and 4 rows made from ArrayData, so that the
// last boxes each column when first asked for it (a batch made from arrays holds them boxed).
Chunks MakeChunks() {
  const arrow::ArrayVector first = ColumnsOf({5, std::nullopt, 3});
  arrow::FieldVector fields;
  for (std::size_t c = 0; c < first.size(); ++c) {
    fields.push_back(arrow::field("c" + std::to_string(c), first[c]->type()));
  }
  const auto schema = arrow::schema(fields);
  std::vector<std::shared_ptr<arrow::ArrayData>> last;
  for (const std::shared_ptr<arrow::Array>& column : ColumnsOf({7, 8, std::nullopt, 10})) {
    last.push_back(column->data());
  }
  return {arrow::RecordBatch::Make(schema, 3, first),
          arrow::RecordBatch::Make(schema, 0, ColumnsOf({})),
          arrow::RecordBatch::Make(schema, 4, std::move(last))};
}

// GatherRows<kWithNulls> of `refs` over `arrays`, into a builder of `type`.
template <bool kWithNulls, class Ref>
std::shared_ptr<arrow::Array> Gather(std::span<const arrow::Array* const> arrays,
                                     const std::shared_ptr<arrow::DataType>& type,
                                     const std::vector<Ref>& refs) {
  std::unique_ptr<arrow::ArrayBuilder> builder = arrow::MakeBuilder(type).ValueOrDie();
  const arrow::Status status =
      GatherRows<kWithNulls, Ref>(arrays, std::span<const Ref>(refs), *builder);
  EXPECT_TRUE(status.ok()) << status.ToString();
  return builder->Finish().ValueOrDie();
}

// The same over column `column` of `chunks`, which asks each chunk for it (the sort buffer's).
template <bool kWithNulls, class Ref>
std::shared_ptr<arrow::Array> Gather(const Chunks& chunks, int column,
                                     const std::shared_ptr<arrow::DataType>& type,
                                     const std::vector<Ref>& refs) {
  std::unique_ptr<arrow::ArrayBuilder> builder = arrow::MakeBuilder(type).ValueOrDie();
  const arrow::Status status =
      GatherRows<kWithNulls, Ref>(chunks, column, std::span<const Ref>(refs), *builder);
  EXPECT_TRUE(status.ok()) << status.ToString();
  return builder->Finish().ValueOrDie();
}

// `gathered` holds the rows `refs` of column `column` of `chunks`, each read on its own (NULL for a
// ref without a row).
template <class Ref>
void ExpectRows(const arrow::Array& gathered, const Chunks& chunks, int column,
                const std::vector<Ref>& refs) {
  ASSERT_EQ(gathered.length(), static_cast<int64_t>(refs.size()));
  for (std::size_t i = 0; i < refs.size(); ++i) {
    const arrow::Result<std::shared_ptr<arrow::Scalar>> got =
        gathered.GetScalar(static_cast<int64_t>(i));
    ASSERT_TRUE(got.ok()) << got.status().ToString();
    if (refs[i].chunk == kNoChunk) {
      EXPECT_FALSE((*got)->is_valid) << "row " << i;
      continue;
    }
    const arrow::Result<std::shared_ptr<arrow::Scalar>> want =
        chunks[refs[i].chunk]->column(column)->GetScalar(refs[i].row);
    ASSERT_TRUE(want.ok()) << want.status().ToString();
    EXPECT_TRUE((*got)->Equals(**want))
        << "row " << i << ": " << (*got)->ToString() << ", not " << (*want)->ToString();
  }
}

// The refs {chunk, row} of `rows`, in order.
template <class Ref>
std::vector<Ref> RefsOf(std::initializer_list<std::pair<std::uint32_t, std::uint32_t>> rows) {
  std::vector<Ref> refs;
  refs.reserve(rows.size());
  for (const auto& [chunk, row] : rows) {
    refs.push_back(Ref{.chunk = chunk, .row = row});
  }
  return refs;
}

// Across chunks and back, a row twice, rows whose data is NULL, and runs of consecutive rows (one
// slice each on the slice path).
template <class Ref>
std::vector<Ref> MixedRefs() {
  return RefsOf<Ref>({{2, 3}, {0, 0}, {0, 1}, {0, 2}, {2, 0}, {2, 1}, {2, 2}, {0, 1}, {2, 3}});
}

// Every chunk's array of every column, in chunk order, each the array the chunk keeps: asked again,
// a chunk returns it (and ASan would see a read of one it had freed). Then no chunks.
TEST(GatherTest, ChunkColumnsTakeTheArraysTheirChunksKeep) {
  const Chunks chunks = MakeChunks();
  const int fields = chunks.front()->num_columns();
  const ChunkColumns columns(chunks, fields);
  for (int c = 0; c < fields; ++c) {
    const std::span<const arrow::Array* const> arrays = columns.column(c);
    ASSERT_EQ(arrays.size(), chunks.size());
    for (std::size_t k = 0; k < chunks.size(); ++k) {
      EXPECT_EQ(arrays[k], chunks[k]->column(c).get()) << "column " << c << ", chunk " << k;
      EXPECT_EQ(arrays[k]->length(), chunks[k]->num_rows());
    }
  }
  const ChunkColumns none(Chunks{}, fields);
  EXPECT_TRUE(none.column(fields - 1).empty());
}

// Every type, gathered from a ChunkColumns with either kind of row reference, and from the chunks
// with the sort buffer's.
TEST(GatherTest, GathersEveryTypeAsReadRowByRow) {
  const Chunks chunks = MakeChunks();
  const ChunkColumns columns(chunks, chunks.front()->num_columns());
  const auto join_refs = MixedRefs<JoinRowRef>();
  const auto sort_refs = MixedRefs<SortBuffer::RowRef>();
  for (int c = 0; c < chunks.front()->num_columns(); ++c) {
    const std::shared_ptr<arrow::DataType>& type = chunks.front()->schema()->field(c)->type();
    SCOPED_TRACE(type->ToString());
    ExpectRows(*Gather<false>(columns.column(c), type, join_refs), chunks, c, join_refs);
    ExpectRows(*Gather<true>(columns.column(c), type, join_refs), chunks, c, join_refs);
    ExpectRows(*Gather<false>(columns.column(c), type, sort_refs), chunks, c, sort_refs);
    ExpectRows(*Gather<false>(chunks, c, type, sort_refs), chunks, c, sort_refs);
  }
}

// With NULLs, a ref without a row (kNoChunk) appends NULL: first, alone, in a run, next to rows
// whose own data is NULL, and last.
TEST(GatherTest, RefsWithoutARowGatherNull) {
  const Chunks chunks = MakeChunks();
  const ChunkColumns columns(chunks, chunks.front()->num_columns());
  const std::vector<JoinRowRef> refs = RefsOf<JoinRowRef>(
      {{kNoChunk, 0}, {0, 0}, {0, 1}, {kNoChunk, 0}, {kNoChunk, 0}, {2, 2}, {2, 3}, {kNoChunk, 0}});
  for (int c = 0; c < chunks.front()->num_columns(); ++c) {
    const std::shared_ptr<arrow::DataType>& type = chunks.front()->schema()->field(c)->type();
    SCOPED_TRACE(type->ToString());
    const std::shared_ptr<arrow::Array> gathered = Gather<true>(columns.column(c), type, refs);
    ExpectRows(*gathered, chunks, c, refs);
    EXPECT_EQ(gathered->null_count(), 6);
  }
}

// No refs: an empty array of the column's type, also over no chunks.
TEST(GatherTest, NoRefsGatherAnEmptyArray) {
  const Chunks chunks = MakeChunks();
  const ChunkColumns columns(chunks, chunks.front()->num_columns());
  const ChunkColumns none(Chunks{}, chunks.front()->num_columns());
  const std::vector<JoinRowRef> refs;
  const std::vector<SortBuffer::RowRef> sort_refs;
  for (int c = 0; c < chunks.front()->num_columns(); ++c) {
    const std::shared_ptr<arrow::DataType>& type = chunks.front()->schema()->field(c)->type();
    SCOPED_TRACE(type->ToString());
    for (const std::shared_ptr<arrow::Array>& gathered :
         {Gather<false>(columns.column(c), type, refs), Gather<true>(none.column(c), type, refs),
          Gather<false>(Chunks{}, c, type, sort_refs)}) {
      EXPECT_EQ(gathered->length(), 0);
      EXPECT_TRUE(gathered->type()->Equals(*type));
    }
  }
}

// A column it did not take is a programming error, not a read past its arrays.
TEST(GatherDeathTest, AColumnNotTakenIsNotRead) {
  const Chunks chunks = MakeChunks();
  const ChunkColumns columns(chunks, 2);
  EXPECT_DEATH(static_cast<void>(columns.column(2)), "check failed");
}

}  // namespace
}  // namespace antb1::exec
