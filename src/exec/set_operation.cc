#include "set_operation.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/compute/api_vector.h>
#include <arrow/util/bit_util.h>

#include "antb1/common/narrow.h"
#include "antb1/exec/profile.h"
#include "antb1/plan/types.h"

namespace antb1::exec {
namespace {

using plan::SetTermKind;

// The value of an operator term over its operands' values (X1 to Xn, counts or 0/1), one operand
// at a time over all the chunk's groups, so each loop is a plain one the compiler vectorizes.
void Combine(SetTermKind kind, std::span<const std::vector<int64_t>> operands,
             std::vector<int64_t>& out) {
  const std::vector<int64_t>& first = operands.front();
  const std::span<const std::vector<int64_t>> rest = operands.subspan(1);
  const std::size_t n = first.size();
  out.resize(n);
  switch (kind) {
    case SetTermKind::kUnion:
    case SetTermKind::kIntersect:
    case SetTermKind::kExcept:
      for (std::size_t i = 0; i < n; ++i) {
        out[i] = first[i] > 0 ? 1 : 0;
      }
      break;
    case SetTermKind::kIntersectAll:
    case SetTermKind::kExceptAll:
    case SetTermKind::kInput:
      std::ranges::copy(first, out.begin());
      break;
  }
  for (const std::vector<int64_t>& v : rest) {
    switch (kind) {
      case SetTermKind::kUnion:
        for (std::size_t i = 0; i < n; ++i) {
          out[i] = out[i] != 0 || v[i] > 0 ? 1 : 0;
        }
        break;
      case SetTermKind::kIntersect:
        for (std::size_t i = 0; i < n; ++i) {
          out[i] = out[i] != 0 && v[i] > 0 ? 1 : 0;
        }
        break;
      case SetTermKind::kExcept:
        for (std::size_t i = 0; i < n; ++i) {
          out[i] = out[i] != 0 && v[i] == 0 ? 1 : 0;
        }
        break;
      case SetTermKind::kIntersectAll:
        for (std::size_t i = 0; i < n; ++i) {
          out[i] = std::min(out[i], v[i]);
        }
        break;
      case SetTermKind::kExceptAll:
        // max(X1 - X2 - ... - Xn, 0), clamped after each operand: every count is at least 0, so
        // that is the same value, and a difference of two values at least 0 stays within int64.
        for (std::size_t i = 0; i < n; ++i) {
          out[i] = std::max<int64_t>(out[i] - v[i], 0);
        }
        break;
      case SetTermKind::kInput:
        break;
    }
  }
}

}  // namespace

arrow::Result<std::unique_ptr<SetOperationOperator>> SetOperationOperator::Make(
    std::vector<std::unique_ptr<Operator>> inputs, std::vector<std::vector<int>> columns,
    const std::vector<plan::LogicalType>& types, std::vector<plan::SetTerm> formula) {
  if (inputs.empty()) {
    return arrow::Status::Invalid("set operation without inputs");
  }
  if (columns.size() != inputs.size()) {
    return arrow::Status::Invalid("set operation: ", columns.size(), " column lists for ",
                                  inputs.size(), " inputs");
  }
  if (types.empty()) {
    return arrow::Status::Invalid("set operation without columns");
  }
  if (const std::optional<std::string> problem = plan::CheckSetFormula(formula, inputs.size())) {
    return arrow::Status::Invalid("set operation: ", *problem);
  }
  arrow::FieldVector fields;
  fields.reserve(types.size());
  for (std::size_t k = 0; k < inputs.size(); ++k) {
    if (inputs[k] == nullptr) {
      return arrow::Status::Invalid("set operation without its input ", k);
    }
    const arrow::Schema& schema = *inputs[k]->output_schema();
    if (columns[k].size() != types.size()) {
      return arrow::Status::Invalid("set operation: input ", k, " has ", columns[k].size(),
                                    " columns for ", types.size());
    }
    for (std::size_t i = 0; i < types.size(); ++i) {
      const int column = columns[k][i];
      if (column < 0 || column >= schema.num_fields()) {
        return arrow::Status::Invalid("set operation: column ", column, " of input ", k,
                                      " is outside its ", schema.num_fields(), " columns");
      }
      const std::shared_ptr<arrow::DataType> type = plan::ToArrow(types[i]);
      if (type == nullptr || !schema.field(column)->type()->Equals(*type)) {
        return arrow::Status::Invalid("set operation: column ", i, " of input ", k, " is ",
                                      schema.field(column)->type()->ToString(), ", not ",
                                      type == nullptr ? "a type" : type->ToString());
      }
      if (k == 0) {
        fields.push_back(arrow::field(schema.field(column)->name(), type, /*nullable=*/true));
      }
    }
  }
  return std::unique_ptr<SetOperationOperator>(
      new SetOperationOperator(std::move(inputs), std::move(columns), types, std::move(formula),
                               arrow::schema(std::move(fields))));
}

SetOperationOperator::SetOperationOperator(std::vector<std::unique_ptr<Operator>> inputs,
                                           std::vector<std::vector<int>> columns,
                                           std::vector<plan::LogicalType> types,
                                           std::vector<plan::SetTerm> formula,
                                           std::shared_ptr<arrow::Schema> schema)
    : inputs_(std::move(inputs)),
      columns_(std::move(columns)),
      types_(std::move(types)),
      formula_(std::move(formula)),
      starters_(plan::SetStarters(formula_, inputs_.size())),
      counted_(plan::SetCounted(formula_, inputs_.size())),
      all_root_(formula_.back().kind == SetTermKind::kIntersectAll ||
                formula_.back().kind == SetTermKind::kExceptAll),
      schema_(std::move(schema)) {}

arrow::Status SetOperationOperator::Open(ExecContext& ctx) {
  ARROW_RETURN_NOT_OK(Close());
  ctx_ = ctx;
  memory_.Reset(ctx.budget);
  std::vector<plan::BoundColumn> keys;
  keys.reserve(types_.size());
  for (std::size_t i = 0; i < types_.size(); ++i) {
    keys.push_back(plan::BoundColumn{.index = Narrow<int>(i),
                                     .id = plan::kNoColumnId,
                                     .name = schema_->field(Narrow<int>(i))->name(),
                                     .type = types_[i]});
  }
  // An ALL root shows the normalized keys (uniques()), so it keeps no first-seen copy.
  ARROW_ASSIGN_OR_RAISE(
      table_, GroupTable::Make(std::move(keys), {}, schema_->num_fields(), ctx.pool, ctx.budget,
                               /*first_keys=*/!all_root_));
  bits_.assign(inputs_.size(), {});
  counts_.assign(inputs_.size(), {});
  opened_ = true;
  return arrow::Status::OK();
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> SetOperationOperator::Rows(
    std::size_t k, const Batch& batch) const {
  const std::vector<int>& picked = columns_[k];
  std::vector<std::shared_ptr<arrow::Array>> data;
  data.reserve(picked.size());
  for (std::size_t i = 0; i < picked.size(); ++i) {
    const int column = picked[i];
    if (column >= batch.data->num_columns() ||
        !batch.data->column(column)->type()->Equals(*schema_->field(Narrow<int>(i))->type())) {
      return arrow::Status::Invalid("set operation: input ", k, " gave a batch whose column ",
                                    column, " is not of its schema");
    }
    data.push_back(batch.data->column(column));
  }
  return Materialize(
      Batch{.data = arrow::RecordBatch::Make(schema_, batch.data->num_rows(), std::move(data)),
            .selection = batch.selection},
      ctx_.pool);
}

arrow::Status SetOperationOperator::Grow() {
  const std::size_t groups = table_->num_groups();
  // Each membership's capacity after it grows to `groups`, doubling as a vector does, so the
  // budget is charged what the vectors hold, not just their size.
  const auto capacity = [groups](std::size_t now) {
    return now >= groups ? now : std::max(groups, 2 * now);
  };
  int64_t bytes = 0;
  for (std::size_t k = 0; k < inputs_.size(); ++k) {
    if (counted_[k]) {
      bytes += Narrow<int64_t>(capacity(counts_[k].capacity()) * sizeof(int64_t));
    } else {
      bytes += Narrow<int64_t>(capacity(bits_[k].capacity()));
    }
  }
  bytes += Narrow<int64_t>(copies_.capacity() * sizeof(int64_t));
  ARROW_RETURN_NOT_OK(memory_.Resize(bytes));
  for (std::size_t k = 0; k < inputs_.size(); ++k) {
    if (counted_[k]) {
      counts_[k].reserve(capacity(counts_[k].capacity()));
      counts_[k].resize(groups, 0);
    } else {
      bits_[k].reserve(capacity(bits_[k].capacity()));
      bits_[k].resize(groups, 0);
    }
  }
  return arrow::Status::OK();
}

arrow::Status SetOperationOperator::Build() {
  int64_t starter_rows = 0;
  int64_t lookup_rows = 0;
  int64_t lookup_hits = 0;
  std::vector<std::uint32_t> ids;
  for (std::size_t k = 0; k < inputs_.size(); ++k) {
    Operator& input = *inputs_[k];
    open_input_ = k;
    input_open_ = true;  // Close closes it even if its Open fails
    ARROW_RETURN_NOT_OK(input.Open(ctx_));
    while (true) {
      ARROW_ASSIGN_OR_RAISE(const Batch batch, input.Next());
      if (batch.end()) {
        break;
      }
      ARROW_ASSIGN_OR_RAISE(const std::shared_ptr<arrow::RecordBatch> rows, Rows(k, batch));
      if (rows->num_rows() == 0) {
        continue;
      }
      // Hoisted: a write through bits (uint8_t) may alias anything, so the compiler would reload
      // counted_[k] and each vector's data on every row.
      const bool counted = counted_[k];
      if (starters_[k]) {
        starter_rows += rows->num_rows();
        ARROW_RETURN_NOT_OK(table_->Consume(*rows, &ids));
        ARROW_RETURN_NOT_OK(Grow());
        if (counted) {
          int64_t* const counts = counts_[k].data();
          for (const std::uint32_t group : ids) {
            ++counts[group];
          }
        } else {
          std::uint8_t* const bits = bits_[k].data();
          for (const std::uint32_t group : ids) {
            bits[group] = 1;
          }
        }
        continue;
      }
      // A lookup adds no group: a row of this input whose keys no starter has can change no
      // group's copies (docs/adr/0039-set-operations.md).
      lookup_rows += rows->num_rows();
      ARROW_ASSIGN_OR_RAISE(const std::shared_ptr<arrow::UInt32Array> found, table_->Lookup(*rows));
      lookup_hits += found->length() - found->null_count();
      const std::uint32_t* const groups = found->raw_values();
      const std::uint8_t* const valid =
          found->null_count() == 0 ? nullptr : found->null_bitmap_data();
      const auto offset = Narrow<std::uint64_t>(found->offset());
      const int64_t length = found->length();
      const auto add = [&](auto mark) {
        if (valid == nullptr) {
          for (int64_t i = 0; i < length; ++i) {
            mark(groups[i]);
          }
          return;
        }
        for (int64_t i = 0; i < length; ++i) {
          if (arrow::bit_util::GetBit(valid, offset + static_cast<std::uint64_t>(i))) {
            mark(groups[i]);
          }
        }
      };
      if (counted) {
        int64_t* const counts = counts_[k].data();
        add([counts](std::uint32_t group) { ++counts[group]; });
      } else {
        std::uint8_t* const bits = bits_[k].data();
        add([bits](std::uint32_t group) { bits[group] = 1; });
      }
    }
    // Closed before the next input opens: its builds and its parts' windows are gone by then.
    input_open_ = false;
    ARROW_RETURN_NOT_OK(input.Close());
  }
  ARROW_RETURN_NOT_OK(Grow());
  ARROW_RETURN_NOT_OK(Evaluate());
  if (ProfileNode* node = profile(); node != nullptr) {
    node->Add("starter_rows", MetricUnit::kCount, starter_rows);
    node->Add("lookup_rows", MetricUnit::kCount, lookup_rows);
    node->Add("lookup_hits", MetricUnit::kCount, lookup_hits);
    node->Add("groups", MetricUnit::kCount, table_->num_groups());
  }
  return arrow::Status::OK();
}

arrow::Status SetOperationOperator::Evaluate() {
  const std::size_t groups = table_->num_groups();
  copies_.assign(groups, 0);
  ARROW_RETURN_NOT_OK(Grow());  // charges copies_ too
  // The postfix formula over chunks of groups, so the operands' vectors stay small.
  std::vector<std::vector<int64_t>> stack;
  std::vector<int64_t> result;
  for (std::size_t begin = 0; begin < groups; begin += GroupTable::kMaxMergeChunk) {
    const std::size_t end = std::min(groups, begin + GroupTable::kMaxMergeChunk);
    stack.clear();
    for (const plan::SetTerm& term : formula_) {
      if (term.kind == SetTermKind::kInput) {
        std::vector<int64_t>& values = stack.emplace_back(end - begin, 0);
        if (counted_[term.input]) {
          const int64_t* const counts = counts_[term.input].data();
          std::copy(counts + begin, counts + end, values.begin());
        } else {
          const std::uint8_t* const bits = bits_[term.input].data();
          std::copy(bits + begin, bits + end, values.begin());
        }
        continue;
      }
      const std::size_t first = stack.size() - term.operands;
      Combine(term.kind, std::span(stack).subspan(first), result);
      stack.resize(first);
      stack.push_back(std::move(result));
      result = {};
    }
    std::ranges::copy(stack.back(), copies_.begin() + static_cast<std::ptrdiff_t>(begin));
  }
  // The memberships have done their work.
  bits_.assign(inputs_.size(), {});
  counts_.assign(inputs_.size(), {});
  return memory_.Resize(Narrow<int64_t>(copies_.size() * sizeof(int64_t)));
}

arrow::Result<Batch> SetOperationOperator::Next() {
  if (!opened_) {
    return arrow::Status::Invalid("set operation: Next() before Open() or after Close()");
  }
  if (!built_) {
    ARROW_RETURN_NOT_OK(Build());
    built_ = true;
  }
  if (table_ == nullptr) {
    return Batch{};
  }
  return all_root_ ? NextRepeated() : NextDistinct();
}

// The table's chunks of groups in group order, each with a selection of the groups kept.
arrow::Result<Batch> SetOperationOperator::NextDistinct() {
  while (true) {
    ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::RecordBatch> chunk, table_->NextChunk(schema_));
    if (chunk == nullptr) {
      table_.reset();  // gives the memory back
      return Batch{};
    }
    const auto rows = static_cast<std::size_t>(chunk->num_rows());
    arrow::BooleanBuilder selection(ctx_.pool);
    ARROW_RETURN_NOT_OK(selection.Reserve(chunk->num_rows()));
    std::size_t kept = 0;
    for (std::size_t i = 0; i < rows; ++i) {
      const bool keep = copies_[next_group_ + i] > 0;
      kept += keep ? 1 : 0;
      selection.UnsafeAppend(keep);
    }
    next_group_ += Narrow<std::uint32_t>(rows);
    if (kept == 0) {
      continue;
    }
    if (kept == rows) {
      return Batch{.data = std::move(chunk), .selection = nullptr};
    }
    std::shared_ptr<arrow::BooleanArray> mask;
    ARROW_RETURN_NOT_OK(selection.Finish(&mask));
    return Batch{.data = std::move(chunk), .selection = std::move(mask)};
  }
}

// The normalized keys of each group, repeated as many times as its copies, at most
// kMaxRepeatedRows a batch: a group with more copies spans batches.
arrow::Result<Batch> SetOperationOperator::NextRepeated() {
  const std::uint32_t groups = table_->num_groups();
  if (uniques_ == nullptr && groups > 0) {
    ARROW_ASSIGN_OR_RAISE(const arrow::compute::ExecBatch keys, table_->uniques());
    std::vector<std::shared_ptr<arrow::Array>> arrays;
    arrays.reserve(keys.values.size());
    for (const arrow::Datum& key : keys.values) {
      arrays.push_back(key.make_array());
    }
    uniques_ = arrow::RecordBatch::Make(schema_, keys.length, std::move(arrays));
  }
  arrow::UInt32Builder indices(ctx_.pool);
  int64_t rows = 0;
  while (next_group_ < groups && rows < kMaxRepeatedRows) {
    if (pending_copies_ == 0) {
      pending_copies_ = copies_[next_group_];
      if (pending_copies_ == 0) {
        ++next_group_;
        continue;
      }
    }
    const int64_t take = std::min(pending_copies_, kMaxRepeatedRows - rows);
    ARROW_RETURN_NOT_OK(indices.Reserve(take));
    for (int64_t i = 0; i < take; ++i) {
      indices.UnsafeAppend(next_group_);
    }
    rows += take;
    pending_copies_ -= take;
    if (pending_copies_ == 0) {
      ++next_group_;
    }
  }
  if (rows == 0) {
    uniques_.reset();
    table_.reset();  // gives the memory back
    return Batch{};
  }
  std::shared_ptr<arrow::Array> taken_indices;
  ARROW_RETURN_NOT_OK(indices.Finish(&taken_indices));
  arrow::compute::ExecContext kernels(ctx_.pool);
  ARROW_ASSIGN_OR_RAISE(
      const arrow::Datum taken,
      arrow::compute::Take(uniques_, taken_indices, arrow::compute::TakeOptions::NoBoundsCheck(),
                           &kernels));
  return Batch{.data = taken.record_batch(), .selection = nullptr};
}

arrow::Status SetOperationOperator::Close() {
  arrow::Status status;
  if (input_open_) {
    input_open_ = false;
    status = inputs_[open_input_]->Close();  // stops its parts and waits for them
  }
  table_.reset();
  uniques_.reset();
  bits_.clear();
  counts_.clear();
  copies_.clear();
  copies_.shrink_to_fit();
  memory_.Release();
  opened_ = false;
  built_ = false;
  next_group_ = 0;
  pending_copies_ = 0;
  return status;
}

}  // namespace antb1::exec
