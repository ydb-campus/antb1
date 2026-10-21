#include "antb1/exec/scan_filter.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

#include "antb1/common/narrow.h"
#include "antb1/exec/filter.h"
#include "antb1/exec/like.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/table.h"

#include "group_table.h"

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

// The same, evaluating `test` once per distinct value of the chunk's dictionary and then reading
// one index a row. Every predicate kind gets this and none of them knows about it: a dictionary
// says nothing about what is being asked of a value, only that the same value recurs (ADR 0020).
//
// Taken only when there are no more distinct values than rows in the piece. The dictionary belongs
// to the whole column chunk while a piece is at most a page of it, so a dictionary far larger than
// the piece would cost more evaluations than the rows it saves -- the one shape where asking per
// value is the slower question.
template <class Test>
bool KeepByDictionary(const plan::ScanValues& values, int64_t offset, std::uint8_t* selected,
                      const Test& test) {
  if (values.dictionary.empty() || values.indices.size() != static_cast<std::size_t>(values.rows) ||
      std::cmp_greater(values.dictionary.size(), values.rows)) {
    return false;
  }
  std::vector<bool> matches(values.dictionary.size());
  for (std::size_t d = 0; d < values.dictionary.size(); ++d) {
    matches[d] = test(values.dictionary[d]);
  }
  for (int64_t i = 0; i < values.rows; ++i) {
    if (!BitAt(selected, offset + i)) {
      continue;
    }
    const auto row = static_cast<std::size_t>(i);
    if ((values.validity != nullptr && !BitAt(values.validity, i)) ||
        !matches[static_cast<std::size_t>(values.indices[row])]) {
      arrow::bit_util::ClearBit(selected, offset + i);
    }
  }
  return true;
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
    const auto keep = [&](const auto& test) {
      if (!KeepByDictionary(values, offset, selected, test)) {
        KeepStrings(values, offset, selected, test);
      }
    };
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
        keep([this](std::string_view v) { return Holds(v); });
        break;
      case Kind::kNotIn:
        keep([this](std::string_view v) { return !Holds(v); });
        break;
      default:  // kIsNotNull: NULL fails
        keep([](std::string_view) { return true; });
        break;
    }
  }

 private:
  // Whether the IN list holds `v`. A short list is walked rather than searched: a binary search
  // calls std::less<> on string_views, which is an out-of-line memcmp, where a length test rejects
  // most candidates before any byte is read. Measured over a seven-value CHAR column, 20M rows:
  // 12.5 ns a row for the search against 5.6 for the walk. The search wins again once the list is
  // long enough for log2(n) comparisons to beat n length tests, so both are kept and the list's
  // size chooses -- and where the column's dictionary is available neither runs more than once per
  // distinct value (KeepByDictionary).
  [[nodiscard]] bool Holds(std::string_view v) const {
    if (values_.size() > kLinearValues) {
      return std::ranges::binary_search(values_, v, std::less<>());
    }
    return std::ranges::any_of(values_, [v](const std::string& candidate) {
      return candidate.size() == v.size() && std::memcmp(candidate.data(), v.data(), v.size()) == 0;
    });
  }

  static constexpr std::size_t kLinearValues = 8;

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

// A hash join's key filter as a scan filter: one column, and a hash per row computed with the very
// function the build hashed its keys with, so the two cannot disagree. Immutable once made, and
// Apply is const, so every part of a query applies the one object concurrently.
class KeyScanFilter final : public plan::ScanFilter {
 public:
  KeyScanFilter(std::shared_ptr<const KeyFilter> filter, int column,
                std::shared_ptr<std::atomic<int64_t>> dropped)
      : filter_(std::move(filter)), columns_{column}, dropped_(std::move(dropped)) {}

  [[nodiscard]] const std::vector<int>& columns() const override { return columns_; }

  arrow::Status Apply(int column, const plan::ScanValues& values, int64_t offset,
                      std::uint8_t* selected) const override {
    if (column != 0) {
      return arrow::Status::Invalid("no such filter column");
    }
    if (values.array == nullptr) {
      // A VARCHAR column, which MakeKeyScanFilter refuses: nothing to hash here.
      return arrow::Status::Invalid("a key scan filter needs a fixed-width column");
    }
    // KeyHashes, not a hash of our own: the build hashed with it (JoinBuildPart's HashRows), and a
    // hash that differed by one bit would make the filter miss rows the join must keep.
    ARROW_ASSIGN_OR_RAISE(
        const std::vector<std::uint64_t> hashes,
        KeyHashes(arrow::compute::ExecBatch({values.array}, values.array->length())));
    // How far ahead to warm the filter's cache lines. A probe row's read has no locality, so
    // without this every row waits on one; the distance has to cover the latency at the rate the
    // loop runs, which is why it is far longer than the eight rows ADR 0019's table uses -- that
    // one walks a table it has just built and this one an 8 MB filter. Measured on the 80 M-row
    // probe of Q16 at SF 100, scan time summed over parts: 1.443 s at 8 rows, 1.321 at 16, 1.228
    // at 32, 1.142 at 64, 1.125 at 128 and back up to 1.189 at 256, where the lines are evicted
    // before they are read.
    constexpr int64_t kPrefetch = 128;
    int64_t dropped = 0;
    for (int64_t row = 0; row < values.rows; ++row) {
      if (row + kPrefetch < values.rows) {
        filter_->Prefetch(hashes[static_cast<std::size_t>(row + kPrefetch)]);
      }
      // GetBit takes an unsigned index and ClearBit a signed one, so each gets its own.
      const int64_t bit = offset + row;
      if (arrow::bit_util::GetBit(selected, Narrow<std::uint64_t>(bit)) &&
          !filter_->MayContain(hashes[static_cast<std::size_t>(row)])) {
        arrow::bit_util::ClearBit(selected, bit);
        ++dropped;
      }
    }
    // The rows this filter cleared, and only those: a row another filter had already cleared is not
    // counted, because the bit was tested before it was touched (D3).
    if (dropped_ != nullptr && dropped > 0) {
      dropped_->fetch_add(dropped, std::memory_order_relaxed);
    }
    return arrow::Status::OK();
  }

 private:
  std::shared_ptr<const KeyFilter> filter_;
  std::vector<int> columns_;
  std::shared_ptr<std::atomic<int64_t>> dropped_;  // nullptr: the scan is not counting
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

arrow::Result<std::shared_ptr<const plan::ScanFilter>> MakeKeyScanFilter(
    std::shared_ptr<const KeyFilter> filter, int column,
    std::shared_ptr<std::atomic<int64_t>> dropped) {
  if (filter == nullptr) {
    return arrow::Status::Invalid("a key scan filter without a filter");
  }
  if (column < 0) {
    return arrow::Status::Invalid("a key scan filter on a column outside the scan");
  }
  return std::make_shared<KeyScanFilter>(std::move(filter), column, std::move(dropped));
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
