#include "parallel_compute.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/util/future.h>

namespace antb1::exec {

arrow::Result<Batch> ParallelComputeOperator::Shared::Compute(const Batch& batch,
                                                              arrow::MemoryPool* pool) const {
  try {
    return ComputeBatch(batch, exprs, schema, pool);
  } catch (const std::bad_alloc&) {
    return arrow::Status::OutOfMemory("out of memory while computing expressions");
  }
}

void ParallelComputeOperator::Shared::ComputeIntoSlot(int64_t id, Batch batch,
                                                      arrow::MemoryPool* pool) {
  arrow::Result<Batch> result = Compute(batch, pool);
  batch = Batch{};  // the input's reference goes here, not with the task
  const std::scoped_lock lock(mutex);
  slots.insert_or_assign(id, std::move(result));
}

arrow::Result<Batch> ParallelComputeOperator::Shared::TakeSlot(int64_t id) {
  const std::scoped_lock lock(mutex);
  auto it = slots.find(id);
  if (it == slots.end()) {
    return arrow::Status::Invalid("parallel compute: no result for batch ", id);
  }
  arrow::Result<Batch> result = std::move(it->second);
  slots.erase(it);
  return result;
}

ParallelComputeOperator::ParallelComputeOperator(std::unique_ptr<Operator> input,
                                                 std::vector<plan::ExprPtr> exprs)
    : input_(std::move(input)), shared_(std::make_shared<Shared>()) {
  shared_->schema = ComputedSchema(*input_->output_schema(), exprs);
  shared_->exprs = std::move(exprs);
}

ParallelComputeOperator::~ParallelComputeOperator() {
  Drop();
  entries_.clear();
}

arrow::Status ParallelComputeOperator::Open(ExecContext& ctx) {
  Drop();
  entries_.clear();
  pool_ = ctx.pool;
  executor_ = ctx.executor;
  budget_ = ctx.budget;
  max_window_ = static_cast<std::size_t>(std::max(ctx.threads, 1));
  window_ = max_window_;
  next_id_ = 0;
  input_done_ = false;
  input_error_ = arrow::Status::OK();
  return input_->Open(ctx);
}

arrow::Status ParallelComputeOperator::Submit(Entry& entry) {
  // The task holds the shared state and a copy of the input's references only until it has run.
  ARROW_ASSIGN_OR_RAISE(arrow::Future<> done,
                        executor_->Submit([shared = shared_, id = entry.id, batch = entry.input,
                                           pool = pool_]() mutable {
                          shared->ComputeIntoSlot(id, std::move(batch), pool);
                          return arrow::Status::OK();
                        }));
  entry.done = std::move(done);
  ++in_flight_;
  return arrow::Status::OK();
}

arrow::Status ParallelComputeOperator::Fill() {
  const auto room = [&] {
    if (in_flight_ >= window_) {
      return false;
    }
    return in_flight_ == 0 || budget_ == nullptr || !budget_->under_pressure();
  };
  for (Entry& entry : entries_) {  // batches dropped after an out-of-memory retry, in order
    if (!room()) {
      return arrow::Status::OK();
    }
    if (!entry.done.has_value()) {
      ARROW_RETURN_NOT_OK(Submit(entry));
    }
  }
  while (!input_done_ && room()) {
    arrow::Result<Batch> in = input_->Next();
    if (!in.ok()) {
      input_done_ = true;
      input_error_ = in.status();
      break;
    }
    if (in->end()) {
      input_done_ = true;
      break;
    }
    entries_.push_back(Entry{.id = next_id_++, .input = *std::move(in), .done = {}});
    ARROW_RETURN_NOT_OK(Submit(entries_.back()));
  }
  return arrow::Status::OK();
}

arrow::Result<Batch> ParallelComputeOperator::Next() {
  if (executor_ == nullptr) {
    ARROW_ASSIGN_OR_RAISE(const Batch in, input_->Next());
    if (in.end()) {
      return in;
    }
    return ComputeBatch(in, shared_->exprs, shared_->schema, pool_);
  }
  ARROW_RETURN_NOT_OK(Fill());
  if (entries_.empty()) {
    ARROW_RETURN_NOT_OK(input_error_);
    return Batch{};
  }
  Entry& front = entries_.front();
  if (!front.done.has_value()) {
    ARROW_RETURN_NOT_OK(Submit(front));
  }
  front.done->Wait();
  --in_flight_;
  front.done.reset();
  arrow::Result<Batch> result = shared_->TakeSlot(front.id);
  if (!result.ok() && result.status().IsOutOfMemory()) {
    // Out of memory on a worker, maybe next to other batches (taken since, or still in flight):
    // drop those in flight (their memory with them; they are computed again when reached) and
    // compute this one alone here. Only a failure alone fails the query.
    Drop();
    window_ = 1;
    result = shared_->Compute(front.input, pool_);
  } else if (budget_ != nullptr && budget_->under_pressure()) {
    window_ = std::max<std::size_t>(1, window_ / 2);
  } else {
    window_ = std::min(max_window_, window_ + 1);
  }
  entries_.pop_front();
  return result;
}

arrow::Status ParallelComputeOperator::Close() {
  Drop();
  entries_.clear();
  return input_->Close();
}

void ParallelComputeOperator::Drop() {
  for (Entry& entry : entries_) {
    if (entry.done.has_value()) {
      entry.done->Wait();
      entry.done.reset();
    }
  }
  in_flight_ = 0;
  std::unordered_map<int64_t, arrow::Result<Batch>> dropped;
  {
    const std::scoped_lock lock(shared_->mutex);
    dropped.swap(shared_->slots);
  }
}

}  // namespace antb1::exec
