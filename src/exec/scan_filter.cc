#include "antb1/exec/scan_filter.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <arrow/api.h>
#include <arrow/util/bit_util.h>
#include <arrow/util/bitmap_ops.h>

#include "antb1/exec/filter.h"
#include "antb1/exec/like.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/table.h"

namespace antb1::exec {
namespace {

using Kind = plan::Predicate::Kind;

bool BitAt(const std::uint8_t* bits, int64_t i) {
  return arrow::bit_util::GetBit(bits, static_cast<std::uint64_t>(i));
}

// Clears the bits of the selected rows of `values` that are NULL or fail `test`.
template <typename Test>
void KeepStrings(const plan::ScanValues& values, int64_t offset, std::uint8_t* selected,
                 const Test& test) {
  for (int64_t i = 0; i < values.rows; ++i) {
    if (!BitAt(selected, offset + i)) {
      continue;
    }
    if ((values.validity != nullptr && !BitAt(values.validity, i)) ||
        !test(values.strings[static_cast<std::size_t>(i)])) {
      arrow::bit_util::ClearBit(selected, offset + i);
    }
  }
}

// One predicate on a VARCHAR column, evaluated on views.
class StringPredicate {
 public:
  static arrow::Result<StringPredicate> Make(const plan::Predicate& p) {
    StringPredicate out;
    out.kind_ = p.kind;
    out.op_ = p.op;
    const auto text = [](const plan::Constant& c) -> arrow::Result<std::string> {
      const auto* bytes = std::get_if<std::string>(&c.value);
      if (bytes == nullptr) {
        return arrow::Status::Invalid("a VARCHAR column compared with a non-VARCHAR constant");
      }
      return *bytes;
    };
    switch (p.kind) {
      case Kind::kCompare: {
        ARROW_ASSIGN_OR_RAISE(out.constant_, text(p.constant));
        break;
      }
      case Kind::kLike:
      case Kind::kNotLike: {
        ARROW_ASSIGN_OR_RAISE(const std::string pattern, text(p.constant));
        out.pattern_.emplace(pattern);
        break;
      }
      case Kind::kIn:
      case Kind::kNotIn:
        if (p.values.empty()) {
          return arrow::Status::Invalid("IN without values");
        }
        for (const plan::Constant& value : p.values) {
          ARROW_ASSIGN_OR_RAISE(std::string bytes, text(value));
          out.values_.push_back(std::move(bytes));
        }
        std::ranges::sort(out.values_);
        break;
      default:
        break;
    }
    return out;
  }

  void Apply(const plan::ScanValues& values, int64_t offset, std::uint8_t* selected) const {
    const std::string_view c = constant_;
    const auto keep = [&](const auto& test) { KeepStrings(values, offset, selected, test); };
    switch (kind_) {
      case Kind::kCompare:
        switch (op_) {
          case plan::CompareOp::kEq:
            keep([c](std::string_view v) { return v == c; });
            break;
          case plan::CompareOp::kNe:
            keep([c](std::string_view v) { return v != c; });
            break;
          case plan::CompareOp::kLt:
            keep([c](std::string_view v) { return v < c; });
            break;
          case plan::CompareOp::kLe:
            keep([c](std::string_view v) { return v <= c; });
            break;
          case plan::CompareOp::kGt:
            keep([c](std::string_view v) { return v > c; });
            break;
          case plan::CompareOp::kGe:
            keep([c](std::string_view v) { return v >= c; });
            break;
        }
        break;
      case Kind::kLike:
        keep([this](std::string_view v) { return pattern_->Matches(v); });
        break;
      case Kind::kNotLike:
        keep([this](std::string_view v) { return !pattern_->Matches(v); });
        break;
      case Kind::kIn:
        keep([this](std::string_view v) {
          return std::ranges::binary_search(values_, v, std::less<>());
        });
        break;
      case Kind::kNotIn:
        keep([this](std::string_view v) {
          return !std::ranges::binary_search(values_, v, std::less<>());
        });
        break;
      default:  // kIsNotNull: NULL fails
        keep([](std::string_view) { return true; });
        break;
    }
  }

 private:
  Kind kind_ = Kind::kIsNotNull;
  plan::CompareOp op_ = plan::CompareOp::kEq;
  std::string constant_;                // kCompare
  std::optional<LikePattern> pattern_;  // kLike and kNotLike
  std::vector<std::string> values_;     // kIn and kNotIn, sorted bytewise
};

// The predicates of one scan column.
struct ColumnPredicates {
  bool strings = false;                     // VARCHAR: evaluated on views
  std::shared_ptr<arrow::Schema> schema;    // the column alone (fixed-width)
  std::vector<PredicateEvaluator> fixed;    // fixed-width, on the column alone
  std::vector<StringPredicate> on_strings;  // VARCHAR
};

class PredicateScanFilter final : public plan::ScanFilter {
 public:
  PredicateScanFilter(std::vector<int> columns, std::vector<ColumnPredicates> predicates,
                      arrow::MemoryPool* pool)
      : columns_(std::move(columns)), predicates_(std::move(predicates)), pool_(pool) {}

  [[nodiscard]] const std::vector<int>& columns() const override { return columns_; }

  arrow::Status Apply(int column, const plan::ScanValues& values, int64_t offset,
                      std::uint8_t* selected) const override {
    if (column < 0 || std::cmp_greater_equal(column, predicates_.size())) {
      return arrow::Status::Invalid("scan filter: no column ", column);
    }
    const ColumnPredicates& c = predicates_[static_cast<std::size_t>(column)];
    if (c.strings) {
      if (values.array != nullptr || std::cmp_not_equal(values.strings.size(), values.rows)) {
        return arrow::Status::Invalid("scan filter: a VARCHAR column needs one view per row");
      }
      for (const StringPredicate& p : c.on_strings) {
        p.Apply(values, offset, selected);
      }
      return arrow::Status::OK();
    }
    if (values.array == nullptr || values.array->length() != values.rows ||
        !values.array->type()->Equals(*c.schema->field(0)->type())) {
      return arrow::Status::Invalid("scan filter: values of another type than the column's");
    }
    const auto batch = arrow::RecordBatch::Make(c.schema, values.rows, {values.array});
    for (const PredicateEvaluator& evaluator : c.fixed) {
      ARROW_ASSIGN_OR_RAISE(const arrow::Datum result,
                            evaluator.Evaluate(*batch, pool_, /*kleene=*/false));
      ARROW_ASSIGN_OR_RAISE(const auto passed,
                            result.is_array()
                                ? arrow::Result<std::shared_ptr<arrow::Array>>(result.make_array())
                                : arrow::MakeArrayFromScalar(*result.scalar(), values.rows, pool_));
      // selected &= value, and &= validity where the result has NULLs (NULL fails).
      const arrow::ArrayData& mask = *passed->data();
      ARROW_ASSIGN_OR_RAISE(auto kept, arrow::AllocateEmptyBitmap(values.rows, pool_));
      arrow::internal::BitmapAnd(selected, offset, mask.buffers[1]->data(), mask.offset,
                                 values.rows, 0, kept->mutable_data());
      if (passed->null_count() > 0) {
        ARROW_ASSIGN_OR_RAISE(auto valid, arrow::AllocateEmptyBitmap(values.rows, pool_));
        arrow::internal::BitmapAnd(kept->data(), 0, mask.buffers[0]->data(), mask.offset,
                                   values.rows, 0, valid->mutable_data());
        kept = std::move(valid);
      }
      arrow::internal::CopyBitmap(kept->data(), 0, values.rows, selected, offset);
    }
    return arrow::Status::OK();
  }

 private:
  std::vector<int> columns_;
  std::vector<ColumnPredicates> predicates_;  // per position of columns_
  arrow::MemoryPool* pool_;
};

}  // namespace

bool PushableToScan(const plan::Predicate& predicate) {
  switch (predicate.kind) {
    case Kind::kCompare:
    case Kind::kIn:
    case Kind::kNotIn:
    case Kind::kLike:
    case Kind::kNotLike:
    case Kind::kIsNotNull:
      return predicate.column.has_value();
    case Kind::kCompareColumns:
    case Kind::kFalse:
    case Kind::kIsTrue:
      return false;
  }
  return false;
}

arrow::Result<std::shared_ptr<const plan::ScanFilter>> MakeScanFilter(
    const std::vector<plan::Predicate>& predicates, const arrow::Schema& schema,
    arrow::MemoryPool* pool) {
  std::vector<int> columns;
  std::vector<ColumnPredicates> per_column;
  for (const plan::Predicate& p : predicates) {
    if (!PushableToScan(p) || !p.column.has_value()) {
      return arrow::Status::Invalid("a predicate the scan cannot apply");
    }
    const int index = p.column->index;
    if (index < 0 || index >= schema.num_fields()) {
      return arrow::Status::Invalid("scan filter on a column outside the scan");
    }
    const auto found = std::ranges::find(columns, index);
    const auto position = static_cast<std::size_t>(found - columns.begin());
    if (found == columns.end()) {
      columns.push_back(index);
      ColumnPredicates c;
      c.strings = schema.field(index)->type()->id() == arrow::Type::BINARY;
      c.schema = arrow::schema({schema.field(index)});
      per_column.push_back(std::move(c));
    }
    ColumnPredicates& c = per_column[position];
    if (c.strings) {
      // Same checks and errors as the Filter operator's.
      ARROW_RETURN_NOT_OK(PredicateEvaluator::Make(p, schema).status());
      ARROW_ASSIGN_OR_RAISE(StringPredicate string, StringPredicate::Make(p));
      c.on_strings.push_back(std::move(string));
    } else {
      plan::Predicate alone = p;  // over the column alone
      alone.column = plan::BoundColumn{.index = 0, .name = p.column->name, .type = p.column->type};
      ARROW_ASSIGN_OR_RAISE(PredicateEvaluator evaluator,
                            PredicateEvaluator::Make(alone, *c.schema));
      c.fixed.push_back(std::move(evaluator));
    }
  }
  return std::make_shared<const PredicateScanFilter>(std::move(columns), std::move(per_column),
                                                     pool);
}

}  // namespace antb1::exec
