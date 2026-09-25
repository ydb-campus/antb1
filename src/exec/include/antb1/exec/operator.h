#pragma once

#include <cstdint>
#include <memory>

#include <arrow/memory_pool.h>
#include <arrow/record_batch.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/table.h>

// Pull-based, batch-at-a-time physical operators (docs/adr/0003-engine-architecture.md).

namespace antb1::exec {

struct ExecContext {
  arrow::MemoryPool* pool = arrow::default_memory_pool();
  int64_t batch_size = int64_t{64} * 1024;
};

class Operator {
 public:
  virtual ~Operator() = default;
  Operator() = default;
  Operator(const Operator&) = delete;
  Operator& operator=(const Operator&) = delete;
  Operator(Operator&&) = delete;
  Operator& operator=(Operator&&) = delete;

  [[nodiscard]] virtual const std::shared_ptr<arrow::Schema>& output_schema() const = 0;
  virtual arrow::Status Open(ExecContext& ctx) = 0;
  // Next batch, or nullptr at end of stream.
  virtual arrow::Result<std::shared_ptr<arrow::RecordBatch>> Next() = 0;
  virtual arrow::Status Close() = 0;
};

// Opens op, pulls every batch into a table, closes op.
arrow::Result<std::shared_ptr<arrow::Table>> Drain(Operator& op, ExecContext& ctx);

}  // namespace antb1::exec
