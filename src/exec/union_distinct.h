#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <arrow/compute/exec.h>
#include <arrow/stl_allocator.h>

#include "antb1/exec/operator.h"

#include "key_table.h"

namespace antb1::exec {

// A blocking pure UNION. Hash partitions have a single writer. Only first occurrences survive
// into the output batches, which also hold the rows used for exact comparisons on hash hits.
class UnionDistinct {
 public:
  explicit UnionDistinct(const ExecContext& ctx);
  ~UnionDistinct();
  UnionDistinct(const UnionDistinct&) = delete;
  UnionDistinct& operator=(const UnionDistinct&) = delete;

  arrow::Status Append(const std::shared_ptr<arrow::RecordBatch>& rows);
  // Serial, bounded batch path shared with part-local INTERSECT deduplication. Keep the first
  // original row for each normalized key tuple. key_columns has one row per row of rows.
  // Allocate the Take indices and retained rows through output; key and hash scratch uses ctx_.
  arrow::Status AppendSerial(const std::shared_ptr<arrow::RecordBatch>& rows,
                             const arrow::RecordBatch& key_columns,
                             arrow::compute::ExecContext& output);
  [[nodiscard]] int64_t num_groups() const { return groups_; }
  // Complete earlier input work before the next input opens.
  arrow::Status Flush();
  void Finish(ProfileNode* profile);
  Batch Next();

 private:
  class Index;
  struct Piece;
  std::size_t Lane(std::uint64_t hash) const;
  arrow::Status SeedLanes();
  arrow::Result<arrow::compute::ExecBatch> Keys(const arrow::RecordBatch& rows) const;
  arrow::Status Route(Piece& piece);
  arrow::Status Insert(std::size_t lane);
  arrow::Status Compact(Piece& piece) const;

  ExecContext ctx_;
  arrow::compute::ExecContext kernels_;
  std::unique_ptr<KeyTable> serial_;
  std::vector<Index> lanes_;
  std::vector<Piece> pending_;
  std::vector<std::shared_ptr<arrow::RecordBatch>,
              arrow::stl::allocator<std::shared_ptr<arrow::RecordBatch>>>
      output_;
  std::size_t next_ = 0;
  int64_t pending_rows_ = 0;
  int64_t groups_ = 0;
};

}  // namespace antb1::exec
