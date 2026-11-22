#include "antb1/exec/group_aggregate.h"

#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <utility>
#include <vector>

#include <arrow/api.h>

#include "antb1/exec/operator.h"
#include "antb1/exec/profile.h"
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
  budget_ = ctx.budget;
  output_ = 0;
  partition_ = nullptr;
  for (const auto& key : keys_) {
    partition_ = input_->RequestPartitioning(key.index, PartitionRequest::kExisting);
    if (partition_ != nullptr) {
      break;
    }
  }
  tables_.clear();
  tables_.resize(partition_ != nullptr ? partition_->count : 1);
  ARROW_ASSIGN_OR_RAISE(tables_.front(),
                        GroupTable::Make(keys_, aggregates_, input_->output_schema()->num_fields(),
                                         pool_, ctx.budget));
  opened_ = true;
  return input_->Open(ctx);
}

arrow::Status GroupAggregateOperator::Close() {
  tables_.clear();
  opened_ = false;
  return input_->Close();
}

arrow::Result<Batch> GroupAggregateOperator::Next() {
  if (!opened_ || output_ == tables_.size()) {
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
        auto& table = tables_[partition_ != nullptr ? partition_->bucket : 0];
        if (table == nullptr) {
          ARROW_ASSIGN_OR_RAISE(
              table, GroupTable::Make(keys_, aggregates_, input_->output_schema()->num_fields(),
                                      pool_, budget_));
        }
        ARROW_RETURN_NOT_OK(table->Consume(*rows));
      }
    }
    done_ = true;
    // Every group's finalize errors before the first chunk, which a LIMIT may stop after.
    int64_t occupied = 0;
    for (const auto& table : tables_) {
      if (table != nullptr) {
        ARROW_RETURN_NOT_OK(table->CheckFinalize());
        occupied += table->num_groups() != 0 ? 1 : 0;
      }
    }
    if (partition_ != nullptr && profile() != nullptr) {
      profile()->Add("group_buckets", MetricUnit::kCount, occupied);
    }
  }
  // Chunks of new groups, joined (GroupTable::NextChunk): their keys as first seen, the aggregates
  // of those groups.
  for (; output_ < tables_.size(); ++output_) {
    auto& table = tables_[output_];
    if (table != nullptr) {
      ARROW_ASSIGN_OR_RAISE(auto chunk, table->NextChunk(schema_));
      if (chunk != nullptr) {
        return Batch{.data = std::move(chunk), .selection = {}};
      }
      table.reset();
    }
  }
  return Batch{};
}

}  // namespace antb1::exec
