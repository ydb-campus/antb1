#include "antb1/exec/scan_filter.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <numeric>
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
  // A byte a value, not std::vector<bool>: this is read once a row, and a bit-packed read costs a
  // shift and a mask where a byte costs a load. The allocation is one per piece per predicate,
  // against a piece of up to a page of rows.
  std::vector<std::uint8_t> matches(values.dictionary.size());
  for (std::size_t d = 0; d < values.dictionary.size(); ++d) {
    matches[d] = static_cast<std::uint8_t>(test(values.dictionary[d]) ? 1 : 0);
  }
  for (int64_t i = 0; i < values.rows; ++i) {
    if (!BitAt(selected, offset + i)) {
      continue;
    }
    const auto row = static_cast<std::size_t>(i);
    if ((values.validity != nullptr && !BitAt(values.validity, i)) ||
        matches[static_cast<std::size_t>(values.indices[row])] == 0) {
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
// Per-column cost, in units of a hundredth of a second over the measurement below, and measured
// end to end rather than guessed: one predicate on one column of 600 million rows at four threads,
// the column held constant so the decode is constant, wall seconds ->
//
//   a DATE column, one range bound        0.389   (two bounds: also 0.389)
//   a VARCHAR column, = one value         1.560
//   a VARCHAR column, IN two values       2.156
//   a VARCHAR column, LIKE a suffix       2.742
//
// What belongs in this weight is only the work a LATE position avoids, because that is the only
// work the order can move. A column's decode is not it: io reads every row of every one of the
// filter's columns whatever the selection says, and a fixed-width column's predicate is an Arrow
// kernel over the whole array (io::FilteredScanReader::FilterStrings and ReadFixed). What a late
// position does avoid is the per-row predicate work on a VARCHAR column, which skips the rows
// already rejected, and the copy of the values it keeps.
//
// So the 1.560 s above is mostly unavoidable and only its difference from 2.156 s is not: one value
// compared is 0.596 s. Taking a hundredth of a second as the unit,
//
//   one value compared, length and memcmp    60 units   (0.596 s)
//   one LIKE pattern matched                180 units   (2.742 s less the 0.964 s decode)
//   a fixed-width column's predicates         1 unit    (under the noise floor; see below)
//
// A fixed-width column gets 1 rather than 0 because two bounds on a DATE column cost what one bound
// does, 0.389 s either way, which puts its avoidable work under the ~0.005 s those repeats agree
// to -- under a single unit, not at zero. Zero would also break the comparator (see Order).
//
// Measured against the two orders this gets right and the old one got wrong: a fixed-width column
// removing a seventh of the rows, and a VARCHAR column with two predicates removing almost all of
// them, run 2.13 s with the fixed-width column first and 2.30 s with it last -- the fixed-width
// column should go first even though it removes far less, because its own work costs the same
// wherever it goes.
//
// A dictionary-encoded VARCHAR column is far cheaper than these weights (ADR 0020: 0.19 ns a row
// against 5.6), but whether Arrow will grant the dictionary is known only when the chunk is read,
// which is after this order is fixed. So the weights are the undiscounted ones, and a column that
// turns out to be dictionary-encoded is ordered more pessimistically than it deserves.
constexpr std::int64_t kFixedWidthCost = 1;
constexpr std::int64_t kStringCompareCost = 60;
constexpr std::int64_t kStringLikeCost = 180;

// The fraction of rows a predicate is assumed to keep, out of kAllSurvive, when nothing is known
// about the column's values. These mirror the no-statistics defaults of plan::PredicateRows
// (src/plan/join_order.cc) and are deliberately a separate, coarser statement: that one estimates
// a relation's rows for the join order and divides by a column's domain where the footers give
// one, while this one ranks columns within a scan and has no statistics at hand at all. Neither
// term dominates: what a column removes spans 10..100 of these, and nothing at all for the kinds
// assumed to keep every row, which is as wide a span as the weights above. So both halves of the
// rank decide real orders, and the next real improvement here is this column's own part
// statistics, which the scan could reach through plan::Table::part_stats.
constexpr std::int64_t kAllSurvive = 100;
constexpr std::int64_t kEqualSurvives = 10;
constexpr std::int64_t kRangeSurvives = 33;
constexpr std::int64_t kLikeSurvives = 25;

struct ColumnPredicates {
  bool strings = false;                     // VARCHAR: evaluated on views
  std::shared_ptr<arrow::Schema> schema;    // the column alone (fixed-width)
  std::vector<PredicateEvaluator> fixed;    // fixed-width, on the column alone
  std::vector<StringPredicate> on_strings;  // VARCHAR
  // What reading and filtering this column costs -- its read plus a term per predicate -- and what
  // fraction of rows survives it, for the order the scan applies the columns in (Order below). Both
  // are per column, because the unit the scan loops over is a column: it reads one, narrows the
  // selection, and only then reads the next.
  std::int64_t cost = 0;
  std::int64_t survives = kAllSurvive;  // out of kAllSurvive
};

class PredicateScanFilter final : public plan::ScanFilter {
 public:
  PredicateScanFilter(std::vector<int> columns, std::vector<ColumnPredicates> predicates,
                      arrow::MemoryPool* pool,
                      std::shared_ptr<std::atomic<int64_t>> dictionary_pieces)
      : columns_(std::move(columns)),
        predicates_(std::move(predicates)),
        pool_(pool),
        dictionary_pieces_(std::move(dictionary_pieces)) {}

  [[nodiscard]] const std::vector<int>& columns() const override { return columns_; }

  arrow::Status Apply(int column, const plan::ScanValues& values, int64_t offset,
                      std::uint8_t* selected) const override {
    if (column < 0 || std::cmp_greater_equal(column, predicates_.size())) {
      return arrow::Status::Invalid("scan filter: no column ", column);
    }
    const ColumnPredicates& c = predicates_[static_cast<std::size_t>(column)];
    if (c.strings) {
      // Either shape plan::ScanValues documents: one view per row, or a dictionary and one index
      // per row with no views at all -- which is what a column no batch carries is read as, since
      // its views would exist only to be compared. The second shape is admissible only when
      // KeepByDictionary will certainly take it, because the per-row fallback would otherwise read
      // views nobody wrote; the table produces it under that same condition and this re-checks it
      // rather than trusting it.
      if (values.array != nullptr) {
        return arrow::Status::Invalid("scan filter: a VARCHAR column is not an array");
      }
      const bool views = std::cmp_equal(values.strings.size(), values.rows);
      const bool by_dictionary = !values.dictionary.empty() &&
                                 values.indices.size() == static_cast<std::size_t>(values.rows) &&
                                 std::cmp_less_equal(values.dictionary.size(), values.rows);
      if (!views && !by_dictionary) {
        return arrow::Status::Invalid(
            "scan filter: a VARCHAR column needs one view per row, or a dictionary no larger than "
            "the piece with one index per row");
      }
      // Counted here because nothing above the scan can see it: whether a predicate was asked once
      // a distinct value or once a row is decided per piece, by the encoding of the chunk it came
      // from, and a scan that silently stops asking the cheap question looks exactly like one that
      // still asks it. One add a piece, not a row, and atomic because a ScanFilter is applied by
      // several parts at once.
      if (by_dictionary && dictionary_pieces_ != nullptr) {
        dictionary_pieces_->fetch_add(1, std::memory_order_relaxed);
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
  // How many (column, piece) pairs took the dictionary rather than one compare a row; nullptr:
  // not counted.
  std::shared_ptr<std::atomic<int64_t>> dictionary_pieces_;
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
    // The row count first, because the loop below runs to it while `hashes` is sized from the
    // array, and because a negative one would overflow the offset check's own subtraction. Then the
    // offset, the bit of the batch's first row, which plan::ScanFilter documents and every caller
    // gives: this is the one filter that does arithmetic on it, and `offset + row` is the bit
    // ClearBit writes, so a negative offset -- or one so large that the sum wraps -- would clear a
    // bit before the mask instead of in it.
    if (values.rows < 0 || values.array->length() != values.rows) {
      return arrow::Status::Invalid("a key scan filter was given ", values.rows, " rows of a ",
                                    values.array->length(), "-row column");
    }
    if (offset < 0 || offset > std::numeric_limits<int64_t>::max() - values.rows) {
      return arrow::Status::Invalid("a scan filter's offset is outside the selection: ", offset);
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
    //
    // **It prefetches for every row, including rows another filter has already cleared, and that is
    // not an oversight.** Where this filter shares a scan with pushed predicates
    // (MakeBothScanFilters) most bits are clear by the time the key column arrives, so the obvious
    // economy is to test the bit before issuing the prefetch. Measured, that is **1.010x over the
    // 22 and 1.028x on the query it was meant to help** (7 repeats, interleaved and rotated, the
    // guard the only change). A prefetch is a non-blocking hint whose line is evicted harmlessly if
    // nothing reads it; the bitmap read needed to avoid issuing one is not. The lookup below is
    // already behind the bit test, which is the half that pays.
    //
    // What is left on the table is the hashing, not the prefetching: KeyHashes runs over the whole
    // array, so on that query it hashes 600,037,902 rows of which 148,067,261 are still selected --
    // about 0.81 s summed over parts, 3% of the query. Taking it needs the selected values
    // compacted before hashing and a threshold for when that is cheaper than hashing everything,
    // which is its own change with its own measurement.
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

// What `p` adds to its column's cost. A predicate on a fixed-width column adds nothing a late
// position would avoid: its kernel runs over the whole array wherever the column is placed.
std::int64_t CostOf(const ColumnPredicates& c, const plan::Predicate& p) {
  if (!c.strings) {
    return 0;
  }
  switch (p.kind) {
    case Kind::kLike:
    case Kind::kNotLike:
      return kStringLikeCost;
    case Kind::kIn:
    case Kind::kNotIn:
      // One length test and memcmp per value of the list. Linear is right over the range where
      // this term can decide anything: a list of ten values or more is assumed below to keep every
      // row, which sends the column last on selectivity alone whatever its cost, and under ten is
      // also where StringPredicate walks the values instead of searching them.
      return kStringCompareCost * std::max<std::int64_t>(1, Narrow<std::int64_t>(p.values.size()));
    default:
      return kStringCompareCost;
  }
}

// The fraction of rows `p` is assumed to keep, out of kAllSurvive.
std::int64_t SurvivesOf(const plan::Predicate& p) {
  switch (p.kind) {
    case Kind::kCompare:
      switch (p.op) {
        case plan::CompareOp::kEq:
          return kEqualSurvives;
        case plan::CompareOp::kNe:
          return kAllSurvive - kEqualSurvives;
        default:
          return kRangeSurvives;
      }
    case Kind::kIn:
      return std::min(kAllSurvive, kEqualSurvives * Narrow<std::int64_t>(p.values.size()));
    case Kind::kNotIn:
      return kAllSurvive -
             std::min(kAllSurvive, kEqualSurvives * Narrow<std::int64_t>(p.values.size()));
    case Kind::kLike:
      return kLikeSurvives;
    case Kind::kNotLike:
      return kAllSurvive - kLikeSurvives;
    default:  // kIsNotNull and anything else: assume it keeps everything
      return kAllSurvive;
  }
}

// Orders the columns the scan applies, cheapest work per row removed first.
//
// The scan reads one column, narrows the selection, then reads the next, so a column placed early
// pays its cost over every row while the columns after it pay theirs only over the rows it left.
// The order that minimises the total is by ascending `cost / (1 - survives)` -- Hellerstein and
// Stonebraker's rank ordering for a conjunction, and the choke point Dreseler et al. call predicate
// positioning (PVLDB 13(8); ADR 0020, "ordering the columns a scan filters"). Compared as a cross
// product, so no division and no floating point enters a planning decision, and stable, so columns
// of equal rank keep the order the query wrote them in: std::sort would leave their order to the
// standard library and so to the platform, and an EXPLAIN must not depend on that.
//
// A column that removes nothing needs no case of its own: its rank is a division by zero, and the
// cross product sends it last on its own, because every cost is positive.
//
// That a cost is positive is also what makes this comparator a strict weak ordering, which
// stable_sort requires and does not check: with it, the comparison is exactly `cost / removed`
// compared over the rationals extended by the infinity at removed == 0, and a comparison of keys
// drawn from an ordered set is a strict weak ordering by construction. A cost of zero would break
// it -- a (0, 0) column would compare equivalent both to every other zero-cost column and to a
// column of positive cost that those compare strictly before, and an intransitive equivalence in
// stable_sort is undefined behaviour. Hence the assertion below rather than a remark here: a cost
// is a sum of the weights above, so those being positive is the whole condition.
//
// No order changes an answer, so this is worth only time -- and it is worth 2.28x of it: the same
// filter, two bounds on a DATE column and a two-value IN on a VARCHAR one over 600 million rows at
// four threads, read 1.102 s written dates-first and 2.507 s written the other way round, because
// the order was the order of the WHERE clause. Both spellings now read 1.103 s.
//
// The products cannot overflow: a column's cost would have to exceed 9.2e16 for `cost * removed` to
// leave int64, and a cost is bounded by the length of the query's IN lists.
static_assert(kFixedWidthCost > 0 && kStringCompareCost > 0 && kStringLikeCost > 0);

void Order(std::vector<int>& columns, std::vector<ColumnPredicates>& per_column) {
  std::vector<std::size_t> by_rank(columns.size());
  std::ranges::iota(by_rank, std::size_t{0});
  std::ranges::stable_sort(by_rank, [&](std::size_t a, std::size_t b) {
    const std::int64_t removed_a = kAllSurvive - per_column[a].survives;
    const std::int64_t removed_b = kAllSurvive - per_column[b].survives;
    return per_column[a].cost * removed_b < per_column[b].cost * removed_a;
  });
  std::vector<int> ordered_columns;
  std::vector<ColumnPredicates> ordered;
  ordered_columns.reserve(columns.size());
  ordered.reserve(per_column.size());
  for (const std::size_t at : by_rank) {
    ordered_columns.push_back(columns[at]);
    ordered.push_back(std::move(per_column[at]));
  }
  columns = std::move(ordered_columns);
  per_column = std::move(ordered);
}

// Both filters over one mask (MakeBothScanFilters).
//
// Its column list is the **union** of theirs and not the concatenation, because a ScanFilter names
// each column it reads once (plan::ScanFilter, enforced by io's filtered scan) and the two may well
// read the same one -- a predicate on a column that is also the join's key is the ordinary case,
// and concatenating made every such scan fail. So each column of the union carries the position it
// has in each child's own list, or none, and Apply drives whichever of the two reads it: both for a
// shared column, which is sound because each only clears bits of the mask it is handed.
class BothScanFilters : public plan::ScanFilter {
 public:
  BothScanFilters(std::shared_ptr<const plan::ScanFilter> first,
                  std::shared_ptr<const plan::ScanFilter> second)
      : first_(std::move(first)), second_(std::move(second)) {
    const auto place = [this](const std::vector<int>& theirs, bool is_first) {
      for (std::size_t i = 0; i < theirs.size(); ++i) {
        const auto at = std::ranges::find(columns_, theirs[i]);
        if (at == columns_.end()) {
          columns_.push_back(theirs[i]);
          in_first_.push_back(-1);
          in_second_.push_back(-1);
        }
        const auto k =
            Narrow<std::size_t>(std::ranges::find(columns_, theirs[i]) - columns_.begin());
        (is_first ? in_first_ : in_second_)[k] = Narrow<int>(i);
      }
    };
    place(first_->columns(), true);
    place(second_->columns(), false);
  }

  [[nodiscard]] const std::vector<int>& columns() const override { return columns_; }

  arrow::Status Apply(int column, const plan::ScanValues& values, int64_t offset,
                      std::uint8_t* selected) const override {
    if (column < 0 || std::cmp_greater_equal(column, columns_.size())) {
      return arrow::Status::Invalid("a conjunction of scan filters has no column ", column);
    }
    const auto k = Narrow<std::size_t>(column);
    // Each index is a position in that child's own column list, by construction. Both bounds are
    // checked rather than asserted so that the position a child is handed is provably one of its
    // own -- which is what its own Apply documents taking, and what a reader of either side can
    // rely on without following the constructor.
    ARROW_RETURN_NOT_OK(Dispatch(*first_, in_first_[k], values, offset, selected));
    return Dispatch(*second_, in_second_[k], values, offset, selected);
  }

 private:
  // `filter` applied for its own column `at`, or nothing where `at` is -1: this filter's column is
  // not one that filter reads.
  static arrow::Status Dispatch(const plan::ScanFilter& filter, int at,
                                const plan::ScanValues& values, int64_t offset,
                                std::uint8_t* selected) {
    if (at < 0) {
      return arrow::Status::OK();
    }
    if (std::cmp_greater_equal(at, filter.columns().size())) {
      return arrow::Status::Invalid("a conjunction of scan filters names column ", at,
                                    " of a filter that reads ", filter.columns().size());
    }
    return filter.Apply(at, values, offset, selected);
  }

  std::shared_ptr<const plan::ScanFilter> first_;
  std::shared_ptr<const plan::ScanFilter> second_;
  std::vector<int> columns_;    // the union, each position once
  std::vector<int> in_first_;   // where each is in first_->columns(), or -1
  std::vector<int> in_second_;  // and in second_->columns()
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
    arrow::MemoryPool* pool, std::shared_ptr<std::atomic<int64_t>> dictionary_pieces) {
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
      c.cost = c.strings ? 0 : kFixedWidthCost;
      c.schema = arrow::schema({schema.field(index)});
      per_column.push_back(std::move(c));
    }
    ColumnPredicates& c = per_column[position];
    c.cost += CostOf(c, p);
    c.survives = (c.survives * SurvivesOf(p)) / kAllSurvive;
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
  Order(columns, per_column);
  return std::make_shared<const PredicateScanFilter>(std::move(columns), std::move(per_column),
                                                     pool, std::move(dictionary_pieces));
}

arrow::Result<std::shared_ptr<const plan::ScanFilter>> MakeBothScanFilters(
    std::shared_ptr<const plan::ScanFilter> first, std::shared_ptr<const plan::ScanFilter> second) {
  if (first == nullptr || second == nullptr) {
    return arrow::Status::Invalid("a conjunction of scan filters without both of them");
  }
  return std::make_shared<const BothScanFilters>(std::move(first), std::move(second));
}

}  // namespace antb1::exec
