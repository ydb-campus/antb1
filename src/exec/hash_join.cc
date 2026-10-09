#include "hash_join.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/compute/api_vector.h>
#include <arrow/compute/exec.h>
#include <arrow/util/bit_util.h>

#include "antb1/common/check.h"
#include "antb1/common/narrow.h"
#include "antb1/exec/compute.h"
#include "antb1/exec/join_table.h"
#include "antb1/exec/memory_budget.h"
#include "antb1/exec/operator.h"
#include "antb1/exec/profile.h"
#include "antb1/plan/logical_plan.h"

#include "gather.h"
#include "part_operators.h"
#include "part_scheduler.h"

namespace antb1::exec {
namespace {

using BuildPart = std::shared_ptr<const JoinBuildPart>;

// Frees the memory of `values` (clear() keeps it).
template <class T>
void Free(std::vector<T>& values) {
  std::vector<T>().swap(values);
}

// `values` resized to `size`, its growth reserved on `memory` before it is allocated.
template <class T>
arrow::Status Fit(std::vector<T>& values, std::size_t size, MemoryReservation& memory) {
  if (size > values.capacity()) {
    ARROW_RETURN_NOT_OK(
        memory.Resize(memory.bytes() + Narrow<int64_t>((size - values.capacity()) * sizeof(T))));
    values.reserve(size);
  }
  values.resize(size);
  return arrow::Status::OK();
}

// The join's output: the left input's fields, then the right input's; the build's are nullable
// (the 1:1 path writes NULL where a probe row has no match).
std::shared_ptr<arrow::Schema> JoinSchema(const arrow::Schema& probe, const arrow::Schema& build,
                                          plan::BuildSide build_side) {
  arrow::FieldVector build_fields;
  build_fields.reserve(static_cast<std::size_t>(build.num_fields()));
  for (const std::shared_ptr<arrow::Field>& field : build.fields()) {
    build_fields.push_back(field->WithNullable(true));
  }
  const bool build_left = build_side == plan::BuildSide::kLeft;
  arrow::FieldVector fields = build_left ? build_fields : probe.fields();
  const arrow::FieldVector& right = build_left ? probe.fields() : build_fields;
  fields.insert(fields.end(), right.begin(), right.end());
  return arrow::schema(std::move(fields));
}

// The columns of a batch of candidate pairs of a join that builds on the right: the probe's, the
// build's (the left input's, then the right one's, as residuals read them), then each pair's slot.
std::shared_ptr<arrow::Schema> PairSchema(const arrow::Schema& probe, const arrow::Schema& build) {
  arrow::FieldVector fields = JoinSchema(probe, build, plan::BuildSide::kRight)->fields();
  fields.push_back(arrow::field("slot", arrow::uint32(), /*nullable=*/false));
  return arrow::schema(std::move(fields));
}

// A candidate (an index of JoinTable::rows()) that names no build row: a left join's padded row.
constexpr std::uint32_t kPadded = std::numeric_limits<std::uint32_t>::max();

// The most bytes of VARCHAR values that one window of a one-row join appends: the values' columns
// repeat each value once per row, so a window holds fewer rows than batch_size when they are long
// (one row at least).
constexpr int64_t kOneRowWindowBytes = int64_t{1024} * 1024;

// The values of the single row of `table` as columns of up to max(ctx.batch_size, 1) rows, fewer
// when its VARCHAR values are long (kOneRowWindowBytes), from ctx.pool.
arrow::Result<std::shared_ptr<const OneRowValues>> OneRowValuesOf(const JoinTable& table,
                                                                  const ExecContext& ctx) {
  const JoinRowRef ref = table.rows().front();
  const arrow::RecordBatch& chunk = *table.chunks()[ref.chunk];
  std::vector<std::shared_ptr<arrow::Scalar>> scalars;
  scalars.reserve(static_cast<std::size_t>(chunk.num_columns()));
  int64_t varchar_bytes = 0;
  for (int c = 0; c < chunk.num_columns(); ++c) {
    ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::Scalar> scalar,
                          chunk.column(c)->GetScalar(ref.row));
    if (scalar->is_valid && scalar->type->id() == arrow::Type::BINARY) {
      varchar_bytes += static_cast<const arrow::BinaryScalar&>(*scalar).value->size();
    }
    scalars.push_back(std::move(scalar));
  }
  const int64_t batch_size = std::max<int64_t>(ctx.batch_size, 1);
  auto values = std::make_shared<OneRowValues>();
  values->rows = varchar_bytes == 0
                     ? batch_size
                     : std::clamp<int64_t>(kOneRowWindowBytes / varchar_bytes, 1, batch_size);
  values->columns.reserve(scalars.size());
  for (const std::shared_ptr<arrow::Scalar>& scalar : scalars) {
    ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::Array> column,
                          arrow::MakeArrayFromScalar(*scalar, values->rows, ctx.pool));
    values->columns.push_back(std::move(column));
  }
  return values;
}

}  // namespace

// ---- JoinBuild ----

JoinBuild::JoinBuild(std::shared_ptr<const JoinBuildSpec> spec, PartPipeline pipeline,
                     int64_t num_parts, std::vector<std::shared_ptr<JoinBuild>> builds,
                     ProfileNode* profile, plan::JoinKind kind)
    : spec_(std::move(spec)),
      pipeline_(std::make_shared<const PartPipeline>(std::move(pipeline))),
      num_parts_(num_parts),
      builds_(std::move(builds)),
      profile_(profile),
      kind_(kind) {
  ANTB1_CHECK(spec_ != nullptr && *pipeline_);
}

JoinBuild::JoinBuild(std::shared_ptr<const JoinBuildSpec> spec, std::unique_ptr<Operator> input,
                     ProfileNode* profile, plan::JoinKind kind)
    : spec_(std::move(spec)), input_(std::move(input)), profile_(profile), kind_(kind) {
  ANTB1_CHECK(spec_ != nullptr && input_ != nullptr);
}

JoinBuild::~JoinBuild() = default;

arrow::Status JoinBuild::Prepare(ExecContext& ctx) {
  Release();
  const auto start = std::chrono::steady_clock::now();
  arrow::Status status;
  try {
    status = BuildTable(ctx);
  } catch (const std::bad_alloc&) {
    Release();  // before the status, whose message allocates
    status = arrow::Status::OutOfMemory("out of memory in a join build");
  }
  if (!status.ok()) {
    Release();
  }
  if (profile_ != nullptr) {
    profile_->AddTime(std::chrono::steady_clock::now() - start);
    profile_->AddInstance();
  }
  return status;
}

void JoinBuild::Release() {
  table_.reset();
  values_.reset();
  ReleaseBuilds(builds_);
}

arrow::Result<std::vector<Batch>> JoinBuild::DrainInput(ExecContext& ctx,
                                                        MemoryReservation& memory) {
  std::vector<Batch> batches;
  arrow::Status status;
  bool out_of_memory = false;
  try {
    status = input_->Open(ctx);
    while (status.ok()) {
      arrow::Result<Batch> batch = input_->Next();
      if (!batch.ok()) {
        status = batch.status();
        break;
      }
      if (batch->end()) {
        break;
      }
      if (batch->selected_rows() == 0) {
        continue;
      }
      if (batches.size() == batches.capacity()) {
        const std::size_t capacity = std::max<std::size_t>(16, 2 * batches.capacity());
        status = memory.Resize(Narrow<int64_t>(capacity * sizeof(Batch)));
        if (!status.ok()) {
          break;
        }
        batches.reserve(capacity);
      }
      batches.push_back(*std::move(batch));
    }
  } catch (const std::bad_alloc&) {
    out_of_memory = true;  // the input is closed first: nothing allocates before
  }
  const arrow::Status closed = input_->Close();  // also after a failure: it holds nothing
  if (out_of_memory) {
    return arrow::Status::OutOfMemory("out of memory in a join build");
  }
  ARROW_RETURN_NOT_OK(status);
  ARROW_RETURN_NOT_OK(closed);
  return batches;
}

arrow::Status JoinBuild::BuildTable(ExecContext& ctx) {
  // The builds the input probes come first (post-order).
  ARROW_RETURN_NOT_OK(PrepareBuilds(builds_, ctx));
  MemoryReservation batch_memory;  // the vector of `batches`
  batch_memory.Reset(ctx.budget);
  // A drained input's batches, one per part. Its tasks read them through a pointer and own none:
  // a worker may destroy a task's closure after its run, and so after Prepare has returned. The
  // scheduler, gone first, waits for every task it started, and the batches go on this thread.
  std::vector<Batch> batches;
  PartScheduler<BuildPart>::Task task;
  int64_t num_parts = num_parts_;
  if (pipeline_ != nullptr) {
    task = [pipeline = pipeline_, part_ctx = PartContext(ctx), spec = spec_, profile = profile_](
               int64_t part, const std::atomic<bool>& stop) -> arrow::Result<BuildPart> {
      const ProfileTimer part_time(profile, "part_time");
      auto rows = std::make_shared<JoinBuildPart>(spec, part_ctx.budget);
      ARROW_RETURN_NOT_OK(
          RunPart(*pipeline, part, part_ctx, stop, [&](const Batch& batch) -> arrow::Result<bool> {
            ARROW_RETURN_NOT_OK(rows->Append(batch, part_ctx.pool));
            return true;
          }));
      return rows;
    };
  } else {
    ARROW_ASSIGN_OR_RAISE(batches, DrainInput(ctx, batch_memory));
    num_parts = static_cast<int64_t>(batches.size());
    task = [drained = &batches, spec = spec_, pool = ctx.pool, budget = ctx.budget,
            profile = profile_](int64_t part,
                                const std::atomic<bool>& /*stop*/) -> arrow::Result<BuildPart> {
      const ProfileTimer part_time(profile, "part_time");
      auto rows = std::make_shared<JoinBuildPart>(spec, budget);
      ARROW_RETURN_NOT_OK(rows->Append((*drained)[static_cast<std::size_t>(part)], pool));
      return rows;
    };
  }
  if (profile_ != nullptr) {
    profile_->Max("parts", MetricUnit::kCount, num_parts);
  }
  const int64_t window = PartWindow(ctx);
  ARROW_ASSIGN_OR_RAISE(std::unique_ptr<JoinTableBuilder> builder,
                        JoinTableBuilder::Make(spec_, num_parts, ctx.executor, window, ctx.budget));
  arrow::Status status;
  {
    PartScheduler<BuildPart> scheduler(num_parts, std::move(task), ctx.executor, window,
                                       ctx.budget);
    // A part that runs out of memory runs again alone once the parts before it are merged. A
    // merge failure found here stays the build's: the next Add returns it.
    scheduler.set_before_retry([&builder] { static_cast<void>(builder->Merged()); });
    for (int64_t part = 0; !scheduler.done(); ++part) {
      arrow::Result<BuildPart> rows = [&] {
        const ProfileTimer wait(profile_, "wait");
        return scheduler.Next();
      }();
      if (!rows.ok()) {
        status = rows.status();
        break;
      }
      status = builder->Add(part, *std::move(rows));
      if (!status.ok()) {
        break;
      }
      if (pipeline_ == nullptr) {
        batches[static_cast<std::size_t>(part)] = Batch{};  // appended: no run of it is left
      }
    }
  }
  {
    // A merge failure is of an earlier part than any failure of the scheduler seen after it.
    const ProfileTimer tail(profile_, "lanes_tail");
    ARROW_RETURN_NOT_OK(builder->Merged());
  }
  ARROW_RETURN_NOT_OK(status);
  {
    const ProfileTimer finish(profile_, "finish");
    ARROW_ASSIGN_OR_RAISE(table_, builder->Finish());
    if (kind_ == plan::JoinKind::kOneRow) {
      // A planner bug, never an answer: SQL plans only an aggregate without groups here.
      if (table_->num_rows() != 1) {
        return arrow::Status::Invalid("a one-row join's right input returned ", table_->num_rows(),
                                      " rows");
      }
      ARROW_ASSIGN_OR_RAISE(values_, OneRowValuesOf(*table_, ctx));
    }
  }
  ReleaseBuilds(builds_);  // the input's parts are done: the tables they probed are not needed
  if (profile_ != nullptr) {
    for (const std::shared_ptr<arrow::RecordBatch>& chunk : table_->chunks()) {
      profile_->AddRows(chunk->num_rows());
    }
    profile_->Add("null_keys", MetricUnit::kCount, table_->null_key_rows());
    profile_->Max("unique", MetricUnit::kCount, table_->unique() ? 1 : 0);
    profile_->Max("direct", MetricUnit::kCount,
                  table_->layout() == JoinTable::Layout::kDirect ? 1 : 0);
  }
  return arrow::Status::OK();
}

bool EmptiesJoin(plan::JoinKind kind, const JoinTable& table) {
  return (kind == plan::JoinKind::kInner || kind == plan::JoinKind::kSemi) && table.num_rows() == 0;
}

arrow::Status PrepareBuilds(std::span<const std::shared_ptr<JoinBuild>> builds, ExecContext& ctx) {
  for (const std::shared_ptr<JoinBuild>& build : builds) {
    if (arrow::Status status = build->Prepare(ctx); !status.ok()) {
      ReleaseBuilds(builds);
      return status;
    }
    if (EmptiesJoin(build->kind(), *build->table())) {
      break;  // no row comes out of its join: the joins below it are never probed
    }
  }
  return arrow::Status::OK();
}

void ReleaseBuilds(std::span<const std::shared_ptr<JoinBuild>> builds) {
  for (const std::shared_ptr<JoinBuild>& build : builds) {
    build->Release();
  }
}

// ---- BuildsFirstOperator ----

BuildsFirstOperator::BuildsFirstOperator(std::unique_ptr<PartSink> sink,
                                         std::vector<std::shared_ptr<JoinBuild>> builds)
    : builds_(std::move(builds)), sink_(std::move(sink)) {
  ANTB1_CHECK(sink_ != nullptr);
  // The sink calls it from its Next, on this operator's thread, once no part of it runs.
  sink_->set_parts_done([this] { ReleaseBuilds(builds_); });
}

BuildsFirstOperator::~BuildsFirstOperator() = default;

arrow::Status BuildsFirstOperator::Open(ExecContext& ctx) {
  ARROW_RETURN_NOT_OK(Close());
  ctx_ = ctx;
  opened_ = true;
  return arrow::Status::OK();
}

arrow::Result<Batch> BuildsFirstOperator::Next() {
  if (!opened_) {
    return arrow::Status::Invalid("builds first: Next() before Open() or after Close()");
  }
  if (!sink_opened_) {
    ARROW_RETURN_NOT_OK(PrepareBuilds(builds_, ctx_));
    sink_opened_ = true;  // Close closes it even if its Open fails
    ARROW_RETURN_NOT_OK(sink_->Open(ctx_));
  }
  return sink_->Next();
}

arrow::Status BuildsFirstOperator::Close() {
  arrow::Status status;
  if (sink_opened_) {
    status = sink_->Close();  // stops its parts and waits for them
    sink_opened_ = false;
  }
  ReleaseBuilds(builds_);
  opened_ = false;
  return status;
}

// ---- HashJoinOperator ----

arrow::Result<std::unique_ptr<HashJoinOperator>> HashJoinOperator::Make(
    std::unique_ptr<Operator> probe, std::shared_ptr<JoinBuild> build, std::vector<int> probe_keys,
    plan::BuildSide build_side, std::vector<plan::ExprPtr> residual, bool prepares) {
  if (probe == nullptr || build == nullptr) {
    return arrow::Status::Invalid("a hash join without its inputs");
  }
  const plan::JoinKind kind = build->kind();
  const std::string_view kind_name = plan::ToString(kind);
  const JoinBuildSpec& spec = *build->spec();
  if (kind != plan::JoinKind::kInner && build_side == plan::BuildSide::kLeft) {
    return arrow::Status::Invalid("a ", kind_name, " hash join that builds on its left input");
  }
  if ((kind == plan::JoinKind::kOneRow) != spec.keys().empty()) {
    return arrow::Status::Invalid("a ", kind_name, " hash join over a build ",
                                  spec.keys().empty() ? "without" : "with", " keys");
  }
  if (kind == plan::JoinKind::kNullAwareAnti && spec.keys().size() != 1) {
    return arrow::Status::Invalid("a ", kind_name, " hash join of ", spec.keys().size(), " keys");
  }
  if ((kind == plan::JoinKind::kNullAwareAnti || kind == plan::JoinKind::kOneRow) &&
      !residual.empty()) {
    return arrow::Status::Invalid("a ", kind_name, " hash join with residuals");
  }
  const arrow::Schema& probe_schema = *probe->output_schema();
  if (probe_keys.size() != spec.keys().size()) {
    return arrow::Status::Invalid("a hash join probe of ", probe_keys.size(), " keys for ",
                                  spec.keys().size(), " build keys");
  }
  for (std::size_t k = 0; k < probe_keys.size(); ++k) {
    const int key = probe_keys[k];
    if (key < 0 || key >= probe_schema.num_fields()) {
      return arrow::Status::Invalid("hash join probe key outside its input");
    }
    const arrow::DataType& probe_type = *probe_schema.field(key)->type();
    const arrow::DataType& build_type = *spec.schema()->field(spec.keys()[k].index)->type();
    if (!probe_type.Equals(build_type)) {
      return arrow::Status::Invalid("a hash join probe key of type ", probe_type.ToString(),
                                    " for a build key of type ", build_type.ToString());
    }
  }
  // Semi, anti and null-aware anti joins pass the probe's batches on: its schema, the same object,
  // so that a build of their output takes it (JoinBuildPart checks the schema).
  const bool pass_through = kind == plan::JoinKind::kSemi || kind == plan::JoinKind::kAnti ||
                            kind == plan::JoinKind::kNullAwareAnti;
  std::shared_ptr<arrow::Schema> schema =
      pass_through ? probe->output_schema() : JoinSchema(probe_schema, *spec.schema(), build_side);
  // A residual reads the left input's columns, then the right one's (the probe's and the build's,
  // in the order of the sides).
  const int width = probe_schema.num_fields() + spec.schema()->num_fields();
  for (const plan::ExprPtr& expr : residual) {
    if (expr == nullptr || expr->type != plan::LogicalType::kBoolean) {
      return arrow::Status::Invalid("a hash join residual that is not BOOLEAN");
    }
    std::vector<int> columns;
    plan::CollectColumns(*expr, columns);
    if (std::ranges::any_of(columns, [&](int column) { return column < 0 || column >= width; })) {
      return arrow::Status::Invalid("a hash join residual reads a column outside the join");
    }
  }
  return std::unique_ptr<HashJoinOperator>(
      new HashJoinOperator(std::move(probe), std::move(build), std::move(probe_keys), build_side,
                           std::move(residual), prepares, std::move(schema)));
}

HashJoinOperator::HashJoinOperator(std::unique_ptr<Operator> probe,
                                   std::shared_ptr<JoinBuild> build, std::vector<int> probe_keys,
                                   plan::BuildSide build_side, std::vector<plan::ExprPtr> residual,
                                   bool prepares, std::shared_ptr<arrow::Schema> schema)
    : input_(std::move(probe)),
      build_(std::move(build)),
      probe_keys_(std::move(probe_keys)),
      build_side_(build_side),
      residual_(std::move(residual)),
      prepares_(prepares),
      schema_(std::move(schema)),
      kind_(build_->kind()),
      pair_schema_(kind_ != plan::JoinKind::kInner && !residual_.empty()
                       ? PairSchema(*input_->output_schema(), *build_->spec()->schema())
                       : nullptr) {}

HashJoinOperator::~HashJoinOperator() = default;

arrow::Status HashJoinOperator::Open(ExecContext& ctx) {
  ARROW_RETURN_NOT_OK(Close());
  ctx_ = ctx;
  batch_size_ = std::max<int64_t>(ctx.batch_size, 1);
  memory_.Reset(ctx.budget);
  opened_ = true;
  if (prepares_) {
    return arrow::Status::OK();  // the first Next prepares the build
  }
  ARROW_RETURN_NOT_OK(Start());
  started_ = true;
  return arrow::Status::OK();
}

arrow::Status HashJoinOperator::Start() {
  if (prepares_) {
    ARROW_RETURN_NOT_OK(build_->Prepare(ctx_));
  }
  table_ = build_->table();
  if (table_ == nullptr) {
    return arrow::Status::Invalid("a hash join probe before its build is prepared");
  }
  if (EmptiesJoin(kind_, *table_)) {
    End();  // no row can come out: the input is never opened
    return arrow::Status::OK();
  }
  switch (kind_) {
    case plan::JoinKind::kAnti:
      keep_ = table_->num_rows() == 0 ? Keep::kAll : Keep::kUnmatched;
      break;
    case plan::JoinKind::kNullAwareAnti:
      // A NULL in the set leaves no row (each one's NOT IN is false or NULL), yet the input is read
      // to its end, as DuckDB reads it; an empty set keeps every row, a NULL key included.
      if (table_->has_null()) {
        keep_ = Keep::kNone;
      } else {
        keep_ = table_->empty() ? Keep::kAll : Keep::kUnmatchedNotNull;
      }
      break;
    case plan::JoinKind::kOneRow:
      values_ = build_->values();  // made with the table, which holds its one row
      ANTB1_CHECK(values_ != nullptr);
      keep_ = Keep::kAll;
      break;
    default:  // inner, semi
      keep_ = Keep::kMatched;
      break;
  }
  done_ = false;
  input_opened_ = true;  // Close closes it even if its Open fails
  return input_->Open(ctx_);
}

arrow::Result<Batch> HashJoinOperator::Next() {
  if (!opened_) {
    return arrow::Status::Invalid("hash join: Next() before Open() or after Close()");
  }
  if (!started_) {
    ARROW_RETURN_NOT_OK(Start());
    started_ = true;
  }
  while (!done_) {
    if (batch_.data == nullptr) {
      ARROW_RETURN_NOT_OK(Pull());
      continue;
    }
    ARROW_ASSIGN_OR_RAISE(std::optional<Batch> out, NextOutput());
    if (out.has_value()) {
      return *std::move(out);
    }
  }
  return Batch{};
}

arrow::Result<std::optional<Batch>> HashJoinOperator::NextOutput() {
  switch (kind_) {
    case plan::JoinKind::kInner:
      return table_->unique() ? NextWindow() : NextPairs();
    case plan::JoinKind::kLeft:
      return table_->unique() && residual_.empty() ? NextLeftWindow() : NextPadded();
    case plan::JoinKind::kOneRow:
      return NextOneRow();
    default:  // semi, anti, null-aware anti (Make takes no other kind)
      if (!residual_.empty() && keep_ != Keep::kAll && !evaluated_) {
        ARROW_RETURN_NOT_OK(EvaluatePairs());  // before the batch's first window
      }
      return NextSelected();
  }
}

arrow::Status HashJoinOperator::Pull() {
  ARROW_ASSIGN_OR_RAISE(Batch in, input_->Next());
  if (in.end()) {
    End();
    return arrow::Status::OK();
  }
  if (keep_ == Keep::kNone || in.selected_rows() == 0) {
    return arrow::Status::OK();  // read, and dropped
  }
  if (std::cmp_greater(in.data->num_rows(), std::numeric_limits<std::uint32_t>::max())) {
    return arrow::Status::CapacityError("a join probe batch of ", in.data->num_rows(), " rows");
  }
  if (keep_ != Keep::kAll) {  // the rows' matches decide
    keys_.clear();
    for (const int key : probe_keys_) {
      keys_.push_back(in.data->column(key));
    }
    ARROW_RETURN_NOT_OK(Fit(matches_, static_cast<std::size_t>(in.data->num_rows()), memory_));
    const ProfileTimer find(profile(), "find");
    ARROW_RETURN_NOT_OK(table_->Find(keys_, in.selection.get(), ctx_.pool, matches_));
  }
  batch_ = std::move(in);
  row_ = 0;
  taken_ = 0;
  window_ = 0;
  evaluated_ = false;
  row_passed_ = false;
  return arrow::Status::OK();
}

std::optional<std::pair<int64_t, int64_t>> HashJoinOperator::NextRange(int64_t limit) {
  const int64_t length = batch_.data->num_rows();
  if (window_ >= length) {
    batch_ = Batch{};
    return std::nullopt;
  }
  const int64_t begin = window_;
  const int64_t rows = std::min(limit, length - begin);
  window_ = begin + rows;
  return std::pair(begin, rows);
}

std::optional<std::shared_ptr<arrow::BooleanArray>> HashJoinOperator::SelectionOf(
    int64_t begin, int64_t rows) const {
  if (batch_.selection == nullptr) {
    return std::shared_ptr<arrow::BooleanArray>();
  }
  auto selection =
      std::static_pointer_cast<arrow::BooleanArray>(batch_.selection->Slice(begin, rows));
  const int64_t selected = selection->true_count();
  if (selected == 0) {
    return std::nullopt;
  }
  if (selected == rows) {
    selection = nullptr;
  }
  return selection;
}

bool HashJoinOperator::Selected(int64_t row) const {
  return batch_.selection == nullptr ||
         (batch_.selection->IsValid(row) && batch_.selection->Value(row));
}

arrow::Result<std::optional<Batch>> HashJoinOperator::NextSelected() {
  const std::optional<std::pair<int64_t, int64_t>> range = NextRange(batch_size_);
  if (!range.has_value()) {
    return std::nullopt;
  }
  const auto [begin, rows] = *range;
  // The rows the window keeps: one pass counts them, a second sets their bits if some are not.
  const auto window = [&](const auto& keeps) -> arrow::Result<std::optional<Batch>> {
    int64_t kept = 0;
    for (int64_t row = begin; row < begin + rows; ++row) {
      kept += keeps(row) ? 1 : 0;
    }
    if (kept == 0) {
      return std::nullopt;
    }
    std::shared_ptr<arrow::BooleanArray> selection;
    if (kept < rows) {
      ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::Buffer> bits,
                            arrow::AllocateEmptyBitmap(rows, ctx_.pool));
      for (int64_t row = begin; row < begin + rows; ++row) {
        if (keeps(row)) {
          arrow::bit_util::SetBit(bits->mutable_data(), row - begin);
        }
      }
      selection = std::make_shared<arrow::BooleanArray>(rows, std::move(bits));
    }
    return Batch{.data = batch_.data->Slice(begin, rows), .selection = std::move(selection)};
  };
  const auto matched = [this](int64_t row) {
    if (evaluated_) {  // with residuals: one of its pairs passed them all
      return passed_[static_cast<std::size_t>(row)] != 0;
    }
    const JoinMatches& m = matches_[static_cast<std::size_t>(row)];
    return m.end > m.begin;
  };
  switch (keep_) {
    case Keep::kMatched:  // Find gives no match to a row that is not selected
      return window(matched);
    case Keep::kUnmatched:
      return window([&](int64_t row) { return Selected(row) && !matched(row); });
    case Keep::kUnmatchedNotNull:
      return window(
          [&](int64_t row) { return Selected(row) && keys_[0]->IsValid(row) && !matched(row); });
    default:  // kAll (kNone never keeps a batch)
      return window([this](int64_t row) { return Selected(row); });
  }
}

arrow::Result<std::optional<Batch>> HashJoinOperator::NextOneRow() {
  const std::optional<std::pair<int64_t, int64_t>> range =
      NextRange(std::min(batch_size_, values_->rows));
  if (!range.has_value()) {
    return std::nullopt;
  }
  const auto [begin, rows] = *range;
  std::optional<std::shared_ptr<arrow::BooleanArray>> selection = SelectionOf(begin, rows);
  if (!selection.has_value()) {
    return std::nullopt;
  }
  arrow::ArrayVector build;
  {
    const ProfileTimer gather(profile(), "gather");
    build.reserve(values_->columns.size());
    for (const std::shared_ptr<arrow::Array>& column : values_->columns) {
      build.push_back(column->Slice(0, rows));
    }
  }
  const std::shared_ptr<arrow::RecordBatch> probe = batch_.data->Slice(begin, rows);
  return Batch{.data = Assemble(probe->columns(), build, rows), .selection = *std::move(selection)};
}

arrow::Result<std::optional<Batch>> HashJoinOperator::NextLeftWindow() {
  const std::optional<std::pair<int64_t, int64_t>> range = NextRange(batch_size_);
  if (!range.has_value()) {
    return std::nullopt;
  }
  const auto [begin, rows] = *range;
  std::optional<std::shared_ptr<arrow::BooleanArray>> selection = SelectionOf(begin, rows);
  if (!selection.has_value()) {
    return std::nullopt;
  }
  if (profile() != nullptr) {
    profile()->Add("window_rows", MetricUnit::kCount, rows);
  }
  ARROW_RETURN_NOT_OK(Fit(build_rows_, static_cast<std::size_t>(rows), memory_));
  const std::span<const JoinRowRef> table_rows = table_->rows();
  for (int64_t i = 0; i < rows; ++i) {  // NULL where a row has no match (padded)
    const JoinMatches& m = matches_[static_cast<std::size_t>(begin + i)];
    build_rows_[static_cast<std::size_t>(i)] =
        m.end > m.begin ? table_rows[m.begin] : JoinRowRef{.chunk = kNoChunk, .row = 0};
  }
  ARROW_ASSIGN_OR_RAISE(const arrow::ArrayVector build, GatherBuild<true>(rows));
  const std::shared_ptr<arrow::RecordBatch> probe = batch_.data->Slice(begin, rows);
  return Batch{.data = Assemble(probe->columns(), build, rows), .selection = *std::move(selection)};
}

arrow::Result<std::optional<Batch>> HashJoinOperator::NextPadded() {
  const bool continues = taken_ > 0;
  ARROW_ASSIGN_OR_RAISE(int64_t rows, NextSlots());
  if (rows == 0) {
    batch_ = Batch{};
    return std::nullopt;
  }
  if (!residual_.empty()) {
    ARROW_ASSIGN_OR_RAISE(const int64_t passing, PassingSlots(rows));
    rows = KeepPassingOrPadded(rows, passing, continues);
    if (rows == 0) {
      return std::nullopt;
    }
  }
  ARROW_ASSIGN_OR_RAISE(const arrow::ArrayVector probe,
                        TakeProbe(std::span<const std::uint32_t>(probe_rows_.data(),
                                                                 static_cast<std::size_t>(rows))));
  ARROW_ASSIGN_OR_RAISE(const arrow::ArrayVector build, GatherBuild<true>(rows));
  return Batch{.data = Assemble(probe, build, rows), .selection = {}};
}

int64_t HashJoinOperator::KeepPassingOrPadded(int64_t count, int64_t passing, bool continues) {
  // Each row's pairs come in slot order, its last candidate last; `any`: one of the current row's
  // pairs passed, in these slots or (for a row that continues) in the slots before.
  int64_t current = -1;
  bool any = false;
  if (continues) {
    current = probe_rows_.front();
    any = row_passed_;
  }
  int64_t next_passing = 0;  // in pair_slots_
  int64_t kept = 0;
  for (int64_t s = 0; s < count; ++s) {
    const auto slot = static_cast<std::size_t>(s);
    const std::uint32_t row = probe_rows_[slot];
    if (row != current) {
      current = row;
      any = false;
    }
    JoinRowRef ref = build_rows_[slot];
    if (ref.chunk != kNoChunk) {  // a candidate: kept when it passed
      if (next_passing < passing &&
          pair_slots_[static_cast<std::size_t>(next_passing)] == static_cast<std::uint32_t>(s)) {
        ++next_passing;
        any = true;
      } else if (last_[slot] != 0 && !any) {
        ref = JoinRowRef{.chunk = kNoChunk, .row = 0};  // no candidate of the row passed
      } else {
        continue;
      }
    }
    probe_rows_[static_cast<std::size_t>(kept)] = row;
    build_rows_[static_cast<std::size_t>(kept)] = ref;
    ++kept;
  }
  // A row whose candidates go on in the next slots carries whether one of them passed.
  row_passed_ = taken_ > 0 && any;
  return kept;
}

arrow::Result<int64_t> HashJoinOperator::NextSlots() {
  const int64_t length = batch_.data->num_rows();
  const int64_t limit = std::min<int64_t>(batch_size_, std::numeric_limits<std::uint32_t>::max());
  const bool pads = kind_ == plan::JoinKind::kLeft;
  // Calls emit(probe row, candidate (kPadded: none), slot, whether it is the row's last candidate)
  // for the slots from (row_, taken_) on, at most `limit` of them; the position after the last.
  const auto walk = [&](const auto& emit) {
    int64_t row = row_;
    std::uint32_t taken = taken_;
    int64_t count = 0;
    while (row < length && count < limit) {
      const JoinMatches& m = matches_[static_cast<std::size_t>(row)];
      const std::uint32_t candidates = m.end - m.begin;
      if (candidates == 0) {
        if (pads && Selected(row)) {
          emit(row, kPadded, count, true);
          ++count;
        }
        ++row;
        continue;
      }
      const auto take =
          static_cast<std::uint32_t>(std::min<int64_t>(candidates - taken, limit - count));
      for (std::uint32_t k = 0; k < take; ++k) {
        emit(row, m.begin + taken + k, count + k, taken + k + 1 == candidates);
      }
      count += take;
      taken += take;
      if (taken == candidates) {
        ++row;
        taken = 0;
      }
    }
    return std::tuple(row, taken, count);
  };
  const auto [row, taken, count] = walk(
      [](int64_t /*probe_row*/, std::uint32_t /*candidate*/, int64_t /*at*/, bool /*last*/) {});
  if (count > 0) {
    const auto size = static_cast<std::size_t>(count);
    ARROW_RETURN_NOT_OK(Fit(probe_rows_, size, memory_));
    ARROW_RETURN_NOT_OK(Fit(build_rows_, size, memory_));
    const bool marks = pads && !residual_.empty();
    if (marks) {
      ARROW_RETURN_NOT_OK(Fit(last_, size, memory_));
    }
    const std::span<const JoinRowRef> table_rows = table_->rows();
    walk([&](int64_t probe_row, std::uint32_t candidate, int64_t at, bool last) {
      const auto slot = static_cast<std::size_t>(at);
      probe_rows_[slot] = Narrow<std::uint32_t>(probe_row);
      build_rows_[slot] =
          candidate == kPadded ? JoinRowRef{.chunk = kNoChunk, .row = 0} : table_rows[candidate];
      if (marks) {
        last_[slot] = last ? 1 : 0;
      }
    });
  }
  row_ = row;
  taken_ = taken;
  return count;
}

arrow::Result<int64_t> HashJoinOperator::PassingSlots(int64_t count) {
  // The candidate slots: those with a build row.
  const auto size = static_cast<std::size_t>(count);
  ARROW_RETURN_NOT_OK(Fit(pair_rows_, size, memory_));
  ARROW_RETURN_NOT_OK(Fit(pair_builds_, size, memory_));
  ARROW_RETURN_NOT_OK(Fit(pair_slots_, size, memory_));
  std::size_t pairs = 0;
  for (std::size_t s = 0; s < size; ++s) {
    if (build_rows_[s].chunk != kNoChunk) {
      pair_rows_[pairs] = probe_rows_[s];
      pair_builds_[pairs] = build_rows_[s];
      pair_slots_[pairs] = static_cast<std::uint32_t>(s);  // s < 2^32 - 1 (NextSlots)
      ++pairs;
    }
  }
  if (pairs == 0) {
    return 0;
  }
  ARROW_ASSIGN_OR_RAISE(arrow::ArrayVector columns,
                        TakeProbe(std::span<const std::uint32_t>(pair_rows_.data(), pairs)));
  ARROW_ASSIGN_OR_RAISE(
      const arrow::ArrayVector build,
      GatherBuild<false>(std::span<const JoinRowRef>(pair_builds_.data(), pairs)));
  columns.insert(columns.end(), build.begin(), build.end());
  const auto length = static_cast<int64_t>(pairs);
  columns.push_back(
      std::make_shared<arrow::UInt32Array>(length, arrow::Buffer::Wrap(pair_slots_.data(), pairs)));
  std::shared_ptr<arrow::RecordBatch> batch =
      arrow::RecordBatch::Make(pair_schema_, length, std::move(columns));
  const ProfileTimer residual(profile(), "residual");
  arrow::compute::ExecContext kernels(ctx_.pool);
  for (const plan::ExprPtr& expr : residual_) {
    // Only the pairs the residuals before passed: a later one never fails on a dropped pair.
    ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::Array> values,
                          EvaluateExpr(*expr, *batch, ctx_.pool));
    const int64_t passing = static_cast<const arrow::BooleanArray&>(*values).true_count();
    if (passing == 0) {
      return 0;
    }
    if (passing < batch->num_rows()) {  // the filter drops NULL as it drops false
      ARROW_ASSIGN_OR_RAISE(
          const arrow::Datum kept,
          arrow::compute::Filter(batch, values, arrow::compute::FilterOptions::Defaults(),
                                 &kernels));
      batch = kept.record_batch();
    }
  }
  const auto& slots =
      static_cast<const arrow::UInt32Array&>(*batch->column(batch->num_columns() - 1));
  if (slots.length() < length) {  // a copy: pair_slots_ itself when every pair passed
    std::copy_n(slots.raw_values(), slots.length(), pair_slots_.begin());
  }
  return slots.length();
}

arrow::Status HashJoinOperator::EvaluatePairs() {
  ARROW_RETURN_NOT_OK(Fit(passed_, static_cast<std::size_t>(batch_.data->num_rows()), memory_));
  std::ranges::fill(passed_, std::uint8_t{0});
  while (true) {
    ARROW_ASSIGN_OR_RAISE(const int64_t count, NextSlots());
    if (count == 0) {
      break;
    }
    ARROW_ASSIGN_OR_RAISE(const int64_t passing, PassingSlots(count));
    for (int64_t i = 0; i < passing; ++i) {
      passed_[probe_rows_[pair_slots_[static_cast<std::size_t>(i)]]] = 1;
    }
  }
  evaluated_ = true;
  return arrow::Status::OK();
}

arrow::Result<arrow::ArrayVector> HashJoinOperator::TakeProbe(std::span<const std::uint32_t> rows) {
  const ProfileTimer gather(profile(), "gather");
  const auto indices = std::make_shared<arrow::UInt32Array>(
      static_cast<int64_t>(rows.size()), arrow::Buffer::Wrap(rows.data(), rows.size()));
  arrow::compute::ExecContext kernels(ctx_.pool);
  ARROW_ASSIGN_OR_RAISE(
      const arrow::Datum taken,
      arrow::compute::Take(batch_.data, indices, arrow::compute::TakeOptions::NoBoundsCheck(),
                           &kernels));
  return taken.record_batch()->columns();
}

arrow::Result<std::optional<Batch>> HashJoinOperator::NextWindow() {
  const std::optional<std::pair<int64_t, int64_t>> range = NextRange(batch_size_);
  if (!range.has_value()) {
    return std::nullopt;
  }
  const auto [begin, rows] = *range;
  const std::span<const JoinMatches> matches = std::span<const JoinMatches>(matches_).subspan(
      static_cast<std::size_t>(begin), static_cast<std::size_t>(rows));
  const int64_t matched =
      std::ranges::count_if(matches, [](const JoinMatches& m) { return m.end > m.begin; });
  if (matched == 0) {
    return std::nullopt;
  }
  if (profile() != nullptr) {
    profile()->Add("window_rows", MetricUnit::kCount, rows);
  }
  ARROW_RETURN_NOT_OK(Fit(build_rows_, static_cast<std::size_t>(rows), memory_));
  const std::span<const JoinRowRef> table_rows = table_->rows();
  for (std::size_t i = 0; i < matches.size(); ++i) {
    build_rows_[i] = matches[i].end > matches[i].begin ? table_rows[matches[i].begin]
                                                       : JoinRowRef{.chunk = kNoChunk, .row = 0};
  }
  ARROW_ASSIGN_OR_RAISE(const arrow::ArrayVector build, GatherBuild<true>(rows));
  std::shared_ptr<arrow::BooleanArray> selection;
  if (matched < rows) {
    ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::Buffer> bits,
                          arrow::AllocateEmptyBitmap(rows, ctx_.pool));
    for (std::size_t i = 0; i < matches.size(); ++i) {
      if (matches[i].end > matches[i].begin) {
        arrow::bit_util::SetBit(bits->mutable_data(), static_cast<int64_t>(i));
      }
    }
    selection = std::make_shared<arrow::BooleanArray>(rows, std::move(bits));
  }
  const std::shared_ptr<arrow::RecordBatch> probe = batch_.data->Slice(begin, rows);
  return ApplyResiduals(
      Batch{.data = Assemble(probe->columns(), build, rows), .selection = std::move(selection)});
}

arrow::Result<std::optional<Batch>> HashJoinOperator::NextPairs() {
  const int64_t length = batch_.data->num_rows();
  // Calls emit(probe row, match) for the pairs from (row_, taken_) on, at most batch_size_ of them;
  // the position after the last one.
  const auto walk = [&](const auto& emit) {
    int64_t row = row_;
    std::uint32_t taken = taken_;
    int64_t count = 0;
    while (row < length && count < batch_size_) {
      const JoinMatches& m = matches_[static_cast<std::size_t>(row)];
      const std::uint32_t remaining = m.end - m.begin - taken;
      const auto take =
          static_cast<std::uint32_t>(std::min<int64_t>(remaining, batch_size_ - count));
      for (std::uint32_t k = 0; k < take; ++k) {
        emit(row, m.begin + taken + k, count + k);
      }
      count += take;
      taken += take;
      if (taken == m.end - m.begin) {
        ++row;
        taken = 0;
      }
    }
    return std::tuple(row, taken, count);
  };
  const auto [row, taken, count] =
      walk([](int64_t /*probe_row*/, std::uint32_t /*match*/, int64_t /*at*/) {});
  if (count == 0) {
    batch_ = Batch{};
    return std::nullopt;
  }
  ARROW_RETURN_NOT_OK(Fit(probe_rows_, static_cast<std::size_t>(count), memory_));
  ARROW_RETURN_NOT_OK(Fit(build_rows_, static_cast<std::size_t>(count), memory_));
  const std::span<const JoinRowRef> table_rows = table_->rows();
  walk([&](int64_t probe_row, std::uint32_t match, int64_t at) {
    probe_rows_[static_cast<std::size_t>(at)] = Narrow<std::uint32_t>(probe_row);
    build_rows_[static_cast<std::size_t>(at)] = table_rows[match];
  });
  row_ = row;
  taken_ = taken;
  arrow::ArrayVector probe;
  {
    const ProfileTimer gather(profile(), "gather");
    const auto indices =
        std::make_shared<arrow::UInt32Array>(count, arrow::Buffer::Wrap(probe_rows_.data(), count));
    arrow::compute::ExecContext kernels(ctx_.pool);
    ARROW_ASSIGN_OR_RAISE(
        const arrow::Datum taken_rows,
        arrow::compute::Take(batch_.data, indices, arrow::compute::TakeOptions::NoBoundsCheck(),
                             &kernels));
    probe = taken_rows.record_batch()->columns();
  }
  ARROW_ASSIGN_OR_RAISE(const arrow::ArrayVector build, GatherBuild<false>(count));
  return ApplyResiduals(Batch{.data = Assemble(probe, build, count), .selection = {}});
}

template <bool kWithNulls>
arrow::Result<arrow::ArrayVector> HashJoinOperator::GatherBuild(int64_t rows) {
  return GatherBuild<kWithNulls>(
      std::span<const JoinRowRef>(build_rows_.data(), static_cast<std::size_t>(rows)));
}

template <bool kWithNulls>
arrow::Result<arrow::ArrayVector> HashJoinOperator::GatherBuild(std::span<const JoinRowRef> refs) {
  const ProfileTimer gather(profile(), "gather");
  const arrow::Schema& schema = *table_->spec().schema();
  arrow::ArrayVector columns;
  columns.reserve(static_cast<std::size_t>(schema.num_fields()));
  for (int c = 0; c < schema.num_fields(); ++c) {
    ARROW_ASSIGN_OR_RAISE(std::unique_ptr<arrow::ArrayBuilder> builder,
                          arrow::MakeBuilder(schema.field(c)->type(), ctx_.pool));
    ARROW_RETURN_NOT_OK((GatherRows<kWithNulls, JoinRowRef>(table_->chunks(), c, refs, *builder)));
    ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::Array> column, builder->Finish());
    columns.push_back(std::move(column));
  }
  return columns;
}

std::shared_ptr<arrow::RecordBatch> HashJoinOperator::Assemble(const arrow::ArrayVector& probe,
                                                               const arrow::ArrayVector& build,
                                                               int64_t rows) const {
  const bool build_left = build_side_ == plan::BuildSide::kLeft;
  arrow::ArrayVector columns = build_left ? build : probe;
  const arrow::ArrayVector& right = build_left ? probe : build;
  columns.insert(columns.end(), right.begin(), right.end());
  return arrow::RecordBatch::Make(schema_, rows, std::move(columns));
}

arrow::Result<std::optional<Batch>> HashJoinOperator::ApplyResiduals(Batch out) {
  if (residual_.empty()) {
    return out;
  }
  const ProfileTimer residual(profile(), "residual");
  ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::RecordBatch> rows, Materialize(out, ctx_.pool));
  arrow::compute::ExecContext kernels(ctx_.pool);
  for (const plan::ExprPtr& expr : residual_) {
    // Only the rows the residuals before passed: a later one never fails on a dropped row.
    ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::Array> values,
                          EvaluateExpr(*expr, *rows, ctx_.pool));
    const int64_t passing = static_cast<const arrow::BooleanArray&>(*values).true_count();
    if (passing == 0) {
      return std::nullopt;
    }
    if (passing < rows->num_rows()) {  // the filter drops NULL as it drops false
      ARROW_ASSIGN_OR_RAISE(const arrow::Datum kept,
                            arrow::compute::Filter(
                                rows, values, arrow::compute::FilterOptions::Defaults(), &kernels));
      rows = kept.record_batch();
    }
  }
  return Batch{.data = std::move(rows), .selection = {}};
}

void HashJoinOperator::End() {
  done_ = true;
  batch_ = Batch{};
  Free(keys_);
  Free(matches_);
  Free(probe_rows_);
  Free(build_rows_);
  Free(last_);
  Free(passed_);
  Free(pair_rows_);
  Free(pair_builds_);
  Free(pair_slots_);
  memory_.Release();
  evaluated_ = false;
  row_passed_ = false;
  table_.reset();
  values_.reset();
  if (prepares_) {
    build_->Release();
  }
}

arrow::Status HashJoinOperator::Close() {
  arrow::Status status;
  if (input_opened_) {
    status = input_->Close();
    input_opened_ = false;
  }
  End();
  opened_ = false;
  started_ = false;
  done_ = false;
  return status;
}

}  // namespace antb1::exec
