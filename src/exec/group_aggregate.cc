#include "antb1/exec/group_aggregate.h"

#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <utility>
#include <vector>

#include <arrow/api.h>

#include "antb1/exec/operator.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

#include "group_table.h"

namespace antb1::exec {
namespace {

std::shared_ptr<arrow::Schema> OutputOf(const std::vector<plan::BoundColumn>& keys,
                                        const std::vector<plan::AggregateCall>& aggregates) {
  arrow::FieldVector fields;
  fields.reserve(keys.size() + aggregates.size());
  for (std::size_t i = 0; i < keys.size(); ++i) {
    fields.push_back(arrow::field(std::format("key{}", i), plan::ToArrow(keys[i].type)));
  }
  for (std::size_t i = 0; i < aggregates.size(); ++i) {
    fields.push_back(arrow::field(std::format("agg{}", i), plan::ToArrow(aggregates[i].type)));
  }
  return arrow::schema(std::move(fields));
}

}  // namespace

GroupAggregateOperator::GroupAggregateOperator(std::unique_ptr<Operator> input,
                                               std::vector<plan::BoundColumn> keys,
                                               std::vector<plan::AggregateCall> aggregates)
    : input_(std::move(input)),
      keys_(std::move(keys)),
      aggregates_(std::move(aggregates)),
      schema_(OutputOf(keys_, aggregates_)) {}

GroupAggregateOperator::~GroupAggregateOperator() = default;

arrow::Status GroupAggregateOperator::Open(ExecContext& ctx) {
  pool_ = ctx.pool;
  done_ = false;
  table_.reset();
  ARROW_ASSIGN_OR_RAISE(
      table_, GroupTable::Make(keys_, aggregates_, input_->output_schema()->num_fields(), pool_,
                               ctx.budget));
  opened_ = true;
  return input_->Open(ctx);
}

arrow::Status GroupAggregateOperator::Close() {
  table_.reset();
  return input_->Close();
}

arrow::Result<Batch> GroupAggregateOperator::Next() {
  if (!opened_ || table_ == nullptr) {
    return done_ ? arrow::Result<Batch>(Batch{})
                 : arrow::Status::Invalid("group aggregate: Next() before Open()");
  }
  if (!done_) {
    while (true) {
      ARROW_ASSIGN_OR_RAISE(const Batch in, input_->Next());
      if (in.end()) {
        break;
      }
      ARROW_ASSIGN_OR_RAISE(const auto rows, Materialize(in, pool_));
      if (rows->num_rows() > 0) {
        ARROW_RETURN_NOT_OK(table_->Consume(*rows));
      }
    }
    done_ = true;
  }
  // Chunks of new groups, joined (GroupTable::NextChunk): their keys as first seen, the aggregates
  // of those groups.
  ARROW_ASSIGN_OR_RAISE(auto chunk, table_->NextChunk(schema_));
  if (chunk == nullptr) {
    table_.reset();  // gives the memory back
    return Batch{};
  }
  return Batch{.data = std::move(chunk), .selection = {}};
}

}  // namespace antb1::exec
