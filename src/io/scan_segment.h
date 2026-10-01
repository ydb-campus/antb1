#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <arrow/buffer.h>
#include <arrow/io/file.h>
#include <arrow/memory_pool.h>
#include <arrow/result.h>
#include <parquet/metadata.h>

// Private to io: what the scans of a ParquetTable share.

namespace antb1::io {

// Row groups of one file that a scan reads, with the file's footer and size as read at Open.
struct Segment {
  std::string path;
  std::shared_ptr<parquet::FileMetaData> metadata;
  int64_t bytes = 0;
  std::shared_ptr<arrow::Buffer> footer;  // as stored: the serialized FileMetaData, length, magic
  std::vector<int> row_groups;
};

// Opens the segment's file for reading into `pool`, after checking that it still has the size
// and the footer it had at Open: the footer locates the column chunks and fixes their types, so a
// file rewritten since must not be decoded with it (IOError).
arrow::Result<std::shared_ptr<arrow::io::ReadableFile>> OpenSegmentFile(const Segment& segment,
                                                                        arrow::MemoryPool* pool);

}  // namespace antb1::io
