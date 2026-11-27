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

// Serial set operators share a table of normalized keys. The original rows stay with the
// caller: the positions of newly encountered keys refer to the original input order.
arrow::Result<KeyIds> ConsumeDistinctKeys(const arrow::RecordBatch& rows,
                                          std::unique_ptr<KeyTable>& table,
                                          arrow::compute::ExecContext& kernels,
                                          MemoryBudget* budget);
arrow::Result<std::shared_ptr<arrow::UInt32Array>> FirstDistinctRows(const KeyIds& ids,
                                                                     arrow::MemoryPool* pool);

// A blocking pure UNION. Hash partitions have a single writer. Only first occurrences survive
// into the output batches, which also hold the rows used for exact comparisons on hash hits.
class UnionDistinct {
 public:
  explicit UnionDistinct(const ExecContext& ctx);
  ~UnionDistinct();
  UnionDistinct(const UnionDistinct&) = delete;
  UnionDistinct& operator=(const UnionDistinct&) = delete;

  arrow::Status Append(const std::shared_ptr<arrow::RecordBatch>& rows);
  // Complete earlier input work before the next input opens.
  arrow::Status Flush();
  void Finish(ProfileNode* profile);
  Batch Next();

 private:
  class Index;
  struct Piece;
  std::size_t Lane(std::uint64_t hash) const;
  arrow::Status SeedLanes();
  arrow::Status ConsumeSerial(const std::shared_ptr<arrow::RecordBatch>& rows);
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
