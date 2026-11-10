#include "relations.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include "antb1/common/int128.h"

#include "canonical.h"
#include "engine.h"
#include "supported_features.h"

namespace antb1::metamorphic {
namespace {

using slt::ColumnClass;
using slt::ResultSet;

struct Value {
  ColumnClass cls = ColumnClass::kInteger;
  std::optional<std::string> text;  // canonical text; std::nullopt is NULL
};

std::expected<Value, std::string> SingleValue(std::span<const ResultSet> answers, std::size_t i) {
  const ResultSet& r = answers[i];
  if (r.classes.size() != 1 || r.rows.size() != 1 || r.rows[0].size() != 1) {
    return std::unexpected(std::format("answer {} is not a single value ({} rows, {} columns)", i,
                                       r.rows.size(), r.classes.size()));
  }
  return Value{.cls = r.classes[0], .text = r.rows[0][0]};
}

std::optional<Int128> ParseInt128(std::string_view text) {
  const bool negative = text.starts_with('-');
  const std::string_view digits = negative ? text.substr(1) : text;
  if (digits.empty()) {
    return std::nullopt;
  }
  Int128 value = 0;
  for (const char c : digits) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    Int128 next = 0;
    if (__builtin_mul_overflow(value, 10, &next) ||
        __builtin_sub_overflow(next, static_cast<Int128>(c - '0'), &value)) {
      return std::nullopt;  // accumulate negatively: kInt128Min parses too
    }
  }
  if (!negative) {
    if (value == kInt128Min) {
      return std::nullopt;
    }
    value = -value;
  }
  return value;
}

// A DECIMAL's canonical text ("-0.25", ".500", "17": plan::FormatDecimal) as its unscaled value and
// its number of fraction digits, the scale; std::nullopt for any other text.
std::optional<std::pair<Int128, std::size_t>> ParseDecimal(std::string_view text) {
  const std::size_t point = text.find('.');
  if (point == std::string_view::npos) {
    const auto value = ParseInt128(text);
    return value.has_value() ? std::optional(std::pair(*value, std::size_t{0})) : std::nullopt;
  }
  const std::string_view fraction = text.substr(point + 1);
  const auto value = ParseInt128(std::string(text.substr(0, point)) + std::string(fraction));
  if (!value.has_value() || fraction.empty()) {
    return std::nullopt;
  }
  return std::pair(*value, fraction.size());
}

template <class T>
int ThreeWay(const T& a, const T& b) {
  if (a < b) {
    return -1;
  }
  return a > b ? 1 : 0;
}

std::optional<double> ParseDouble(std::string_view text) {
  double value = 0;
  const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  return ec == std::errc{} && ptr == text.data() + text.size() ? std::optional(value)
                                                               : std::nullopt;
}

// <0, 0, >0 like strcmp; std::nullopt if the values do not compare (different classes, bad text).
std::optional<int> CompareValues(const Value& a, const Value& b) {
  if (a.cls != b.cls || !a.text.has_value() || !b.text.has_value()) {
    return std::nullopt;
  }
  switch (a.cls) {
    case ColumnClass::kInteger: {
      const auto x = ParseInt128(*a.text);
      const auto y = ParseInt128(*b.text);
      if (!x.has_value() || !y.has_value()) {
        return std::nullopt;
      }
      return ThreeWay(*x, *y);
    }
    case ColumnClass::kReal: {
      const auto x = ParseDouble(*a.text);
      const auto y = ParseDouble(*b.text);
      if (!x.has_value() || !y.has_value()) {
        return std::nullopt;
      }
      return ThreeWay(*x, *y);
    }
    case ColumnClass::kText:  // bytes (char_traits<char> compares as unsigned char); dates sort too
      return a.text->compare(*b.text);
    case ColumnClass::kDecimal: {  // by value within one type: the unscaled values at one scale
      const auto x = ParseDecimal(*a.text);
      const auto y = ParseDecimal(*b.text);
      if (!x.has_value() || !y.has_value() || x->second != y->second) {
        return std::nullopt;
      }
      return ThreeWay(x->first, y->first);
    }
  }
  return std::nullopt;
}

std::string Show(const std::optional<std::string>& text) { return text.value_or("NULL"); }

Check Extreme(bool want_min) {
  return [want_min](std::span<const ResultSet> answers) -> std::optional<std::string> {
    auto first = SingleValue(answers, 0);
    if (!first) {
      return first.error();
    }
    std::optional<Value> best;
    for (std::size_t i = 1; i < answers.size(); ++i) {
      auto v = SingleValue(answers, i);
      if (!v) {
        return v.error();
      }
      if (!v->text.has_value()) {
        continue;
      }
      if (!best.has_value()) {
        best = *v;
        continue;
      }
      const auto order = CompareValues(*v, *best);
      if (!order.has_value()) {
        return std::format("answer {} ({}) does not compare with {}", i, Show(v->text),
                           Show(best->text));
      }
      if (want_min ? *order < 0 : *order > 0) {
        best = *v;
      }
    }
    const std::optional<std::string> expected =
        best.has_value() ? best->text : std::optional<std::string>();
    if (first->text != expected) {
      return std::format("answer 0 is {}, but the {} of the others is {}", Show(first->text),
                         want_min ? "minimum" : "maximum", Show(expected));
    }
    return std::nullopt;
  };
}

std::expected<int64_t, std::string> FirstCount(std::span<const ResultSet> answers) {
  auto first = SingleValue(answers, 0);
  if (!first) {
    return std::unexpected(first.error());
  }
  const auto n = first->text.has_value() ? ParseInt128(*first->text) : std::nullopt;
  if (!n.has_value() || !Int128ToInt64(*n).has_value()) {
    return std::unexpected(std::format("answer 0 ({}) is not a row count", Show(first->text)));
  }
  return Int128ToInt64(*n).value_or(0);
}

Probe Q(std::string sql, int64_t batch_size = kDefaultBatchSize) {
  return Probe{.sql = std::move(sql), .batch_size = batch_size};
}

// `base` plus `features`: the relations below share a base set and each adds a few to it.
template <typename... Features>
slt::FeatureSet SetWith(slt::FeatureSet base, Features... features) {
  (base.Add(features), ...);
  return base;
}

std::string PathOf(std::string_view table) {
  for (const auto& t : TablePaths()) {
    if (t.table == table) {
      return std::format("'${{FIXTURES}}/{}'", t.path);
    }
  }
  return "'${FIXTURES}/no_such_table'";
}

constexpr auto kTablePaths = std::to_array<TablePath>({
    {.table = "hits_like", .path = "hits_like.parquet"},
    {.table = "hits_like_nulls", .path = "hits_like_nulls.parquet"},
    {.table = "hits_like_split", .path = "hits_like_split/part-*.parquet"},
    {.table = "hits_like_required", .path = "hits_like_required.parquet"},
    {.table = "edge", .path = "edge.parquet"},
    {.table = "floats", .path = "floats.parquet"},
    {.table = "decimals", .path = "decimals.parquet"},
    {.table = "empty", .path = "empty.parquet"},
    {.table = "trips", .path = "star/trips.parquet"},
    {.table = "riders", .path = "star/riders.parquet"},
    {.table = "drivers", .path = "star/drivers.parquet"},
    {.table = "zones", .path = "star/zones.parquet"},
    {.table = "cities", .path = "star/cities.parquet"},
    {.table = "shifts", .path = "star/shifts.parquet"},
    {.table = "tariffs", .path = "star/tariffs.parquet"},
    {.table = "promos", .path = "star/promos.parquet"},
    {.table = "bookings", .path = "star/bookings.parquet"},
    {.table = "fleet", .path = "star/fleet.parquet"},
    {.table = "models", .path = "star/models.parquet"},
    {.table = "days", .path = "star/days.parquet"},
});

constexpr std::array<int64_t, 3> kBatchSizes = {1, 7, kDefaultBatchSize};

// Columns of hits_like_nulls with a literal that splits their values (TLP-lite).
struct Split {
  std::string_view column;
  std::string_view literal;
  slt::Feature type;
  slt::Feature literal_kind;
};

constexpr auto kSplits = std::to_array<Split>({
    {.column = "RegionID",
     .literal = "10000",
     .type = slt::Feature::kIntegerColumns,
     .literal_kind = slt::Feature::kIntegerLiteral},
    {.column = "UserID",
     .literal = "2305843009213693952",
     .type = slt::Feature::kIntegerColumns,
     .literal_kind = slt::Feature::kIntegerLiteral},
    {.column = "ResolutionWidth",
     .literal = "1366.5",
     .type = slt::Feature::kIntegerColumns,
     .literal_kind = slt::Feature::kDecimalLiteral},
    {.column = "Title",
     .literal = "'maple'",
     .type = slt::Feature::kVarcharColumns,
     .literal_kind = slt::Feature::kStringLiteral},
    {.column = "EventDate",
     .literal = "DATE '2013-07-15'",
     .type = slt::Feature::kDateColumns,
     .literal_kind = slt::Feature::kDateLiteral},
});

Check EqualBlocks(slt::SortMode sort) {
  return [sort](std::span<const ResultSet> answers) -> std::optional<std::string> {
    const auto render = [sort](const ResultSet& r) { return slt::RenderBlock(r, sort, 0); };
    std::string letters;
    for (const auto c : answers[0].classes) {
      letters += slt::ClassLetter(c);
    }
    const auto first = render(answers[0]);
    for (std::size_t i = 1; i < answers.size(); ++i) {
      std::string other;
      for (const auto c : answers[i].classes) {
        other += slt::ClassLetter(c);
      }
      if (other != letters) {
        return std::format("answer {} has column types {}, answer 0 has {}", i, other, letters);
      }
      const auto block = render(answers[i]);
      if (auto diff = slt::CompareBlocks(first, block, letters, sort, slt::kDefaultRelTolerance)) {
        const std::size_t row = diff->first_row.value_or(0);
        return std::format("answer {} differs from answer 0: {} (row {}: {} vs {})", i,
                           diff->reason, row, row < first.size() ? first[row] : "(none)",
                           row < block.size() ? block[row] : "(none)");
      }
    }
    return std::nullopt;
  };
}

}  // namespace

Check AllEqual() { return EqualBlocks(slt::SortMode::kRowSort); }

Check AllEqualInOrder() { return EqualBlocks(slt::SortMode::kNoSort); }

Check FirstIsUnionOfRest() {
  return [](std::span<const ResultSet> answers) -> std::optional<std::string> {
    if (answers.empty()) {
      return "no answers";
    }
    ResultSet rest = answers[0];
    rest.rows.clear();
    for (std::size_t i = 1; i < answers.size(); ++i) {
      if (answers[i].classes != answers[0].classes) {
        return std::format("answer {} has other column types than answer 0", i);
      }
      rest.rows.insert(rest.rows.end(), answers[i].rows.begin(), answers[i].rows.end());
    }
    const std::array<ResultSet, 2> pair = {answers[0], rest};
    if (auto diff = AllEqual()(pair)) {
      return std::format("answer 0 vs the rows of the others: {}", *diff);
    }
    return std::nullopt;
  };
}

Check SecondIsWindowOfFirst(int64_t offset, int64_t limit) {
  return [offset, limit](std::span<const ResultSet> answers) -> std::optional<std::string> {
    if (answers.size() != 2) {
      return std::format("{} answers, expected 2", answers.size());
    }
    ResultSet window = answers[0];
    const auto begin = std::min(window.rows.size(), static_cast<std::size_t>(offset));
    const auto end = std::min(window.rows.size(), begin + static_cast<std::size_t>(limit));
    window.rows = {window.rows.begin() + static_cast<std::ptrdiff_t>(begin),
                   window.rows.begin() + static_cast<std::ptrdiff_t>(end)};
    const std::array<ResultSet, 2> pair = {window, answers[1]};
    if (auto diff = AllEqualInOrder()(pair)) {
      return std::format("rows [{}, {}) of answer 0 vs answer 1: {}", begin, end, *diff);
    }
    return std::nullopt;
  };
}

Check FirstEqualsSumOfRest() {
  return [](std::span<const ResultSet> answers) -> std::optional<std::string> {
    auto first = SingleValue(answers, 0);
    if (!first) {
      return first.error();
    }
    Int128 sum = 0;
    bool any = false;
    std::string terms;
    for (std::size_t i = 1; i < answers.size(); ++i) {
      auto v = SingleValue(answers, i);
      if (!v) {
        return v.error();
      }
      terms += (i > 1 ? " + " : "") + Show(v->text);
      if (!v->text.has_value()) {
        continue;
      }
      const auto x = ParseInt128(*v->text);
      const auto next = x.has_value() ? CheckedAdd(sum, *x) : std::nullopt;
      if (v->cls != ColumnClass::kInteger || !next.has_value()) {
        return std::format("answer {} ({}) is not an integer that can be summed", i, Show(v->text));
      }
      sum = *next;
      any = true;
    }
    const std::optional<std::string> expected =
        any ? std::optional(Int128ToString(sum)) : std::nullopt;
    if (first->text != expected) {
      return std::format("answer 0 is {}, but {} = {}", Show(first->text), terms, Show(expected));
    }
    return std::nullopt;
  };
}

Check FirstEqualsMinOfRest() { return Extreme(/*want_min=*/true); }

Check FirstEqualsMaxOfRest() { return Extreme(/*want_min=*/false); }

Check RowCountsEqualFirst() {
  return [](std::span<const ResultSet> answers) -> std::optional<std::string> {
    auto n = FirstCount(answers);
    if (!n) {
      return n.error();
    }
    for (std::size_t i = 1; i < answers.size(); ++i) {
      if (std::cmp_not_equal(answers[i].rows.size(), *n)) {
        return std::format("answer {} has {} rows, answer 0 counts {}", i, answers[i].rows.size(),
                           *n);
      }
    }
    return std::nullopt;
  };
}

Check RowCountsAreMinOf(std::vector<int64_t> limits) {
  return [expected_limits =
              std::move(limits)](std::span<const ResultSet> answers) -> std::optional<std::string> {
    auto n = FirstCount(answers);
    if (!n) {
      return n.error();
    }
    if (answers.size() != expected_limits.size() + 1) {
      return std::format("{} answers for {} limits", answers.size(), expected_limits.size());
    }
    for (std::size_t i = 1; i < answers.size(); ++i) {
      const int64_t limit = expected_limits[i - 1];
      const int64_t expected = std::min(limit, *n);
      if (std::cmp_not_equal(answers[i].rows.size(), expected)) {
        return std::format("answer {} (LIMIT {}) has {} rows, expected min({}, {}) = {}", i, limit,
                           answers[i].rows.size(), limit, *n, expected);
      }
    }
    return std::nullopt;
  };
}

Verdict Evaluate(const Relation& r, const std::vector<slt::ExecResult>& answers,
                 slt::FeatureSet supported) {
  using Kind = Verdict::Kind;
  if (answers.size() != r.probes.size()) {
    std::string message = std::format("{} answers for {} queries", answers.size(), r.probes.size());
    return {.kind = Kind::kBroken, .message = message, .redacted = message};
  }
  const slt::FeatureSet missing = r.features.Minus(supported);
  int unsupported = 0;
  std::vector<slt::ResultSet> results;
  for (std::size_t i = 0; i < answers.size(); ++i) {
    if (answers[i].has_value()) {
      results.push_back(*answers[i]);
      continue;
    }
    const slt::EngineError& e = answers[i].error();
    if (!missing.empty() && e.unsupported) {
      ++unsupported;
      continue;
    }
    return {.kind = Kind::kBroken,
            .message = std::format("query {} fails: {}", i, e.message),
            .redacted = std::format("query {} fails ({} error)", i, e.kind)};
  }
  if (!missing.empty()) {
    if (unsupported == 0) {
      std::string message = std::format(
          "antb1 answers every query of this pending relation, but it uses features that "
          "tests/slt/supported_features.h does not declare: {}. Add them to "
          "kSupportedFeatures to activate the relation.",
          missing.Names());
      return {.kind = Kind::kBroken, .message = message, .redacted = message};
    }
    std::string message = std::format("pending: needs {} ({} of {} queries answered Unsupported)",
                                      missing.Names(), unsupported, answers.size());
    return {.kind = Kind::kPending, .message = message, .redacted = message};
  }
  if (auto violation = r.check(results)) {
    return {.kind = Kind::kViolated,
            .message = *std::move(violation),
            .redacted = "the answers violate the relation (values not shown)"};
  }
  return {.kind = Kind::kHolds, .message = {}, .redacted = {}};
}

std::span<const TablePath> TablePaths() { return kTablePaths; }

void PrintTo(const Relation& r, std::ostream* os) { *os << r.name; }

std::vector<Relation> AllRelations() {
  using enum slt::Feature;
  std::vector<Relation> r;
  const slt::FeatureSet count_by_name = {kCountStar, kTableName};
  const slt::FeatureSet count_by_path = {kCountStar, kTableName, kTablePath};

  // ---- active: COUNT(*) (answered from the Parquet footers) ----
  r.push_back({.name = "split_parts_sum_to_table",
               .features = count_by_path,
               .probes = {Q("SELECT COUNT(*) FROM hits_like_split"),
                          Q("SELECT COUNT(*) FROM '${FIXTURES}/hits_like_split/part-0.parquet'"),
                          Q("SELECT COUNT(*) FROM '${FIXTURES}/hits_like_split/part-1.parquet'"),
                          Q("SELECT COUNT(*) FROM '${FIXTURES}/hits_like_split/part-2.parquet'"),
                          Q("SELECT COUNT(*) FROM '${FIXTURES}/hits_like_split/part-3.parquet'")},
               .check = FirstEqualsSumOfRest()});
  r.push_back(
      {.name = "split_equals_single_file",
       .features = count_by_name,
       .probes = {Q("SELECT COUNT(*) FROM hits_like_split"), Q("SELECT COUNT(*) FROM hits_like")},
       .check = AllEqual()});
  r.push_back(
      {.name = "split_glob_forms_equal",
       .features = count_by_path,
       .probes = {Q("SELECT COUNT(*) FROM hits_like_split"),
                  Q("SELECT COUNT(*) FROM '${FIXTURES}/hits_like_split/part-*.parquet'"),
                  Q("SELECT COUNT(*) FROM '${FIXTURES}/hits_like_split/part-?.parquet'"),
                  Q("SELECT COUNT(*) FROM '${FIXTURES}/hits_like_split/part-[0-3].parquet'"),
                  Q("SELECT COUNT(*) FROM '${FIXTURES}/hits_like_split/*.parquet'")},
       .check = AllEqual()});
  r.push_back(
      {.name = "variants_count_equal",
       .features = count_by_name,
       .probes = {Q("SELECT COUNT(*) FROM hits_like"), Q("SELECT COUNT(*) FROM hits_like_nulls"),
                  Q("SELECT COUNT(*) FROM hits_like_required")},
       .check = AllEqual()});
  for (const auto& t : TablePaths()) {
    r.push_back({.name = std::format("path_equals_table_{}", t.table),
                 .features = count_by_path,
                 .probes = {Q(std::format("SELECT COUNT(*) FROM {}", t.table)),
                            Q(std::format("SELECT COUNT(*) FROM {}", PathOf(t.table)))},
                 .check = AllEqual()});
  }
  for (const std::string_view table : {"hits_like", "hits_like_split", "edge", "empty"}) {
    Relation rel{.name = std::format("batch_size_invariance_count_{}", table),
                 .features = count_by_name,
                 .probes = {},
                 .check = AllEqual()};
    for (const int64_t batch : kBatchSizes) {
      rel.probes.push_back(Q(std::format("SELECT COUNT(*) FROM {}", table), batch));
    }
    r.push_back(std::move(rel));
  }
  r.push_back({.name = "lexical_forms_equal",
               .features = {kCountStar, kTableName, kKeywordCase, kIdentifierCase,
                            kQuotedIdentifier, kLayout, kSemicolon},
               .probes = {Q("SELECT COUNT(*) FROM hits_like"), Q("select count(*) from HITS_LIKE"),
                          Q("SELECT COUNT(*) FROM \"Hits_Like\";"),
                          Q("SELECT\n\tCOUNT ( * ) -- rows\nFROM /* the table */ hits_like ;")},
               .check = AllEqual()});

  // ---- the executor: scans, filters, projections, aggregates, LIMIT ----
  // GROUP BY: the same groups and aggregates whatever the batch size and file layout.
  {
    const slt::FeatureSet grouped = {kGroupBy,
                                     kCountStar,
                                     kSum,
                                     kMin,
                                     kMax,
                                     kColumns,
                                     kMultipleItems,
                                     kIntegerColumns,
                                     kVarcharColumns,
                                     kWhere,
                                     kIntegerLiteral,
                                     kTableName};
    constexpr std::string_view kGrouped =
        "SELECT OS, IsMobile, COUNT(*), SUM(ResolutionWidth), MIN(URL), MAX(EventTime) FROM {} "
        "WHERE OS < 30 GROUP BY OS, IsMobile";
    Relation sizes{.name = "grouped_batch_size_invariance",
                   .features = grouped,
                   .probes = {},
                   .check = AllEqual()};
    for (const int64_t batch : kBatchSizes) {
      sizes.probes.push_back(Q(std::format(kGrouped, "hits_like_split"), batch));
    }
    r.push_back(std::move(sizes));
    r.push_back({.name = "grouped_split_equals_single_file",
                 .features = grouped,
                 .probes = {Q(std::format(kGrouped, "hits_like_split")),
                            Q(std::format(kGrouped, "hits_like"))},
                 .check = AllEqual()});
  }
  // SELECT DISTINCT (ADR 0037's AG3): the distinct rows are the groups of the same list, the
  // distinct values of a NOT NULL column count as COUNT(DISTINCT), whatever the batch size.
  {
    const slt::FeatureSet distinct = {kSelectDistinct, kGroupBy,        kColumns,
                                      kMultipleItems,  kIntegerColumns, kVarcharColumns,
                                      kCountStar,      kCountDistinct,  kDerivedTable,
                                      kWhere,          kIntegerLiteral, kTableName};
    r.push_back({.name = "distinct_is_group_by",
                 .features = distinct,
                 .probes = {Q("SELECT DISTINCT OS, IsMobile FROM hits_like WHERE OS < 30"),
                            Q("SELECT OS, IsMobile FROM hits_like WHERE OS < 30 GROUP BY OS, "
                              "IsMobile")},
                 .check = AllEqual()});
    r.push_back({.name = "distinct_count_is_count_distinct",
                 .features = distinct,
                 .probes = {Q("SELECT COUNT(*) FROM (SELECT DISTINCT OS FROM hits_like) AS d"),
                            Q("SELECT COUNT(DISTINCT OS) FROM hits_like")},
                 .check = AllEqual()});
    Relation sizes{.name = "distinct_batch_size_invariance",
                   .features = distinct,
                   .probes = {},
                   .check = AllEqual()};
    for (const int64_t batch : kBatchSizes) {
      sizes.probes.push_back(
          Q("SELECT DISTINCT OS, URL FROM hits_like_split WHERE OS < 30", batch));
    }
    r.push_back(std::move(sizes));
  }
  // GROUP BY ROLLUP (ADR 0037's RU3): its rows are those of each set's own GROUP BY, a key the set
  // lacks NULL, the empty set's row the ungrouped aggregate's; the same whatever the batch size and
  // over the split file. The calls merge exactly, so the two-level operator answers them.
  {
    const slt::FeatureSet rollup = {
        kRollup, kGroupBy, kColumns, kMultipleItems, kIntegerColumns, kVarcharColumns, kCountStar,
        kSum,    kMin,     kCase,    kWhere,         kIntegerLiteral, kTableName};
    constexpr std::string_view kCalls = "COUNT(*), SUM(ResolutionWidth), MIN(URL)";
    // NULL in every row (no OS is negative), as a key the set lacks: a NULL item is not supported.
    constexpr std::string_view kNull = "CASE WHEN OS < 0 THEN OS END";
    r.push_back(
        {.name = "rollup_is_the_union_of_its_sets",
         .features = rollup,
         .probes = {Q(std::format("SELECT OS, IsMobile, {} FROM hits_like WHERE OS < 30 GROUP BY "
                                  "ROLLUP (OS, IsMobile)",
                                  kCalls)),
                    Q(std::format("SELECT OS, IsMobile, {} FROM hits_like WHERE OS < 30 GROUP BY "
                                  "OS, IsMobile",
                                  kCalls)),
                    Q(std::format("SELECT OS, {}, {} FROM hits_like WHERE OS < 30 GROUP BY OS",
                                  kNull, kCalls)),
                    Q(std::format("SELECT MIN({}), MIN({}), {} FROM hits_like WHERE OS < 30", kNull,
                                  kNull, kCalls))},
         .check = FirstIsUnionOfRest()});
    r.push_back(
        {.name = "rollup_of_no_rows_is_the_ungrouped_aggregate",
         .features = rollup,
         .probes = {Q(std::format("SELECT OS, {} FROM hits_like WHERE OS < 0 GROUP BY ROLLUP (OS)",
                                  kCalls)),
                    Q(std::format("SELECT MIN(OS), {} FROM hits_like WHERE OS < 0", kCalls))},
         .check = AllEqual()});
    constexpr std::string_view kRolled =
        "SELECT OS, IsMobile, COUNT(*), SUM(ResolutionWidth), MIN(URL) FROM {} WHERE OS < 30 "
        "GROUP BY ROLLUP (OS, IsMobile)";
    Relation sizes{.name = "rollup_batch_size_and_split_invariance",
                   .features = rollup,
                   .probes = {Q(std::format(kRolled, "hits_like"))},
                   .check = AllEqual()};
    for (const int64_t batch : kBatchSizes) {
      sizes.probes.push_back(Q(std::format(kRolled, "hits_like_split"), batch));
    }
    r.push_back(std::move(sizes));
  }
  // GROUPING() (ADR 0037's RU4): HAVING on its value picks one set's rows, which are that set's own
  // GROUP BY; over a plain GROUP BY it is 0 in every row.
  {
    const slt::FeatureSet grouping = {
        kRollup,         kGroupingFunction, kGroupBy,   kColumns,     kMultipleItems,
        kIntegerColumns, kCountStar,        kSum,       kCase,        kHaving,
        kWhere,          kIntegerLiteral,   kTableName, kDerivedTable};
    constexpr std::string_view kNull = "CASE WHEN OS < 0 THEN OS END";  // no OS is negative
    r.push_back(
        {.name = "grouping_picks_a_set",
         .features = grouping,
         .probes =
             {Q("SELECT OS, IsMobile, COUNT(*), SUM(ResolutionWidth) FROM hits_like WHERE OS < 30 "
                "GROUP BY ROLLUP (OS, IsMobile) HAVING GROUPING(OS, IsMobile) = 1"),
              Q(std::format("SELECT OS, {}, COUNT(*), SUM(ResolutionWidth) FROM hits_like WHERE "
                            "OS < 30 GROUP BY OS",
                            kNull))},
         .check = AllEqual()});
    r.push_back({.name = "grouping_of_a_plain_group_by_is_zero",
                 .features = grouping,
                 .probes = {Q("SELECT COUNT(*) FROM (SELECT OS FROM hits_like GROUP BY OS) AS d"),
                            Q("SELECT COUNT(*) FROM (SELECT OS FROM hits_like GROUP BY OS HAVING "
                              "GROUPING(OS) = 0) AS d")},
                 .check = AllEqual()});
  }
  // GROUPING SETS, composites and () (ADR 0037's RU5): the sets of a ROLLUP written out are the
  // ROLLUP, a list of sets is the union of their GROUP BYs, and () is the ungrouped aggregate, with
  // its row over no input too.
  {
    const slt::FeatureSet sets = {kGroupingSets,  kRollup,         kGroupBy,        kColumns,
                                  kMultipleItems, kIntegerColumns, kCountStar,      kSum,
                                  kCase,          kWhere,          kIntegerLiteral, kTableName};
    constexpr std::string_view kNull = "CASE WHEN OS < 0 THEN OS END";  // no OS is negative
    r.push_back(
        {.name = "grouping_sets_of_a_rollup_are_the_rollup",
         .features = sets,
         .probes =
             {Q("SELECT OS, IsMobile, COUNT(*), SUM(ResolutionWidth) FROM hits_like WHERE OS < 30 "
                "GROUP BY GROUPING SETS ((OS, IsMobile), (OS), ())"),
              Q("SELECT OS, IsMobile, COUNT(*), SUM(ResolutionWidth) FROM hits_like WHERE OS < 30 "
                "GROUP BY ROLLUP (OS, IsMobile)")},
         .check = AllEqual()});
    r.push_back(
        {.name = "grouping_sets_are_the_union_of_their_group_bys",
         .features = sets,
         .probes =
             {Q("SELECT OS, IsMobile, COUNT(*) FROM hits_like WHERE OS < 30 GROUP BY GROUPING "
                "SETS ((OS), (IsMobile))"),
              Q(std::format("SELECT OS, {}, COUNT(*) FROM hits_like WHERE OS < 30 GROUP BY OS",
                            kNull)),
              Q(std::format("SELECT {}, IsMobile, COUNT(*) FROM hits_like WHERE OS < 30 GROUP BY "
                            "IsMobile",
                            "CASE WHEN IsMobile < 0 THEN IsMobile END"))},
         .check = FirstIsUnionOfRest()});
    for (const std::string_view where : {"OS < 30", "OS < 0"}) {
      r.push_back(
          {.name = std::format("empty_set_is_the_ungrouped_aggregate_{}",
                               where == "OS < 0" ? "of_no_rows" : "of_rows"),
           .features = sets,
           .probes = {Q(std::format("SELECT COUNT(*), SUM(ResolutionWidth) FROM hits_like WHERE {} "
                                    "GROUP BY ()",
                                    where)),
                      Q(std::format("SELECT COUNT(*), SUM(ResolutionWidth) FROM hits_like WHERE {}",
                                    where))},
           .check = AllEqual()});
    }
  }
  // CUBE (ADR 0037's RU6): its sets written out as GROUPING SETS, every subset of its keys, and
  // CUBE of one key is that key's ROLLUP.
  {
    const slt::FeatureSet cube = {kCube,    kGroupingSets,  kRollup,         kGroupBy,
                                  kColumns, kMultipleItems, kIntegerColumns, kCountStar,
                                  kSum,     kWhere,         kIntegerLiteral, kTableName};
    r.push_back(
        {.name = "cube_is_every_subset_of_its_keys",
         .features = cube,
         .probes =
             {Q("SELECT OS, IsMobile, COUNT(*), SUM(ResolutionWidth) FROM hits_like WHERE OS < 30 "
                "GROUP BY CUBE (OS, IsMobile)"),
              Q("SELECT OS, IsMobile, COUNT(*), SUM(ResolutionWidth) FROM hits_like WHERE OS < 30 "
                "GROUP BY GROUPING SETS ((), (OS), (OS, IsMobile), (IsMobile))")},
         .check = AllEqual()});
    r.push_back({.name = "cube_of_one_key_is_its_rollup",
                 .features = cube,
                 .probes = {Q("SELECT OS, COUNT(*) FROM hits_like GROUP BY CUBE (OS)"),
                            Q("SELECT OS, COUNT(*) FROM hits_like GROUP BY ROLLUP (OS)")},
                 .check = AllEqual()});
  }
  // The variance family (ADR 0037's AG2): VAR_SAMP is STDDEV_SAMP squared, and the moments merged
  // across batches and row groups are those of one pass, within the slt tolerance.
  {
    const slt::FeatureSet variance = {kGroupBy,       kVariance,       kColumns,
                                      kMultipleItems, kIntegerColumns, kArithmetic,
                                      kWhere,         kIntegerLiteral, kTableName};
    r.push_back({.name = "variance_is_stddev_squared",
                 .features = variance,
                 .probes = {Q("SELECT OS, VAR_SAMP(ResolutionWidth) FROM hits_like WHERE OS < 30 "
                              "GROUP BY OS"),
                            Q("SELECT OS, STDDEV_SAMP(ResolutionWidth) * STDDEV(ResolutionWidth) "
                              "FROM hits_like WHERE OS < 30 GROUP BY OS")},
                 .check = AllEqual()});
    constexpr std::string_view kMoments =
        "SELECT OS, STDDEV_POP(ResolutionWidth), VAR_POP(UserID), VARIANCE(ResolutionWidth) FROM "
        "{} WHERE OS < 30 GROUP BY OS";
    Relation sizes{.name = "variance_batch_size_and_layout_invariance",
                   .features = variance,
                   .probes = {},
                   .check = AllEqual()};
    for (const int64_t batch : kBatchSizes) {
      sizes.probes.push_back(Q(std::format(kMoments, "hits_like_split"), batch));
    }
    sizes.probes.push_back(Q(std::format(kMoments, "hits_like")));
    r.push_back(std::move(sizes));
  }
  // A nullable key with dangling values (the star schema's tr_driver, tools/fixturegen/star.h) over
  // row groups of 1200 rows: the same groups, the NULL group among them, whatever the batch size.
  {
    Relation sizes{
        .name = "star_grouped_nullable_key_batch_size_invariance",
        .features = {kGroupBy, kCountStar, kCountColumn, kSum, kMin, kMax, kColumns, kMultipleItems,
                     kIntegerColumns, kDecimalColumns, kDateColumns, kDoubleColumns, kTableName},
        .probes = {},
        .check = AllEqual()};
    for (const int64_t batch : kBatchSizes) {
      sizes.probes.push_back(
          Q("SELECT tr_driver, COUNT(*), COUNT(tr_fare), SUM(tr_fare), "
            "MIN(tr_day), MAX(tr_distance) FROM trips GROUP BY tr_driver",
            batch));
    }
    r.push_back(std::move(sizes));
  }
  r.push_back(
      {.name = "row_count_vs_scan_count_column",
       .features = {kCountStar, kCountColumn, kIntegerColumns, kTableName},
       .probes = {Q("SELECT COUNT(*) FROM hits_like"), Q("SELECT COUNT(WatchID) FROM hits_like"),
                  Q("SELECT COUNT(CounterID) FROM hits_like_required")},
       .check = AllEqual()});
  r.push_back({.name = "row_count_vs_scan_true_predicate",
               .features = {kCountStar, kWhere, kIntegerLiteral, kIntegerColumns, kTableName},
               .probes = {Q("SELECT COUNT(*) FROM hits_like"),
                          Q("SELECT COUNT(*) FROM hits_like WHERE CounterID >= 0")},
               .check = AllEqual()});
  for (const auto& s : kSplits) {
    const slt::FeatureSet features = {kCountStar, kCountColumn, kWhere,
                                      kTableName, s.type,       s.literal_kind};
    // TLP-lite: the rows of a column split into NULLs (COUNT(*) - COUNT(c)) and two partitions.
    r.push_back({.name = std::format("tlp_lt_ge_{}", s.column),
                 .features = features,
                 .probes = {Q(std::format("SELECT COUNT({}) FROM hits_like_nulls", s.column)),
                            Q(std::format("SELECT COUNT(*) FROM hits_like_nulls WHERE {} < {}",
                                          s.column, s.literal)),
                            Q(std::format("SELECT COUNT(*) FROM hits_like_nulls WHERE {} >= {}",
                                          s.column, s.literal))},
                 .check = FirstEqualsSumOfRest()});
    r.push_back({.name = std::format("tlp_eq_ne_{}", s.column),
                 .features = features,
                 .probes = {Q(std::format("SELECT COUNT({}) FROM hits_like_nulls", s.column)),
                            Q(std::format("SELECT COUNT(*) FROM hits_like_nulls WHERE {} = {}",
                                          s.column, s.literal)),
                            Q(std::format("SELECT COUNT(*) FROM hits_like_nulls WHERE {} <> {}",
                                          s.column, s.literal))},
                 .check = FirstEqualsSumOfRest()});
  }
  r.push_back(
      {.name = "and_is_symmetric",
       .features = {kCountStar, kWhere, kWhereAnd, kIntegerColumns, kVarcharColumns,
                    kIntegerLiteral, kStringLiteral, kTableName},
       .probes =
           {Q("SELECT COUNT(*) FROM hits_like_nulls WHERE RegionID < 10000 AND Title >= 'm'"),
            Q("SELECT COUNT(*) FROM hits_like_nulls WHERE Title >= 'm' AND RegionID < 10000")},
       .check = AllEqual()});
  r.push_back({.name = "literal_first_is_mirrored",
               .features = {kCountStar, kWhere, kLiteralFirst, kIntegerColumns, kIntegerLiteral,
                            kTableName},
               .probes = {Q("SELECT COUNT(*) FROM hits_like WHERE ResolutionWidth > 1366"),
                          Q("SELECT COUNT(*) FROM hits_like WHERE 1366 < ResolutionWidth")},
               .check = AllEqual()});
  // ORDER BY (with a unique last key, so that the order is total): the same rows in the same order
  // whatever the batch size and file layout; a top-N is the window of the full sort.
  {
    const slt::FeatureSet sorted = {kOrderBy,        kColumns,  kMultipleItems,  kIntegerColumns,
                                    kVarcharColumns, kWhere,    kIntegerLiteral, kLimit,
                                    kOffset,         kTableName};
    const auto sorted_sql = [](std::string_view table, bool top_n) {
      return std::format(
          "SELECT WatchID, OS, URL FROM {} WHERE RegionID < 2000 ORDER BY OS DESC, URL, WatchID{}",
          table, top_n ? " LIMIT 40 OFFSET 25" : "");
    };
    for (const bool top_n : {false, true}) {
      Relation sizes{.name = std::format("{}_batch_size_invariance", top_n ? "top_n" : "sorted"),
                     .features = sorted,
                     .probes = {},
                     .check = AllEqualInOrder()};
      for (const int64_t batch : kBatchSizes) {
        sizes.probes.push_back(Q(sorted_sql("hits_like_split", top_n), batch));
      }
      sizes.probes.push_back(Q(sorted_sql("hits_like", top_n)));
      r.push_back(std::move(sizes));
    }
    for (const int64_t batch : kBatchSizes) {
      r.push_back({.name = std::format("top_n_is_window_of_sort_batch_{}", batch),
                   .features = sorted,
                   .probes = {Q(sorted_sql("hits_like_split", false), batch),
                              Q(sorted_sql("hits_like_split", true), batch)},
                   .check = SecondIsWindowOfFirst(25, 40)});
    }
    const slt::FeatureSet grouped_top_n = {kOrderBy,        kGroupBy,       kCountStar,
                                           kColumns,        kMultipleItems, kAlias,
                                           kIntegerColumns, kLimit,         kTableName};
    Relation grouped{.name = "grouped_top_n_batch_size_invariance",
                     .features = grouped_top_n,
                     .probes = {},
                     .check = AllEqualInOrder()};
    for (const int64_t batch : kBatchSizes) {
      grouped.probes.push_back(
          Q("SELECT RegionID, COUNT(*) AS c FROM hits_like_split GROUP BY RegionID "
            "ORDER BY c DESC, RegionID LIMIT 15",
            batch));
    }
    r.push_back(std::move(grouped));
  }
  // HAVING partitions the groups by a condition that is never NULL (on a count, or on a key that
  // has no NULL) and its complement, and filters the same groups whatever the batch size and file
  // layout.
  {
    const slt::FeatureSet having = {kGroupBy,        kHaving,         kCountStar, kSum,
                                    kColumns,        kMultipleItems,  kAlias,     kIntegerColumns,
                                    kIntegerLiteral, kDecimalLiteral, kTableName};
    constexpr std::string_view kGroups =
        "SELECT OS, COUNT(*) AS c, SUM(ResolutionWidth) FROM hits_like GROUP BY OS";
    r.push_back({.name = "having_partitions_groups",
                 .features = having,
                 .probes = {Q(std::string(kGroups)), Q(std::format("{} HAVING c > 100", kGroups)),
                            Q(std::format("{} HAVING COUNT(*) <= 100.5", kGroups))},
                 .check = FirstIsUnionOfRest()});
    r.push_back({.name = "having_key_partitions_groups",
                 .features = having,
                 .probes = {Q(std::string(kGroups)), Q(std::format("{} HAVING OS < 5", kGroups)),
                            Q(std::format("{} HAVING 5 <= OS", kGroups))},
                 .check = FirstIsUnionOfRest()});
    slt::FeatureSet in = having;
    in.Add(kIn);
    r.push_back({.name = "having_in_partitions_groups",
                 .features = in,
                 .probes = {Q(std::string(kGroups)),
                            Q(std::format("{} HAVING OS IN (0, 2, 99) AND c > 0", kGroups)),
                            Q(std::format("{} HAVING OS NOT IN (0, 2, 99)", kGroups))},
                 .check = FirstIsUnionOfRest()});
    constexpr std::string_view kFiltered =
        "SELECT RegionID, COUNT(*) AS c FROM {} WHERE OS < 30 GROUP BY RegionID "
        "HAVING c >= 3 AND MAX(ResolutionWidth) > 1000";
    Relation sizes{.name = "having_batch_size_invariance",
                   .features = {kGroupBy, kHaving, kCountStar, kMax, kColumns, kMultipleItems,
                                kAlias, kIntegerColumns, kWhere, kIntegerLiteral, kTableName},
                   .probes = {},
                   .check = AllEqual()};
    for (const int64_t batch : kBatchSizes) {
      sizes.probes.push_back(Q(std::format(kFiltered, "hits_like_split"), batch));
    }
    sizes.probes.push_back(Q(std::format(kFiltered, "hits_like")));
    r.push_back(std::move(sizes));
  }
  // Arithmetic: SUM(x + c) is SUM(x) + c * COUNT(x) whether the engine rewrites it or computes the
  // right side itself; a comparison of x + c with a literal is one of x; expression keys group
  // alike whatever the batch size and file layout.
  {
    const slt::FeatureSet sum_features = {kSum,           kCountColumn,    kArithmetic,
                                          kMultipleItems, kIntegerColumns, kIntegerLiteral,
                                          kTableName};
    r.push_back({.name = "sum_of_column_plus_constant",
                 .features = sum_features,
                 .probes = {Q("SELECT SUM(ResolutionWidth + 7), SUM(RegionID + 1) FROM hits_like"),
                            Q("SELECT SUM(ResolutionWidth) + 7 * COUNT(ResolutionWidth), "
                              "COUNT(RegionID) + SUM(RegionID) FROM hits_like")},
                 .check = AllEqual()});
    r.push_back({.name = "where_arithmetic_shifts_the_literal",
                 .features = {kCountStar, kWhere, kArithmetic, kIntegerColumns, kIntegerLiteral,
                              kTableName},
                 .probes = {Q("SELECT COUNT(*) FROM hits_like_nulls WHERE RegionID + 10 > 1000"),
                            Q("SELECT COUNT(*) FROM hits_like_nulls WHERE RegionID > 990"),
                            Q("SELECT COUNT(*) FROM hits_like_nulls WHERE 2 * RegionID >= 1982")},
                 .check = AllEqual()});
    constexpr std::string_view kKeys =
        "SELECT ClientIP - 1, ClientIP // 1000 AS k, COUNT(*), SUM(ResolutionWidth * 2) FROM {} "
        "WHERE ClientIP > 0 GROUP BY ClientIP - 1, k";
    Relation sizes{.name = "expression_keys_batch_size_invariance",
                   .features = {kGroupBy, kCountStar, kSum, kArithmetic, kMultipleItems, kAlias,
                                kIntegerColumns, kWhere, kIntegerLiteral, kTableName},
                   .probes = {},
                   .check = AllEqual()};
    for (const int64_t batch : kBatchSizes) {
      sizes.probes.push_back(Q(std::format(kKeys, "hits_like_split"), batch));
    }
    sizes.probes.push_back(Q(std::format(kKeys, "hits_like")));
    r.push_back(std::move(sizes));
  }
  // String functions: inserting one character adds one byte per non-NULL value wherever the match
  // is; regexp_replace keys group alike whatever the batch size and file layout.
  {
    r.push_back(
        {.name = "regexp_replace_insertion_adds_one_byte",
         .features = {kSum, kCountColumn, kArithmetic, kStringFunctions, kVarcharColumns,
                      kStringLiteral, kTableName},
         .probes = {Q("SELECT SUM(strlen(URL)) + COUNT(URL) FROM hits_like_nulls"),
                    Q("SELECT SUM(strlen(regexp_replace(URL, '$', 'x'))) FROM hits_like_nulls"),
                    Q("SELECT SUM(STRLEN(REGEXP_REPLACE(URL, '^', '\\\\'))) FROM hits_like_nulls")},
         .check = AllEqual()});
    constexpr std::string_view kKeys =
        "SELECT regexp_replace(Referer, '//([^/?]*)', '<\\1>') AS k, COUNT(*), "
        "SUM(strlen(Referer)), MAX(Referer) FROM {} GROUP BY k";
    Relation sizes{.name = "regexp_replace_keys_batch_size_invariance",
                   .features = {kGroupBy, kCountStar, kSum, kMax, kStringFunctions, kMultipleItems,
                                kAlias, kVarcharColumns, kStringLiteral, kTableName},
                   .probes = {},
                   .check = AllEqual()};
    for (const int64_t batch : kBatchSizes) {
      sizes.probes.push_back(Q(std::format(kKeys, "hits_like_split"), batch));
    }
    sizes.probes.push_back(Q(std::format(kKeys, "hits_like")));
    r.push_back(std::move(sizes));
    // substring splits a value in two at any point, and the two pieces hold every byte of it once:
    // a window is counted in codepoints while strlen counts bytes (divergence D15), so this holds
    // only if no byte falls outside every codepoint, whatever bytes the column has.
    const slt::FeatureSet split = {kSum,        kSubstring,      kStringFunctions,
                                   kArithmetic, kVarcharColumns, kTableName};
    for (const int k : {1, 2, 7}) {
      r.push_back(
          {.name = std::format("substring_splits_every_byte_once_at_{}", k),
           .features = split,
           .probes = {Q("SELECT SUM(strlen(URL)) FROM hits_like_nulls"),
                      Q(std::format("SELECT SUM(strlen(substring(URL, 1, {}))) + "
                                    "SUM(strlen(substring(URL, {}))) FROM hits_like_nulls",
                                    k, k + 1)),
                      Q(std::format("SELECT SUM(STRLEN(SUBSTRING(URL FROM 1 FOR {}))) + "
                                    "SUM(STRLEN(SUBSTRING(URL FROM {}))) FROM hits_like_nulls",
                                    k, k + 1))},
           .check = AllEqual()});
    }
    // A window that reaches past either end of the value is the value, and one that closes before
    // it opens is empty: three spellings of the whole value agree, and an empty one adds nothing.
    // The start of -4294967295 is the boundary case, since one step further out loses a codepoint:
    // with no length written DuckDB's is 4294967295, not the end of the value.
    r.push_back({.name = "substring_clips_to_the_value",
                 .features = SetWith(split, kNegativeLiteral),
                 .probes = {Q("SELECT SUM(strlen(URL)) FROM hits_like_nulls"),
                            Q("SELECT SUM(strlen(substring(URL, 1, 4294967295))) "
                              "FROM hits_like_nulls"),
                            Q("SELECT SUM(strlen(substring(URL, -4294967295))) "
                              "FROM hits_like_nulls"),
                            Q("SELECT SUM(strlen(substring(URL, 1))) + "
                              "SUM(strlen(substring(URL, 1, -1))) FROM hits_like_nulls")},
                 .check = AllEqual()});
  }
  // Boolean conditions: OR and AND count like a union and an intersection; p and NOT p split the
  // non-NULL values of p's column (NULL is neither, folded comparisons included); CASE WHEN p
  // counts what WHERE p keeps; CASE keys group alike whatever the batch size and file layout.
  {
    constexpr std::string_view kP = "RegionID > 200";
    constexpr std::string_view kQ = "URL LIKE '%a%'";
    r.push_back(
        {.name = "or_and_count_like_union_and_intersection",
         .features = {kSum, kCase, kBooleanExpressions, kArithmetic, kIntegerColumns,
                      kVarcharColumns, kIntegerLiteral, kStringLiteral, kLike, kTableName},
         .probes = {Q(std::format(
                        "SELECT SUM(CASE WHEN {0} OR {1} THEN 1 ELSE 0 END) + "
                        "SUM(CASE WHEN {0} AND {1} THEN 1 ELSE 0 END) FROM hits_like_nulls",
                        kP, kQ)),
                    Q(std::format("SELECT SUM(CASE WHEN {} THEN 1 ELSE 0 END) + "
                                  "SUM(CASE WHEN {} THEN 1 ELSE 0 END) FROM hits_like_nulls",
                                  kP, kQ))},
         .check = AllEqual()});
    for (const auto& [suffix, condition] :
         std::to_array<std::pair<std::string_view, std::string_view>>(
             {{"compare", "RegionID > 200"},
              {"never_true", "RegionID = 1.5"},
              {"arithmetic", "RegionID + 1 < 100000"}})) {
      r.push_back(
          {.name = std::format("not_splits_non_null_values_{}", suffix),
           .features = {kCountStar, kCountColumn, kWhere, kBooleanExpressions, kIntegerColumns,
                        kIntegerLiteral, kDecimalLiteral, kArithmetic, kTableName},
           .probes = {Q("SELECT COUNT(RegionID) FROM hits_like_nulls"),
                      Q(std::format("SELECT COUNT(*) FROM hits_like_nulls WHERE {}", condition)),
                      Q(std::format("SELECT COUNT(*) FROM hits_like_nulls WHERE NOT ({})",
                                    condition))},
           .check = FirstEqualsSumOfRest()});
    }
    r.push_back(
        {.name = "case_counts_what_where_keeps",
         .features = {kCountStar, kCountColumn, kWhere, kCase, kBooleanExpressions, kIntegerColumns,
                      kVarcharColumns, kIntegerLiteral, kStringLiteral, kLike, kTableName},
         .probes = {Q(std::format("SELECT COUNT(*) FROM hits_like_nulls WHERE {} OR {}", kP, kQ)),
                    Q(std::format("SELECT COUNT(CASE WHEN {} OR {} THEN 1 END) FROM "
                                  "hits_like_nulls",
                                  kP, kQ))},
         .check = AllEqual()});
    constexpr std::string_view kKeys =
        "SELECT CASE WHEN IsMobile = 1 AND OS > 100 THEN URL WHEN OS < 10 THEN 'low' ELSE '' END "
        "AS k, COUNT(*), SUM(RegionID) FROM {} GROUP BY k";
    Relation sizes{
        .name = "case_keys_batch_size_invariance",
        .features = {kGroupBy, kCountStar, kSum, kCase, kBooleanExpressions, kMultipleItems, kAlias,
                     kIntegerColumns, kVarcharColumns, kIntegerLiteral, kStringLiteral, kTableName},
        .probes = {},
        .check = AllEqual()};
    for (const int64_t batch : kBatchSizes) {
      sizes.probes.push_back(Q(std::format(kKeys, "hits_like_split"), batch));
    }
    sizes.probes.push_back(Q(std::format(kKeys, "hits_like")));
    r.push_back(std::move(sizes));
  }
  // Timestamps: for seconds t >= 0, EXTRACT and date_trunc of toDateTime(t) agree with integer
  // arithmetic on t; date_trunc keys group alike whatever the batch size and file layout.
  {
    r.push_back({.name = "extract_minute_is_seconds_arithmetic",
                 .features = {kSum, kTimestamps, kArithmetic, kWhere, kIntegerColumns,
                              kIntegerLiteral, kTableName},
                 .probes = {Q("SELECT SUM(EXTRACT(minute FROM toDateTime(EventTime))) FROM "
                              "hits_like_nulls WHERE EventTime >= 0"),
                            Q("SELECT SUM((EventTime // 60) % 60) FROM hits_like_nulls WHERE "
                              "EventTime >= 0")},
                 .check = AllEqual()});
    r.push_back(
        {.name = "date_trunc_day_counts_like_whole_days",
         .features = {kCountDistinct, kTimestamps, kArithmetic, kWhere, kIntegerColumns,
                      kIntegerLiteral, kStringLiteral, kTableName},
         .probes = {Q("SELECT COUNT(DISTINCT date_trunc('day', toDateTime(EventTime))) FROM "
                      "hits_like_nulls WHERE EventTime >= 0"),
                    Q("SELECT COUNT(DISTINCT EventTime // 86400) FROM hits_like_nulls WHERE "
                      "EventTime >= 0")},
         .check = AllEqual()});
    r.push_back({.name = "extract_dow_is_days_arithmetic",
                 .features = {kSum, kTimestamps, kArithmetic, kWhere, kIntegerColumns,
                              kIntegerLiteral, kTableName},
                 .probes = {Q("SELECT SUM(EXTRACT(dow FROM toDateTime(EventTime))) FROM "
                              "hits_like_nulls WHERE EventTime >= 0"),
                            Q("SELECT SUM((EventTime // 86400 + 4) % 7) FROM hits_like_nulls WHERE "
                              "EventTime >= 0")},
                 .check = AllEqual()});
    constexpr std::string_view kKeys =
        "SELECT date_trunc('hour', toDateTime(EventTime)) AS k, COUNT(*), "
        "MIN(toDateTime(EventTime)), MAX(EXTRACT(second FROM toDateTime(EventTime))) FROM {} GROUP "
        "BY k";
    Relation sizes{.name = "date_trunc_keys_batch_size_invariance",
                   .features = {kGroupBy, kCountStar, kMin, kMax, kTimestamps, kMultipleItems,
                                kAlias, kIntegerColumns, kStringLiteral, kTableName},
                   .probes = {},
                   .check = AllEqual()};
    for (const int64_t batch : kBatchSizes) {
      sizes.probes.push_back(Q(std::format(kKeys, "hits_like_split"), batch));
    }
    sizes.probes.push_back(Q(std::format(kKeys, "hits_like")));
    r.push_back(std::move(sizes));
  }
  // [NOT] IN partitions the non-NULL values of a column, and IN over distinct values counts the
  // rows equal to each.
  for (const auto& [column, values, type, literal] :
       std::to_array<std::tuple<std::string_view, std::string_view, slt::Feature, slt::Feature>>(
           {{"RegionID", "1, 2, 229, 70000", kIntegerColumns, kIntegerLiteral},
            {"OS", "1, 3, 5, 7", kIntegerColumns, kIntegerLiteral},
            {"SearchPhrase", "'', 'a'", kVarcharColumns, kStringLiteral}})) {
    const slt::FeatureSet features = {kCountStar, kCountColumn, kWhere, kIn,
                                      kTableName, type,         literal};
    r.push_back(
        {.name = std::format("in_partitions_{}", column),
         .features = features,
         .probes = {Q(std::format("SELECT COUNT({}) FROM hits_like_nulls", column)),
                    Q(std::format("SELECT COUNT(*) FROM hits_like_nulls WHERE {} IN ({})", column,
                                  values)),
                    Q(std::format("SELECT COUNT(*) FROM hits_like_nulls WHERE {} NOT IN ({})",
                                  column, values))},
         .check = FirstEqualsSumOfRest()});
    Relation sum{.name = std::format("in_is_sum_of_equalities_{}", column),
                 .features = features,
                 .probes = {Q(std::format("SELECT COUNT(*) FROM hits_like_nulls WHERE {} IN ({})",
                                          column, values))},
                 .check = FirstEqualsSumOfRest()};
    std::string_view rest = values;
    while (!rest.empty()) {
      const std::size_t comma = std::min(rest.find(", "), rest.size());
      sum.probes.push_back(Q(std::format("SELECT COUNT(*) FROM hits_like_nulls WHERE {} = {}",
                                         column, rest.substr(0, comma))));
      rest = comma < rest.size() ? rest.substr(comma + 2) : std::string_view{};
    }
    r.push_back(std::move(sum));
  }
  // [NOT] LIKE partitions the non-NULL values of a column (NULL is in neither part), for patterns
  // with %, _, both, and none; the same per file of a split table.
  for (const auto& [name, pattern] :
       std::to_array<std::pair<std::string_view, std::string_view>>({{"contains", "%a%"},
                                                                     {"prefix", "http%"},
                                                                     {"underscore", "%_o_%"},
                                                                     {"segments", "%e%a%"},
                                                                     {"exact", ""},
                                                                     {"single", "_"}})) {
    for (const std::string_view column : {"Title", "URL"}) {
      r.push_back(
          {.name = std::format("like_partitions_{}_{}", column, name),
           .features = {kCountStar, kCountColumn, kWhere, kLike, kVarcharColumns, kTableName},
           .probes = {Q(std::format("SELECT COUNT({}) FROM hits_like_nulls", column)),
                      Q(std::format("SELECT COUNT(*) FROM hits_like_nulls WHERE {} LIKE '{}'",
                                    column, pattern)),
                      Q(std::format("SELECT COUNT(*) FROM hits_like_nulls WHERE {} NOT LIKE '{}'",
                                    column, pattern))},
           .check = FirstEqualsSumOfRest()});
    }
  }
  r.push_back({.name = "like_split_is_sum_of_parts",
               .features = {kCountStar, kWhere, kLike, kVarcharColumns, kTableName, kTablePath},
               .probes = {Q("SELECT COUNT(*) FROM hits_like_split WHERE URL LIKE '%a%b%'"),
                          Q("SELECT COUNT(*) FROM '${FIXTURES}/hits_like_split/part-0.parquet' "
                            "WHERE URL LIKE '%a%b%'"),
                          Q("SELECT COUNT(*) FROM '${FIXTURES}/hits_like_split/part-1.parquet' "
                            "WHERE URL LIKE '%a%b%'"),
                          Q("SELECT COUNT(*) FROM '${FIXTURES}/hits_like_split/part-2.parquet' "
                            "WHERE URL LIKE '%a%b%'"),
                          Q("SELECT COUNT(*) FROM '${FIXTURES}/hits_like_split/part-3.parquet' "
                            "WHERE URL LIKE '%a%b%'")},
               .check = FirstEqualsSumOfRest()});
  // COUNT(DISTINCT): the number of groups of the column (NULL left out by a WHERE that is true for
  // every value), the same over a split table and for every batch size, also per group.
  for (const auto& [column, type] : std::to_array<std::pair<std::string_view, slt::Feature>>(
           {{"RegionID", kIntegerColumns}, {"SearchPhrase", kVarcharColumns}})) {
    const bool text = type == kVarcharColumns;
    r.push_back(
        {.name = std::format("count_distinct_is_group_count_{}", column),
         .features = {kCountDistinct, kGroupBy, kColumns, kWhere, kTableName, type,
                      text ? kStringLiteral : kIntegerLiteral, kNegativeLiteral},
         .probes = {Q(std::format("SELECT COUNT(DISTINCT {}) FROM hits_like_nulls", column)),
                    Q(std::format("SELECT {0} FROM hits_like_nulls WHERE {0} >= {1} GROUP BY {0}",
                                  column, text ? "''" : "-2147483648"))},
         .check = RowCountsEqualFirst()});
  }
  {
    Relation sizes{
        .name = "count_distinct_batch_size_and_split_invariance",
        .features = {kCountDistinct, kMultipleItems, kIntegerColumns, kVarcharColumns, kTableName},
        .probes = {},
        .check = AllEqual()};
    constexpr std::string_view kDistinct =
        "SELECT COUNT(DISTINCT UserID), COUNT(DISTINCT SearchPhrase), COUNT(DISTINCT RegionID) "
        "FROM ";
    for (const int64_t batch : kBatchSizes) {
      sizes.probes.push_back(Q(std::string(kDistinct) + "hits_like_split", batch));
    }
    sizes.probes.push_back(Q(std::string(kDistinct) + "hits_like"));
    r.push_back(std::move(sizes));
    Relation grouped{.name = "grouped_count_distinct_batch_size_and_split_invariance",
                     .features = {kCountDistinct, kGroupBy, kColumns, kMultipleItems,
                                  kIntegerColumns, kVarcharColumns, kTableName},
                     .probes = {},
                     .check = AllEqual()};
    constexpr std::string_view kGroupedDistinct =
        "SELECT OS, COUNT(DISTINCT UserID), COUNT(DISTINCT URL) FROM {} GROUP BY OS";
    for (const int64_t batch : kBatchSizes) {
      grouped.probes.push_back(
          Q(std::vformat(kGroupedDistinct, std::make_format_args("hits_like_split")), batch));
    }
    grouped.probes.push_back(Q(std::vformat(kGroupedDistinct, std::make_format_args("hits_like"))));
    r.push_back(std::move(grouped));
  }
  r.push_back({.name = "limit_returns_min_of_n_and_rows",
               .features = {kCountStar, kColumns, kIntegerColumns, kLimit, kTableName},
               .probes = {Q("SELECT COUNT(*) FROM edge"), Q("SELECT id FROM edge LIMIT 0"),
                          Q("SELECT id FROM edge LIMIT 5"), Q("SELECT id FROM edge LIMIT 100"),
                          Q("SELECT id FROM edge LIMIT 5", 1), Q("SELECT id FROM edge LIMIT 5", 7)},
               .check = RowCountsAreMinOf({0, 5, 100, 5, 5})});
  r.push_back(
      {.name = "projection_rows_equal_count",
       .features = {kCountStar, kColumns, kWhere, kIntegerColumns, kIntegerLiteral, kTableName},
       .probes = {Q("SELECT COUNT(*) FROM hits_like_nulls WHERE RegionID < 500"),
                  Q("SELECT WatchID FROM hits_like_nulls WHERE RegionID < 500"),
                  Q("SELECT WatchID FROM hits_like_nulls WHERE RegionID < 500", 7)},
       .check = RowCountsEqualFirst()});
  r.push_back(
      {.name = "split_sum_is_sum_of_parts",
       .features = {kSum, kIntegerColumns, kTableName, kTablePath},
       .probes = {Q("SELECT SUM(UserID) FROM hits_like_split"),
                  Q("SELECT SUM(UserID) FROM '${FIXTURES}/hits_like_split/part-0.parquet'"),
                  Q("SELECT SUM(UserID) FROM '${FIXTURES}/hits_like_split/part-1.parquet'"),
                  Q("SELECT SUM(UserID) FROM '${FIXTURES}/hits_like_split/part-2.parquet'"),
                  Q("SELECT SUM(UserID) FROM '${FIXTURES}/hits_like_split/part-3.parquet'")},
       .check = FirstEqualsSumOfRest()});
  r.push_back(
      {.name = "split_min_is_min_of_parts",
       .features = {kMin, kDateColumns, kTableName, kTablePath},
       .probes = {Q("SELECT MIN(EventDate) FROM hits_like_split"),
                  Q("SELECT MIN(EventDate) FROM '${FIXTURES}/hits_like_split/part-0.parquet'"),
                  Q("SELECT MIN(EventDate) FROM '${FIXTURES}/hits_like_split/part-1.parquet'"),
                  Q("SELECT MIN(EventDate) FROM '${FIXTURES}/hits_like_split/part-2.parquet'"),
                  Q("SELECT MIN(EventDate) FROM '${FIXTURES}/hits_like_split/part-3.parquet'")},
       .check = FirstEqualsMinOfRest()});
  r.push_back({.name = "split_max_is_max_of_parts",
               .features = {kMax, kVarcharColumns, kTableName, kTablePath},
               .probes = {Q("SELECT MAX(URL) FROM hits_like_split"),
                          Q("SELECT MAX(URL) FROM '${FIXTURES}/hits_like_split/part-0.parquet'"),
                          Q("SELECT MAX(URL) FROM '${FIXTURES}/hits_like_split/part-1.parquet'"),
                          Q("SELECT MAX(URL) FROM '${FIXTURES}/hits_like_split/part-2.parquet'"),
                          Q("SELECT MAX(URL) FROM '${FIXTURES}/hits_like_split/part-3.parquet'")},
               .check = FirstEqualsMaxOfRest()});
  // Partitions of aggregates: hits_like has no NULL, so two complementary predicates split every
  // row.
  r.push_back({.name = "partition_sum_is_sum_of_parts",
               .features = {kSum, kWhere, kIntegerColumns, kIntegerLiteral, kTableName},
               .probes = {Q("SELECT SUM(UserID) FROM hits_like"),
                          Q("SELECT SUM(UserID) FROM hits_like WHERE RegionID < 10000"),
                          Q("SELECT SUM(UserID) FROM hits_like WHERE RegionID >= 10000")},
               .check = FirstEqualsSumOfRest()});
  r.push_back(
      {.name = "partition_min_is_min_of_parts",
       .features = {kMin, kWhere, kDateColumns, kVarcharColumns, kStringLiteral, kTableName},
       .probes = {Q("SELECT MIN(EventDate) FROM hits_like"),
                  Q("SELECT MIN(EventDate) FROM hits_like WHERE Title < 'm'"),
                  Q("SELECT MIN(EventDate) FROM hits_like WHERE Title >= 'm'")},
       .check = FirstEqualsMinOfRest()});
  r.push_back({.name = "partition_max_is_max_of_parts",
               .features = {kMax, kWhere, kVarcharColumns, kDateColumns, kDateLiteral, kTableName},
               .probes = {Q("SELECT MAX(URL) FROM hits_like"),
                          Q("SELECT MAX(URL) FROM hits_like WHERE EventDate < DATE '2013-07-15'"),
                          Q("SELECT MAX(URL) FROM hits_like WHERE EventDate >= DATE '2013-07-15'")},
               .check = FirstEqualsMaxOfRest()});
  // Literal folding: equivalent predicates select the same rows.
  r.push_back({.name = "folded_decimal_bound_equals_integer_bound",
               .features = {kCountStar, kWhere, kLiteralFirst, kIntegerColumns, kIntegerLiteral,
                            kDecimalLiteral, kTableName},
               .probes = {Q("SELECT COUNT(*) FROM hits_like WHERE ResolutionWidth >= 1367"),
                          Q("SELECT COUNT(*) FROM hits_like WHERE ResolutionWidth > 1366.5"),
                          Q("SELECT COUNT(*) FROM hits_like WHERE 1366.5 < ResolutionWidth")},
               .check = AllEqual()});
  r.push_back(
      {.name = "folded_out_of_range_bound_keeps_non_null_values",
       .features = {kCountStar, kCountColumn, kWhere, kIntegerColumns, kIntegerLiteral,
                    kNegativeLiteral, kTableName},
       .probes =
           {Q("SELECT COUNT(Interests) FROM hits_like_nulls"),
            Q("SELECT COUNT(*) FROM hits_like_nulls WHERE Interests < 40000"),
            Q("SELECT COUNT(*) FROM hits_like_nulls WHERE Interests > -40000"),
            Q("SELECT COUNT(*) FROM hits_like_nulls WHERE Interests <> 99999999999999999999")},
       .check = AllEqual()});
  r.push_back(
      {.name = "folded_never_true_selects_nothing",
       .features = {kCountStar, kWhere, kIntegerColumns, kIntegerLiteral, kDecimalLiteral,
                    kNegativeLiteral, kTableName},
       .probes = {Q("SELECT COUNT(*) FROM empty"),
                  Q("SELECT COUNT(*) FROM hits_like WHERE RegionID = 1.5"),
                  Q("SELECT COUNT(*) FROM hits_like WHERE Interests > 32767.5"),
                  Q("SELECT COUNT(*) FROM hits_like WHERE EventTime < -9223372036854775809")},
       .check = AllEqual()});
  // The spellings of a date (DuckDB types all four as the DATE) select the same rows.
  r.push_back({.name = "date_spellings_select_the_same_rows",
               .features = {kCountStar, kColumns, kMultipleItems, kGroupBy, kWhere, kWhereAnd,
                            kLiteralFirst, kDateColumns, kStringLiteral, kDateLiteral, kCastDate,
                            kTableName},
               .probes = {Q("SELECT EventDate, COUNT(*) FROM hits_like WHERE EventDate >= DATE "
                            "'2013-07-15' AND EventDate <> DATE '2013-07-20' GROUP BY EventDate"),
                          Q("SELECT EventDate, COUNT(*) FROM hits_like WHERE EventDate >= "
                            "'2013-07-15' AND EventDate <> '2013-07-20' GROUP BY EventDate"),
                          Q("SELECT EventDate, COUNT(*) FROM hits_like WHERE EventDate >= "
                            "CAST('2013-07-15' AS DATE) AND EventDate <> CAST('2013-07-20' AS "
                            "DATE) GROUP BY EventDate"),
                          Q("SELECT EventDate, COUNT(*) FROM hits_like WHERE EventDate >= "
                            "'2013-07-15'::date AND EventDate <> '2013-07-20'::DATE GROUP BY "
                            "EventDate"),
                          Q("SELECT EventDate, COUNT(*) FROM hits_like WHERE CAST('2013-07-15' AS "
                            "DATE) <= EventDate AND '2013-07-20'::DATE <> EventDate GROUP BY "
                            "EventDate")},
               .check = AllEqual()});
  const std::string aggregates =
      "SELECT COUNT(*), COUNT(Title), SUM(UserID), AVG(ResolutionWidth), MIN(EventDate), MAX(URL) "
      "FROM ";
  const slt::FeatureSet aggregate_features = {
      kCountStar,      kCountColumn,    kSum,         kAvg,      kMin, kMax, kMultipleItems,
      kIntegerColumns, kVarcharColumns, kDateColumns, kTableName};
  r.push_back({.name = "variants_aggregates_equal",
               .features = aggregate_features,
               .probes = {Q(aggregates + "hits_like"), Q(aggregates + "hits_like_split"),
                          Q(aggregates + "hits_like_required")},
               .check = AllEqual()});
  Relation batches{.name = "batch_size_invariance_aggregates",
                   .features = aggregate_features,
                   .probes = {},
                   .check = AllEqual()};
  Relation filtered{
      .name = "batch_size_invariance_filter",
      .features = {kCountStar, kWhere, kWhereAnd, kIntegerColumns, kIntegerLiteral, kTableName},
      .probes = {},
      .check = AllEqual()};
  Relation projected{.name = "batch_size_invariance_projection",
                     .features = {kColumns, kMultipleItems, kWhere, kIntegerColumns,
                                  kVarcharColumns, kIntegerLiteral, kTableName},
                     .probes = {},
                     .check = AllEqual()};
  for (const int64_t batch : kBatchSizes) {
    batches.probes.push_back(Q(aggregates + "hits_like_nulls", batch));
    filtered.probes.push_back(Q(
        "SELECT COUNT(*) FROM hits_like_nulls WHERE RegionID < 10000 AND AdvEngineID <> 0", batch));
    projected.probes.push_back(
        Q("SELECT WatchID, Title FROM hits_like_nulls WHERE RegionID < 500", batch));
  }
  r.push_back(std::move(batches));
  r.push_back(std::move(filtered));
  r.push_back(std::move(projected));

  // ---- joins (ADR 0022) ----
  // An inner join gives the same rows in any FROM order and with any connector, its keys in ON or
  // in WHERE, with or without aliases and qualifiers, in batches of any size (the probe resumes
  // inside a batch): 2841 rows over the star schema, 96 trips meeting two riders (a repeated key),
  // and 443 rows of three tables on a two-column key, starting with two unconnected tables.
  {
    const slt::FeatureSet joined = {kColumns,   kMultipleItems, kIntegerColumns, kTableName,
                                    kCommaJoin, kJoinOn,        kTableAlias,     kQualifiedName};
    constexpr std::string_view kRiders =
        "SELECT tr_row, rd_row, rd_city FROM trips JOIN riders ON tr_rider = rd_id";
    r.push_back(
        {.name = "star_join_commutes",
         .features = joined,
         .probes = {Q(std::string(kRiders)),
                    Q("SELECT tr_row, rd_row, rd_city FROM riders JOIN trips ON rd_id = tr_rider"),
                    Q("SELECT tr_row, rd_row, rd_city FROM trips, riders WHERE tr_rider = rd_id"),
                    Q("SELECT t.tr_row, r.rd_row, r.rd_city FROM riders AS r CROSS JOIN trips AS t "
                      "WHERE r.rd_id = t.tr_rider"),
                    Q(std::string(kRiders), 1), Q(std::string(kRiders), 7)},
         .check = AllEqual()});
    slt::FeatureSet three = joined;
    three.Add(kDateColumns);
    constexpr std::string_view kShifts =
        "SELECT tr_row, sh_row, dv_row FROM trips JOIN shifts ON tr_driver = sh_driver AND tr_day "
        "= sh_day JOIN drivers ON sh_driver = dv_id";
    r.push_back(
        {.name = "star_join_order_three_tables",
         .features = three,
         .probes = {Q(std::string(kShifts)),
                    Q("SELECT tr_row, sh_row, dv_row FROM drivers, trips, shifts WHERE tr_driver = "
                      "sh_driver AND tr_day = sh_day AND sh_driver = dv_id"),
                    Q("SELECT t.tr_row, s.sh_row, d.dv_row FROM shifts AS s JOIN drivers AS d ON "
                      "d.dv_id = s.sh_driver, trips AS t WHERE t.tr_driver = s.sh_driver AND "
                      "t.tr_day = s.sh_day"),
                    Q(std::string(kShifts), 1), Q(std::string(kShifts), 7)},
         .check = AllEqual()});
  }

  // ---- pending until derived tables and CTEs are answered (roadmap PR J4, ADR 0022) ----
  // A single-use derived table or CTE is its block's relation, so wrapping a block changes nothing
  // but the plan: the same 244 groups however the wrap is written, at any batch size, nested twice,
  // over a top-N, and with a column alias list. The grouped block's counts still sum to the rows it
  // read, a predicate on its group key may sit inside or outside it, and the columns the outer
  // block does not read are pruned through the boundary.
  {
    const slt::FeatureSet wrapped = {kColumns,        kMultipleItems,  kAlias,     kCountStar,
                                     kGroupBy,        kIntegerColumns, kTableName, kWhere,
                                     kIntegerLiteral, kDerivedTable};
    constexpr std::string_view kBlock =
        "SELECT tr_rider AS k, COUNT(*) AS n FROM trips WHERE tr_row > 100 GROUP BY tr_rider";
    const std::string wrap = std::format("SELECT k, n FROM ({}) AS s", kBlock);
    slt::FeatureSet with_cte = wrapped;
    with_cte.Add(kCommonTableExpr);
    r.push_back({.name = "derived_table_wraps_block",
                 .features = with_cte,
                 .probes = {Q(std::string(kBlock)), Q(wrap),
                            Q(std::format("WITH c AS ({}) SELECT k, n FROM c", kBlock)),
                            Q(std::format("SELECT a, b FROM ({}) AS s(a, b)", kBlock))},
                 .check = AllEqual()});
    r.push_back({.name = "derived_table_batch_size_invariance",
                 .features = wrapped,
                 .probes = {Q(wrap, 1), Q(wrap, 7), Q(wrap)},
                 .check = AllEqual()});
    r.push_back({.name = "grouped_derived_table_sums_to_row_count",
                 .features = SetWith(wrapped, kSum),
                 .probes = {Q(std::format("SELECT SUM(n) FROM ({}) AS s", kBlock)),
                            Q("SELECT COUNT(*) FROM trips WHERE tr_row > 100")},
                 .check = AllEqual()});
    // Only a predicate on the group key may move: one on an aggregate is HAVING, not WHERE.
    r.push_back({.name = "derived_table_filter_moves_in_or_out",
                 .features = SetWith(wrapped, kWhereAnd),
                 .probes = {Q(std::format("SELECT k, n FROM ({}) AS s WHERE k > 100", kBlock)),
                            Q("SELECT tr_rider AS k, COUNT(*) AS n FROM trips WHERE tr_row > 100 "
                              "AND tr_rider > 100 GROUP BY tr_rider")},
                 .check = AllEqual()});
    r.push_back(
        {.name = "nested_derived_tables_collapse",
         .features = wrapped,
         .probes = {Q(std::string(kBlock)), Q(wrap),
                    Q(std::format("SELECT k, n FROM (SELECT k, n FROM ({}) AS s) AS u", kBlock))},
         .check = AllEqual()});
    // Sound only because the inner order is total: tr_row is unique, so the top ten are one set.
    constexpr std::string_view kTopN =
        "SELECT tr_row, tr_fare FROM trips ORDER BY tr_fare DESC NULLS LAST, tr_row LIMIT 10";
    r.push_back({.name = "derived_table_over_top_n_equals_top_n",
                 .features = {kColumns, kMultipleItems, kIntegerColumns, kDecimalColumns,
                              kTableName, kOrderBy, kNullsOrder, kLimit, kDerivedTable},
                 .probes = {Q(std::string(kTopN)),
                            Q(std::format("SELECT tr_row, tr_fare FROM ({}) AS s", kTopN))},
                 .check = AllEqual()});
    // A CTE is bound once per reference (rule 8), so two references are two derived tables, joined
    // on the equality (without it, they would be a cross product).
    r.push_back(
        {.name = "cte_referenced_twice_is_two_derived_tables",
         .features = SetWith(with_cte, kTableAlias, kQualifiedName, kCompareColumns, kCommaJoin),
         .probes = {Q(std::format("WITH p AS ({}) SELECT COUNT(*) FROM p AS a, p AS b "
                                  "WHERE a.k = b.k",
                                  kBlock)),
                    Q(std::format("SELECT COUNT(*) FROM ({}) AS a, ({}) AS b WHERE a.k = b.k",
                                  kBlock, kBlock))},
         .check = AllEqual()});
    r.push_back(
        {.name = "pruning_through_a_sub_plan",
         .features = {kCountStar, kColumns, kStar, kIntegerColumns, kDoubleColumns, kVarcharColumns,
                      kDateColumns, kDecimalColumns, kTableName, kDerivedTable},
         .probes = {Q("SELECT COUNT(*) FROM trips"),
                    Q("SELECT COUNT(*) FROM (SELECT * FROM trips) AS s"),
                    Q("SELECT COUNT(*) FROM (SELECT tr_tariff FROM trips) AS s"),
                    Q("SELECT COUNT(*) FROM (SELECT tr_row FROM trips) AS s")},
         .check = AllEqual()});
  }

  // ---- pending until uncorrelated subqueries are answered (roadmap PR J5, ADR 0022) ----
  // IN keeps a row with a match, once, so it partitions the rows its NOT IN spelling rejects, both
  // sides made NULL-free so that three-valued logic collapses: 2896 = 2577 + 319. IN is a semi
  // join, which is only the inner join when the right key is distinct -- over the unique zn_id it
  // is (2793), over the repeated rd_id it is not (2745 against the join's 2841), which is why the
  // second form distinct-ifies the right side first. A scalar subquery partitions the non-NULL
  // values of a column the same way (2903 = 2574 + 329), selects nothing when it is NULL, and
  // partitions groups from HAVING, above the aggregation (244 = 237 + 7).
  {
    const slt::FeatureSet in_sub = {kCountStar, kIntegerColumns, kTableName,      kWhere,
                                    kWhereAnd,  kIntegerLiteral, kCompareColumns, kInSubquery};
    r.push_back({.name = "in_subquery_partitions_non_null_probes",
                 .features = in_sub,
                 .probes = {Q("SELECT COUNT(*) FROM trips WHERE tr_driver > 0"),
                            Q("SELECT COUNT(*) FROM trips WHERE tr_driver > 0 AND tr_driver IN "
                              "(SELECT dv_id FROM drivers WHERE dv_id > 0)"),
                            Q("SELECT COUNT(*) FROM trips WHERE tr_driver > 0 AND tr_driver NOT IN "
                              "(SELECT dv_id FROM drivers WHERE dv_id > 0)")},
                 .check = FirstEqualsSumOfRest()});
    r.push_back({.name = "not_in_spellings_agree",
                 .features = SetWith(in_sub, kBooleanExpressions),
                 .probes = {Q("SELECT COUNT(*) FROM trips WHERE tr_driver NOT IN "
                              "(SELECT dv_id FROM drivers WHERE dv_id > 0)"),
                            Q("SELECT COUNT(*) FROM trips WHERE NOT (tr_driver IN "
                              "(SELECT dv_id FROM drivers WHERE dv_id > 0))")},
                 .check = AllEqual()});
    // zn_id is unique (tools/fixturegen/star.h), so the semi join and the inner join coincide.
    r.push_back({.name = "in_subquery_over_a_unique_key_is_a_join",
                 .features = SetWith(in_sub, kJoinOn),
                 .probes = {Q("SELECT COUNT(*) FROM trips WHERE tr_pickup IN "
                              "(SELECT zn_id FROM zones)"),
                            Q("SELECT COUNT(*) FROM trips JOIN zones ON tr_pickup = zn_id")},
                 .check = AllEqual()});
    // rd_id repeats, so the join needs the inner GROUP BY to keep each left row at most once.
    r.push_back({.name = "in_subquery_is_a_semi_join_over_distinct_keys",
                 .features = SetWith(in_sub, kJoinOn, kDerivedTable, kGroupBy, kTableAlias,
                                     kQualifiedName, kColumns),
                 .probes = {Q("SELECT COUNT(*) FROM trips WHERE tr_rider IN "
                              "(SELECT rd_id FROM riders)"),
                            Q("SELECT COUNT(*) FROM trips JOIN (SELECT rd_id FROM riders "
                              "GROUP BY rd_id) AS d ON tr_rider = d.rd_id")},
                 .check = AllEqual()});
    constexpr std::string_view kIn =
        "SELECT COUNT(*) FROM trips WHERE tr_rider IN (SELECT rd_id FROM riders)";
    constexpr std::string_view kNotIn =
        "SELECT COUNT(*) FROM trips WHERE tr_rider NOT IN (SELECT rd_id FROM riders)";
    // One query per relation: IN and NOT IN keep different rows, so they cannot share an AllEqual.
    r.push_back({.name = "in_subquery_batch_size_invariance",
                 .features = in_sub,
                 .probes = {Q(std::string(kIn), 1), Q(std::string(kIn), 7), Q(std::string(kIn))},
                 .check = AllEqual()});
    r.push_back(
        {.name = "not_in_subquery_batch_size_invariance",
         .features = in_sub,
         .probes = {Q(std::string(kNotIn), 1), Q(std::string(kNotIn), 7), Q(std::string(kNotIn))},
         .check = AllEqual()});
    const slt::FeatureSet scalar = {kCountStar, kCountColumn, kIntegerColumns, kDecimalColumns,
                                    kTableName, kWhere,       kScalarSubquery, kGroupBy};
    r.push_back({.name = "scalar_subquery_partitions_non_null_values",
                 .features = scalar,
                 .probes = {Q("SELECT COUNT(tr_fare) FROM trips"),
                            Q("SELECT COUNT(*) FROM trips WHERE tr_fare > "
                              "(SELECT COUNT(*) FROM cities)"),
                            Q("SELECT COUNT(*) FROM trips WHERE tr_fare <= "
                              "(SELECT COUNT(*) FROM cities)")},
                 .check = FirstEqualsSumOfRest()});
    // SUM over no row is NULL (shifts has no sh_hours above 100, tools/fixturegen/star.h), and
    // every comparison with NULL is NULL, so neither side keeps a row.
    r.push_back({.name = "null_scalar_subquery_selects_nothing",
                 .features = SetWith(scalar, kSum, kWhereAnd),
                 .probes = {Q("SELECT COUNT(*) FROM trips WHERE tr_fare < "
                              "(SELECT SUM(sh_hours) FROM shifts WHERE sh_hours > 100)"),
                            Q("SELECT COUNT(*) FROM trips WHERE tr_fare >= "
                              "(SELECT SUM(sh_hours) FROM shifts WHERE sh_hours > 100)")},
                 .check = AllEqual()});
    // COUNT(*) is never NULL, so every group falls on one side, the NULL grouping key included.
    r.push_back({.name = "having_scalar_subquery_partitions_groups",
                 .features = SetWith(scalar, kColumns, kHaving),
                 .probes = {Q("SELECT tr_rider FROM trips GROUP BY tr_rider"),
                            Q("SELECT tr_rider FROM trips GROUP BY tr_rider "
                              "HAVING COUNT(*) > (SELECT COUNT(*) FROM cities)"),
                            Q("SELECT tr_rider FROM trips GROUP BY tr_rider "
                              "HAVING COUNT(*) <= (SELECT COUNT(*) FROM cities)")},
                 .check = FirstIsUnionOfRest()});
  }

  // ---- left outer joins (roadmap PR J6, ADR 0022) ----
  // A LEFT JOIN's matches are the inner join's (447, and 310 and 131 with a residual on either
  // input), a strict comparison on the right input in WHERE drops every padded row so the result is
  // the inner join's, and an ON conjunct on the right input filters that input (rule 11) while one
  // on the left input does not: every left row is still there, matched or padded. Each left row is
  // padded exactly once, over a unique right key and over an empty right input (3000), and the
  // inner join below a LEFT JOIN may be reordered though the LEFT JOIN may not (2577).
  {
    constexpr std::string_view kKey = "tr_driver = sh_driver AND tr_day = sh_day";
    const slt::FeatureSet left = {kCountStar, kCountColumn, kIntegerColumns, kDateColumns,
                                  kTableName, kJoinOn,      kCompareColumns, kLeftJoin};
    const auto pair = [&](std::string_view residual) {
      return std::array{
          Q(std::format("SELECT COUNT(sh_row) FROM trips LEFT JOIN shifts ON {}{}", kKey,
                        residual)),
          Q(std::format("SELECT COUNT(*) FROM trips JOIN shifts ON {}{}", kKey, residual))};
    };
    for (const auto& [name, residual] : std::initializer_list<std::pair<const char*, const char*>>{
             {"left_join_matches_equal_inner_join", ""},
             {"left_join_matches_equal_inner_join_right_residual", " AND sh_hours > 6"},
             {"left_join_matches_equal_inner_join_left_residual", " AND tr_fare > 20"}}) {
      const auto probes = pair(residual);
      slt::FeatureSet features = left;
      features.Add(kWhere);
      features.Add(kIntegerLiteral);
      features.Add(kDoubleColumns);
      features.Add(kDecimalColumns);
      r.push_back({.name = name,
                   .features = features,
                   .probes = {probes[0], probes[1]},
                   .check = AllEqual()});
    }
    r.push_back(
        {.name = "left_join_under_null_rejecting_where_is_inner",
         .features = SetWith(left, kWhere, kDoubleColumns, kIntegerLiteral, kBooleanExpressions),
         .probes = {Q("SELECT COUNT(*) FROM trips LEFT JOIN zones ON tr_pickup = zn_id "
                      "WHERE zn_radius > 2"),
                    Q("SELECT COUNT(*) FROM trips JOIN zones ON tr_pickup = zn_id "
                      "WHERE zn_radius > 2"),
                    Q("SELECT COUNT(*) FROM trips LEFT JOIN zones ON tr_pickup = zn_id "
                      "WHERE NOT (zn_radius <= 2)"),
                    Q("SELECT COUNT(*) FROM trips JOIN zones ON tr_pickup = zn_id "
                      "WHERE NOT (zn_radius <= 2)")},
         .check = AllEqual()});
    // Rule 11, as counts: the ON conjunct decides which candidates match, so the matched pairs are
    // the inner join's over the same condition.
    r.push_back({.name = "left_join_on_right_conjunct_filters_the_right_input",
                 .features = SetWith(left, kDoubleColumns, kIntegerLiteral),
                 .probes = {Q("SELECT COUNT(zn_id) FROM trips LEFT JOIN zones "
                              "ON tr_pickup = zn_id AND zn_radius > 2"),
                            Q("SELECT COUNT(*) FROM trips JOIN zones "
                              "ON tr_pickup = zn_id AND zn_radius > 2")},
                 .check = AllEqual()});
    // The same rule as a multiset, against the filtered input written as a derived table.
    r.push_back(
        {.name = "left_join_on_right_conjunct_equals_a_filtered_input",
         .features = SetWith(left, kColumns, kMultipleItems, kStar, kDoubleColumns, kIntegerLiteral,
                             kDerivedTable, kTableAlias, kQualifiedName, kWhere),
         .probes = {Q("SELECT tr_row, zn_id FROM trips LEFT JOIN zones "
                      "ON tr_pickup = zn_id AND zn_radius > 2"),
                    Q("SELECT tr_row, z.zn_id FROM trips LEFT JOIN "
                      "(SELECT * FROM zones WHERE zn_radius > 2) AS z "
                      "ON tr_pickup = z.zn_id")},
         .check = AllEqual()});
    // zn_id is unique and promos is empty (tools/fixturegen/star.h), so every left row appears
    // once.
    r.push_back({.name = "left_join_pads_each_left_row_once",
                 .features = left,
                 .probes = {Q("SELECT COUNT(*) FROM trips"),
                            Q("SELECT COUNT(*) FROM trips LEFT JOIN zones ON tr_pickup = zn_id"),
                            Q("SELECT COUNT(*) FROM trips LEFT JOIN promos ON tr_promo = pm_id")},
                 .check = AllEqual()});
    // An ON conjunct that reads only the preserved side is a residual, not a filter: a left row
    // that fails it is padded, not dropped, so all 3000 tr_row are still present. A plan that
    // pushed `tr_fare > 20` down to trips instead would leave 897 of them -- the same answer as
    // writing that conjunct in WHERE, where it really does filter.
    r.push_back(
        {.name = "left_join_on_left_conjunct_still_pads",
         .features =
             SetWith(left, kCountDistinct, kDecimalColumns, kDoubleColumns, kIntegerLiteral),
         .probes = {Q("SELECT COUNT(DISTINCT tr_row) FROM trips"),
                    Q(std::format("SELECT COUNT(DISTINCT tr_row) FROM trips LEFT JOIN shifts ON {}",
                                  kKey)),
                    Q(std::format("SELECT COUNT(DISTINCT tr_row) FROM trips LEFT JOIN shifts "
                                  "ON {} AND tr_fare > 20",
                                  kKey)),
                    Q(std::format("SELECT COUNT(DISTINCT tr_row) FROM trips LEFT JOIN shifts "
                                  "ON {} AND sh_hours > 6",
                                  kKey))},
         .check = AllEqual()});
    constexpr std::string_view kUnder =
        "SELECT COUNT(*) FROM drivers, trips LEFT JOIN zones ON tr_pickup = zn_id "
        "WHERE tr_driver = dv_id";
    r.push_back(
        {.name = "left_join_batch_size_invariance",
         .features = SetWith(left, kCommaJoin),
         .probes = {Q(std::string(kUnder), 1), Q(std::string(kUnder), 7), Q(std::string(kUnder))},
         .check = AllEqual()});
    // Only two FROM orders are legal: a LEFT JOIN's ON may read nothing but its own two inputs, so
    // `trips, drivers LEFT JOIN zones ON tr_pickup = zn_id` exits 4 (DuckDB refuses it too).
    r.push_back({.name = "inner_join_order_below_a_left_join",
                 .features = SetWith(left, kCommaJoin),
                 .probes = {Q(std::string(kUnder)),
                            Q("SELECT COUNT(*) FROM trips LEFT JOIN zones ON tr_pickup = zn_id, "
                              "drivers WHERE tr_driver = dv_id"),
                            Q(std::string(kUnder), 1), Q(std::string(kUnder), 7)},
                 .check = AllEqual()});
  }

  // ---- full outer joins (ADR 0037's OJ3) ----
  // A FULL JOIN is symmetric: its inputs swapped give the same rows, with an ON conjunct over
  // either input too. Its rows are the LEFT JOIN's and the right input's rows that match nothing,
  // padded: as a multiset, and as counts against NOT EXISTS (3008 = 3000 + 8). A WHERE conjunct
  // that rejects the NULLs of one input leaves the outer join that preserves that input, and one
  // over both inputs leaves the inner join. Batch sizes change nothing, in a chain of two FULL
  // JOINs too.
  {
    const slt::FeatureSet full = {kColumns,        kMultipleItems,  kCountStar,
                                  kCountColumn,    kTableName,      kJoinOn,
                                  kCompareColumns, kIntegerColumns, kFullJoin};
    constexpr std::string_view kTripsDrivers = "trips FULL JOIN drivers ON tr_driver = dv_id";
    for (const auto& [name, residual] : std::initializer_list<std::pair<const char*, const char*>>{
             {"full_join_is_symmetric", ""},
             {"full_join_is_symmetric_right_residual", " AND dv_city = 2"},
             {"full_join_is_symmetric_left_residual", " AND tr_fare > 20"}}) {
      r.push_back(
          {.name = name,
           .features = SetWith(full, kWhere, kIntegerLiteral, kDecimalColumns),
           .probes = {Q(std::format("SELECT tr_row, dv_row FROM {}{}", kTripsDrivers, residual)),
                      Q(std::format("SELECT tr_row, dv_row FROM drivers FULL OUTER JOIN trips "
                                    "ON dv_id = tr_driver{}",
                                    residual))},
           .check = AllEqual()});
    }
    r.push_back(
        {.name = "full_join_is_left_join_and_unmatched_right_rows",
         .features = SetWith(full, kLeftJoin, kWhere),
         .probes = {Q(std::format("SELECT tr_row, dv_row FROM {}", kTripsDrivers)),
                    Q("SELECT tr_row, dv_row FROM trips LEFT JOIN drivers ON tr_driver = dv_id"),
                    Q("SELECT tr_row, dv_row FROM drivers LEFT JOIN trips ON dv_id = tr_driver "
                      "WHERE tr_row IS NULL")},
         .check = FirstIsUnionOfRest()});
    r.push_back({.name = "full_join_counts_left_join_and_not_exists",
                 .features = SetWith(full, kLeftJoin, kQualifiedName, kCorrelatedExists),
                 .probes = {Q(std::format("SELECT COUNT(*) FROM {}", kTripsDrivers)),
                            Q("SELECT COUNT(*) FROM trips LEFT JOIN drivers ON tr_driver = dv_id"),
                            Q("SELECT COUNT(*) FROM drivers WHERE NOT EXISTS "
                              "(SELECT * FROM trips WHERE trips.tr_driver = drivers.dv_id)")},
                 .check = FirstEqualsSumOfRest()});
    r.push_back(
        {.name = "full_join_under_null_rejecting_where_on_the_left_is_a_left_join",
         .features = SetWith(full, kLeftJoin, kWhere, kWhereAnd, kIntegerLiteral, kDecimalColumns),
         .probes = {Q(std::format("SELECT tr_row, dv_row FROM {} WHERE tr_fare > 20",
                                  kTripsDrivers)),
                    Q("SELECT tr_row, dv_row FROM trips LEFT JOIN drivers ON tr_driver = dv_id "
                      "WHERE tr_fare > 20")},
         .check = AllEqual()});
    r.push_back(
        {.name = "full_join_under_null_rejecting_where_on_the_right_is_a_left_join",
         .features = SetWith(full, kLeftJoin, kWhere, kIntegerLiteral),
         .probes = {Q(std::format("SELECT tr_row, dv_row FROM {} WHERE dv_city = 2",
                                  kTripsDrivers)),
                    Q("SELECT tr_row, dv_row FROM drivers LEFT JOIN trips ON dv_id = tr_driver "
                      "WHERE dv_city = 2")},
         .check = AllEqual()});
    r.push_back({.name = "full_join_under_where_on_both_inputs_is_inner",
                 .features = SetWith(full, kWhere, kWhereAnd, kIntegerLiteral, kDecimalColumns),
                 .probes = {Q(std::format(
                                "SELECT tr_row, dv_row FROM {} WHERE tr_fare > 20 AND dv_city = 2",
                                kTripsDrivers)),
                            Q("SELECT tr_row, dv_row FROM trips JOIN drivers ON tr_driver = dv_id "
                              "WHERE tr_fare > 20 AND dv_city = 2")},
                 .check = AllEqual()});
    constexpr std::string_view kChain =
        "SELECT COUNT(*), COUNT(zn_row), COUNT(ct_row), COUNT(dv_row) FROM zones FULL JOIN cities "
        "ON zn_city = ct_id FULL JOIN drivers ON dv_city = ct_id";
    const std::string residual =
        std::format("SELECT tr_row, dv_row FROM {} AND tr_fare > 20", kTripsDrivers);
    r.push_back({.name = "full_join_batch_size_invariance",
                 .features = SetWith(full, kWhere, kIntegerLiteral, kDecimalColumns),
                 .probes = {Q(residual, 1), Q(residual, 7), Q(residual)},
                 .check = AllEqual()});
    r.push_back(
        {.name = "full_join_chain_batch_size_invariance",
         .features = full,
         .probes = {Q(std::string(kChain), 1), Q(std::string(kChain), 7), Q(std::string(kChain))},
         .check = AllEqual()});
  }

  // ---- cross products and joins without an equality key (ADR 0040) ----
  // A cross product's rows are its inputs' rows multiplied (6 x 9); an inequality, its negation and
  // the NULL cases partition a product; swapping the two sides changes nothing; a nested loop on
  // `<=` and `>=` gives the hash join's rows on `=`; and neither the batch size nor the threads
  // change a nested loop's rows.
  {
    const slt::FeatureSet cross = {kColumns,        kMultipleItems, kCountStar,
                                   kIntegerColumns, kTableName,     kCommaJoin,
                                   kCrossJoin,      kWhere,         kCompareColumns};
    r.push_back({.name = "cross_product_counts_multiply",
                 .features = SetWith(cross, kDerivedTable, kTableAlias, kQualifiedName, kArithmetic,
                                     kVarcharColumns, kDoubleColumns, kDecimalColumns),
                 .probes = {Q("SELECT COUNT(*) FROM cities, tariffs"),
                            Q("SELECT a.n * b.m FROM (SELECT COUNT(*) AS n FROM cities) AS a, "
                              "(SELECT COUNT(*) AS m FROM tariffs) AS b")},
                 .check = AllEqual()});
    r.push_back({.name = "an_inequality_partitions_a_cross_product",
                 .features = SetWith(cross, kBooleanExpressions),
                 .probes = {Q("SELECT COUNT(*) FROM zones, cities"),
                            Q("SELECT COUNT(*) FROM zones, cities WHERE zn_city < ct_id"),
                            Q("SELECT COUNT(*) FROM zones, cities WHERE zn_city >= ct_id"),
                            Q("SELECT COUNT(*) FROM zones, cities "
                              "WHERE zn_city IS NULL OR ct_id IS NULL")},
                 .check = FirstEqualsSumOfRest()});
    r.push_back(
        {.name = "a_nested_loop_is_symmetric",
         .features = SetWith(cross, kDecimalColumns, kDoubleColumns),
         .probes = {Q("SELECT zn_id, tf_code FROM zones, tariffs WHERE tf_rate < zn_radius"),
                    Q("SELECT zn_id, tf_code FROM tariffs, zones WHERE tf_rate < zn_radius")},
         .check = AllEqual()});
    r.push_back({.name = "a_nested_loop_on_two_inequalities_equals_the_hash_join",
                 .features = SetWith(cross, kJoinOn, kWhereAnd),
                 .probes = {Q("SELECT tr_row, zn_id FROM trips JOIN zones ON tr_pickup = zn_id"),
                            Q("SELECT tr_row, zn_id FROM trips, zones "
                              "WHERE tr_pickup <= zn_id AND tr_pickup >= zn_id")},
                 .check = AllEqual()});
    constexpr std::string_view kLoop =
        "SELECT dv_id, ct_id FROM drivers, cities WHERE dv_city BETWEEN ct_id - 1 AND ct_id + 1";
    r.push_back(
        {.name = "nested_loop_batch_size_invariance",
         .features = SetWith(cross, kBetween, kArithmetic, kIntegerLiteral),
         .probes = {Q(std::string(kLoop), 1), Q(std::string(kLoop), 7), Q(std::string(kLoop))},
         .check = AllEqual()});
  }

  // ---- LEFT and FULL JOINs without an equality key (ADR 0040's OJ4c) ----
  // A keyless LEFT JOIN keeps every left row and matches the inner join's pairs; a keyless FULL
  // JOIN is symmetric and adds to the LEFT JOIN's rows the right rows that match none; neither
  // depends on the batch size.
  {
    const slt::FeatureSet keyless = {kColumns,   kMultipleItems,  kCountStar,      kCountColumn,
                                     kTableName, kCompareColumns, kIntegerColumns, kCrossJoin};
    constexpr std::string_view kOn = "ON zn_city < ct_id";
    r.push_back(
        {.name = "keyless_left_join_keeps_every_left_row",
         .features = SetWith(keyless, kLeftJoin, kCountDistinct),
         .probes = {Q("SELECT COUNT(*) FROM zones"),
                    Q(std::format("SELECT COUNT(DISTINCT zn_id) FROM zones LEFT JOIN cities {}",
                                  kOn))},
         .check = AllEqual()});
    r.push_back(
        {.name = "keyless_left_join_matches_equal_the_inner_join",
         .features = SetWith(keyless, kLeftJoin, kJoinOn),
         .probes = {Q(std::format("SELECT COUNT(ct_id) FROM zones LEFT JOIN cities {}", kOn)),
                    Q(std::format("SELECT COUNT(*) FROM zones JOIN cities {}", kOn))},
         .check = AllEqual()});
    r.push_back(
        {.name = "keyless_full_join_is_symmetric",
         .features = SetWith(keyless, kFullJoin),
         .probes = {Q("SELECT zn_id, ct_id FROM zones FULL JOIN cities ON zn_city > ct_id"),
                    Q("SELECT zn_id, ct_id FROM cities FULL JOIN zones ON zn_city > ct_id")},
         .check = AllEqual()});
    r.push_back({.name = "keyless_full_join_is_left_join_and_unmatched_right_rows",
                 .features = SetWith(keyless, kLeftJoin, kFullJoin, kWhere),
                 .probes = {Q("SELECT zn_id, ct_id FROM zones FULL JOIN cities ON zn_city > ct_id"),
                            Q("SELECT zn_id, ct_id FROM zones LEFT JOIN cities ON zn_city > ct_id"),
                            Q("SELECT zn_id, ct_id FROM cities LEFT JOIN zones ON zn_city > ct_id "
                              "WHERE zn_id IS NULL")},
                 .check = FirstIsUnionOfRest()});
    for (const std::string_view kind : {"LEFT", "FULL"}) {
      const std::string loop = std::format(
          "SELECT zn_id, ct_id FROM zones {} JOIN cities ON zn_city < ct_id AND ct_pop > 1000",
          kind);
      r.push_back({.name = std::format("keyless_{}_join_batch_size_invariance",
                                       kind == "LEFT" ? "left" : "full"),
                   .features = SetWith(keyless, kind == "LEFT" ? kLeftJoin : kFullJoin, kWhere,
                                       kIntegerLiteral),
                   .probes = {Q(loop, 1), Q(loop, 7), Q(loop)},
                   .check = AllEqual()});
    }
  }

  // ---- NULL-equal keys and IS [NOT] DISTINCT FROM (ADR 0037's OJ5b) ----
  // IS NOT DISTINCT FROM is `=` or both NULL: a join on it is the `=` join and the pairs of NULL
  // keys, and a filter on it the `=` rows and the rows NULL on both sides; IS DISTINCT FROM and IS
  // NOT DISTINCT FROM are never NULL, so they partition a cross product; a correlated EXISTS on it
  // and its NOT EXISTS partition the outer rows; and the batch size changes no NULL-equal join.
  {
    const slt::FeatureSet null_equal = {kColumns,        kMultipleItems, kCountStar,
                                        kIntegerColumns, kTableName,     kNullEqualKeys};
    r.push_back(
        {.name = "a_null_equal_join_is_the_equal_join_and_the_null_pairs",
         .features = SetWith(null_equal, kJoinOn, kCommaJoin, kCrossJoin, kWhere, kWhereAnd),
         .probes = {Q("SELECT rd_id, dv_id FROM riders JOIN drivers "
                      "ON rd_city IS NOT DISTINCT FROM dv_city"),
                    Q("SELECT rd_id, dv_id FROM riders JOIN drivers ON rd_city = dv_city"),
                    Q("SELECT rd_id, dv_id FROM riders, drivers "
                      "WHERE rd_city IS NULL AND dv_city IS NULL")},
         .check = FirstIsUnionOfRest()});
    r.push_back({.name = "not_distinct_from_is_equal_or_both_null",
                 .features = SetWith(null_equal, kWhere, kWhereAnd, kCompareColumns),
                 .probes = {Q("SELECT COUNT(*) FROM zones WHERE zn_parent IS NOT DISTINCT FROM "
                              "zn_city"),
                            Q("SELECT COUNT(*) FROM zones WHERE zn_parent = zn_city"),
                            Q("SELECT COUNT(*) FROM zones WHERE zn_parent IS NULL AND zn_city IS "
                              "NULL")},
                 .check = FirstEqualsSumOfRest()});
    r.push_back({.name = "distinct_from_partitions_a_cross_product",
                 .features = SetWith(null_equal, kCommaJoin, kCrossJoin, kWhere),
                 .probes = {Q("SELECT COUNT(*) FROM zones, cities"),
                            Q("SELECT COUNT(*) FROM zones, cities WHERE zn_city IS DISTINCT FROM "
                              "ct_id"),
                            Q("SELECT COUNT(*) FROM zones, cities WHERE zn_city IS NOT DISTINCT "
                              "FROM ct_id")},
                 .check = FirstEqualsSumOfRest()});
    r.push_back({.name = "null_equal_correlated_exists_partitions_the_outer_rows",
                 .features = SetWith(null_equal, kQualifiedName, kWhere, kWhereAnd,
                                     kCorrelatedExists, kCompareColumns, kIntegerLiteral),
                 .probes = {Q("SELECT rd_id FROM riders"),
                            Q("SELECT rd_id FROM riders WHERE EXISTS (SELECT * FROM drivers "
                              "WHERE drivers.dv_city IS NOT DISTINCT FROM riders.rd_city AND "
                              "drivers.dv_id < 1100)"),
                            Q("SELECT rd_id FROM riders WHERE NOT EXISTS (SELECT * FROM drivers "
                              "WHERE drivers.dv_city IS NOT DISTINCT FROM riders.rd_city AND "
                              "drivers.dv_id < 1100)")},
                 .check = FirstIsUnionOfRest()});
    constexpr std::string_view kJoin =
        "SELECT tr_row, tf_code FROM trips JOIN tariffs ON tr_tariff IS NOT DISTINCT FROM tf_code";
    r.push_back(
        {.name = "null_equal_join_batch_size_invariance",
         .features = SetWith(null_equal, kJoinOn, kVarcharColumns),
         .probes = {Q(std::string(kJoin), 1), Q(std::string(kJoin), 7), Q(std::string(kJoin))},
         .check = AllEqual()});
  }

  // ---- correlated subqueries (ADR 0023's U2 and U3, each declaring its own feature) ----
  // Each carries only its own Feature, so U2 activates the EXISTS family and U3 the aggregate one
  // without waiting for the other: the two PRs run side by side.
  {
    const slt::FeatureSet correlated = {kColumns,   kMultipleItems, kIntegerColumns,
                                        kTableName, kQualifiedName, kWhere};

    // EXISTS and NOT EXISTS partition the outer rows: both are true or false and never NULL, so
    // every outer row is in exactly one of them. The second pair reverses the key's sides and the
    // third adds an inner-only conjunct, which changes which rows match but not the partition.
    constexpr std::string_view kAll = "SELECT tr_row FROM trips";
    r.push_back({.name = "correlated_exists_partitions_the_outer_rows",
                 .features = SetWith(correlated, kCorrelatedExists),
                 .probes = {Q(std::string(kAll)),
                            Q("SELECT tr_row FROM trips WHERE EXISTS (SELECT * FROM riders "
                              "WHERE riders.rd_id = trips.tr_rider)"),
                            Q("SELECT tr_row FROM trips WHERE NOT EXISTS (SELECT * FROM riders "
                              "WHERE riders.rd_id = trips.tr_rider)")},
                 .check = FirstIsUnionOfRest()});
    r.push_back(
        {.name = "correlated_exists_partitions_with_an_inner_conjunct",
         .features = SetWith(correlated, kCorrelatedExists, kDateColumns),
         .probes =
             {Q(std::string(kAll)),
              Q("SELECT tr_row FROM trips WHERE EXISTS (SELECT * FROM shifts "
                "WHERE shifts.sh_driver = trips.tr_driver AND shifts.sh_day > '2024-01-10')"),
              Q("SELECT tr_row FROM trips WHERE NOT EXISTS (SELECT * FROM shifts "
                "WHERE shifts.sh_driver = trips.tr_driver AND shifts.sh_day > '2024-01-10')")},
         .check = FirstIsUnionOfRest()});

    // A correlated scalar aggregate equals its hand-written rewrite: the subquery grouped by the
    // key and joined back, which is exactly what ADR 0023's Path 2 does. The inner join drops an
    // outer row with no group, and the comparison rejects it too, since MIN over no rows is NULL.
    r.push_back(
        {.name = "correlated_aggregate_equals_its_group_by_rewrite",
         .features =
             SetWith(correlated, kCorrelatedAggregate, kMin, kJoinOn, kGroupBy, kTableAlias),
         .probes = {Q("SELECT tr_row FROM trips WHERE 2 < (SELECT MIN(riders.rd_row) FROM riders "
                      "WHERE riders.rd_id = trips.tr_rider)"),
                    Q("SELECT tr_row FROM trips JOIN (SELECT rd_id AS k, MIN(rd_row) AS m "
                      "FROM riders GROUP BY rd_id) AS g ON g.k = trips.tr_rider WHERE 2 < g.m")},
         .check = AllEqual()});
  }

  // ---- uncorrelated values of any shape (ADR 0041's SQ4a) ----
  {
    const slt::FeatureSet value = {kCountStar, kIntegerColumns, kTableName,
                                   kWhere,     kIntegerLiteral, kUncorrelatedValue};

    // A value of one row equals a cross join with it as a one-row derived table, and a value of no
    // row does too: its NULL rejects every row, as the empty derived table leaves none. Zone 10's
    // parent is zone 5 (tools/fixturegen/star.h).
    for (const auto& [name, where] :
         {std::pair{"one_row", "zn_id = 10"}, std::pair{"no_row", "zn_id > 1000"}}) {
      r.push_back(
          {.name = std::format("uncorrelated_value_of_{}_is_a_cross_join", name),
           .features = SetWith(value, kDerivedTable, kCommaJoin, kAlias, kTableAlias,
                               kQualifiedName, kColumns),
           .probes = {Q(std::format("SELECT COUNT(*) FROM trips WHERE tr_pickup = "
                                    "(SELECT zn_parent FROM zones WHERE {})",
                                    where)),
                      Q(std::format("SELECT COUNT(*) FROM trips, (SELECT zn_parent AS v FROM zones "
                                    "WHERE {}) AS s WHERE tr_pickup = s.v",
                                    where))},
           .check = AllEqual()});
    }
    // A subquery of at most one row equals its MIN() form: MIN of one row is its value, and of no
    // row NULL, as the value is then. The first value in order is the MIN of the non-NULL ones,
    // NULL sorting last.
    for (const auto& [name, parent] : {std::pair{"one_row", 5}, std::pair{"no_row", 100}}) {
      r.push_back({.name = std::format("value_of_at_most_{}_is_its_min", name),
                   .features = SetWith(value, kMin, kOrderBy, kLimit),
                   .probes = {Q(std::format("SELECT COUNT(*) FROM trips WHERE tr_pickup = (SELECT "
                                            "zn_id FROM zones WHERE zn_parent = {} ORDER BY zn_id "
                                            "LIMIT 1)",
                                            parent)),
                              Q(std::format("SELECT COUNT(*) FROM trips WHERE tr_pickup = (SELECT "
                                            "MIN(zn_id) FROM zones WHERE zn_parent = {})",
                                            parent))},
                   .check = AllEqual()});
    }
    // A BETWEEN with subquery bounds is its two comparisons, a subquery each.
    r.push_back(
        {.name = "between_subquery_bounds_are_two_comparisons",
         .features = SetWith(value, kBetween, kWhereAnd, kMin, kOrderBy, kLimit),
         .probes = {Q("SELECT COUNT(*) FROM trips WHERE tr_pickup BETWEEN (SELECT MIN(zn_id) "
                      "FROM zones WHERE zn_city = 2) AND (SELECT zn_id FROM zones WHERE "
                      "zn_city = 2 ORDER BY zn_id DESC LIMIT 1)"),
                    Q("SELECT COUNT(*) FROM trips WHERE tr_pickup >= (SELECT MIN(zn_id) "
                      "FROM zones WHERE zn_city = 2) AND tr_pickup <= (SELECT zn_id FROM "
                      "zones WHERE zn_city = 2 ORDER BY zn_id DESC LIMIT 1)")},
         .check = AllEqual()});
    // EXISTS and NOT EXISTS partition the rows, the one true for all and the other for none, and
    // EXISTS is `0 < COUNT(*)` of its block: over a block with a row (some zone groups hold more
    // than 7 zones) and one without (none holds 100).
    for (const auto& [name, count] : {std::pair{"rows", 7}, std::pair{"no_row", 100}}) {
      const std::string block =
          std::format("SELECT zn_city FROM zones GROUP BY zn_city HAVING COUNT(*) > {}", count);
      r.push_back(
          {.name = std::format("exists_and_not_exists_partition_over_{}", name),
           .features = SetWith(value, kColumns, kGroupBy, kHaving),
           .probes = {Q("SELECT COUNT(*) FROM cities"),
                      Q(std::format("SELECT COUNT(*) FROM cities WHERE EXISTS ({})", block)),
                      Q(std::format("SELECT COUNT(*) FROM cities WHERE NOT EXISTS ({})", block))},
           .check = FirstEqualsSumOfRest()});
      r.push_back(
          {.name = std::format("exists_is_a_positive_count_over_{}", name),
           .features = SetWith(value, kColumns, kGroupBy, kHaving, kDerivedTable, kTableAlias,
                               kLiteralFirst),
           .probes = {Q(std::format("SELECT COUNT(*) FROM cities WHERE EXISTS ({})", block)),
                      Q(std::format("SELECT COUNT(*) FROM cities WHERE 0 < (SELECT "
                                    "COUNT(*) FROM ({}) AS g)",
                                    block))},
           .check = AllEqual()});
    }
    // The single value and the EXISTS count are the same at any batch size, a part's rows and its
    // first value merged in part order.
    for (const auto& [name, sql] :
         {std::pair{"single_value",
                    "SELECT COUNT(*) FROM trips WHERE tr_fare > (SELECT tr_fare FROM trips ORDER "
                    "BY tr_fare DESC LIMIT 1 OFFSET 100)"},
          std::pair{"exists",
                    "SELECT COUNT(*) FROM trips WHERE EXISTS (SELECT tr_rider FROM trips GROUP BY "
                    "tr_rider HAVING COUNT(*) > 30)"}}) {
      r.push_back({.name = std::format("{}_batch_size_invariance", name),
                   .features = SetWith(value, kDecimalColumns, kColumns, kOrderBy, kLimit, kOffset,
                                       kGroupBy, kHaving),
                   .probes = {Q(sql, 1), Q(sql, 7), Q(sql)},
                   .check = AllEqual()});
    }
    // The self-check of ADR 0041's decision 28, which needs no DuckDB: a correlated subquery's
    // rows are those of the uncorrelated subquery with each binding of the outer table written as
    // a literal, one query per binding (cities holds the ids 1 to 6, none NULL).
    const auto per_binding = [](std::string_view correlated, std::string_view literal) {
      std::vector<Probe> probes = {Q(std::string(correlated))};
      for (int id = 1; id <= 6; ++id) {
        probes.push_back(Q(std::vformat(literal, std::make_format_args(id, id))));
      }
      return probes;
    };
    r.push_back(
        {.name = "correlated_exists_is_its_bindings_as_literals",
         .features = SetWith(value, kColumns, kQualifiedName, kWhereAnd, kCorrelatedExists),
         .probes = per_binding(
             "SELECT ct_id FROM cities WHERE EXISTS (SELECT * FROM zones WHERE zones.zn_city = "
             "cities.ct_id AND zones.zn_id > 20)",
             "SELECT ct_id FROM cities WHERE ct_id = {} AND EXISTS (SELECT * FROM zones WHERE "
             "zones.zn_city = {} AND zones.zn_id > 20)"),
         .check = FirstIsUnionOfRest()});
    r.push_back(
        {.name = "correlated_not_exists_is_its_bindings_as_literals",
         .features = SetWith(value, kColumns, kQualifiedName, kWhereAnd, kCorrelatedExists),
         .probes = per_binding(
             "SELECT ct_id FROM cities WHERE NOT EXISTS (SELECT * FROM zones WHERE zones.zn_city = "
             "cities.ct_id AND zones.zn_id > 20)",
             "SELECT ct_id FROM cities WHERE ct_id = {} AND NOT EXISTS (SELECT * FROM zones WHERE "
             "zones.zn_city = {} AND zones.zn_id > 20)"),
         .check = FirstIsUnionOfRest()});
    r.push_back(
        {.name = "correlated_aggregate_is_its_bindings_as_literals",
         .features = SetWith(value, kColumns, kQualifiedName, kWhereAnd, kCorrelatedAggregate, kSum,
                             kArithmetic),
         .probes = per_binding(
             "SELECT ct_id FROM cities WHERE ct_pop > (SELECT SUM(zones.zn_id) * 300 FROM zones "
             "WHERE zones.zn_city = cities.ct_id)",
             "SELECT ct_id FROM cities WHERE ct_id = {} AND ct_pop > (SELECT SUM(zones.zn_id) * "
             "300 FROM zones WHERE zones.zn_city = {})"),
         .check = FirstIsUnionOfRest()});
  }

  // ---- UNION ALL (ADR 0036) ----
  // A UNION ALL of two blocks has exactly their rows together, in a derived table and in a CTE,
  // whichever block comes first and at any batch size; its COUNT(*) is the sum of theirs.
  {
    const slt::FeatureSet unioned = {kColumns, kMultipleItems,  kIntegerColumns, kTableName,
                                     kWhere,   kIntegerLiteral, kDerivedTable,   kUnionAll};
    constexpr std::string_view kLow = "SELECT tr_row, tr_rider FROM trips WHERE tr_row < 100";
    constexpr std::string_view kHigh = "SELECT tr_row, tr_rider FROM trips WHERE tr_row >= 2900";
    const std::string both =
        std::format("SELECT tr_row, tr_rider FROM ({} UNION ALL {}) AS u", kLow, kHigh);
    r.push_back({.name = "union_all_has_its_blocks_rows",
                 .features = unioned,
                 .probes = {Q(both), Q(std::string(kLow)), Q(std::string(kHigh))},
                 .check = FirstIsUnionOfRest()});
    r.push_back(
        {.name = "union_all_forms_and_orders_equal",
         .features = SetWith(unioned, kCommonTableExpr),
         .probes = {Q(both),
                    Q(std::format("WITH u AS ({} UNION ALL {}) SELECT tr_row, tr_rider FROM u",
                                  kLow, kHigh)),
                    Q(std::format("SELECT tr_row, tr_rider FROM ({} UNION ALL {}) AS u", kHigh,
                                  kLow)),
                    Q(both, 1), Q(both, 7)},
         .check = AllEqual()});
    // Parentheses (ADR 0039's X4a) change nothing of a UNION ALL's rows: a run in parentheses is
    // spliced into the run around it, and a parenthesized block is the block.
    constexpr std::string_view kMid =
        "SELECT tr_row, tr_rider FROM trips WHERE tr_row >= 1000 AND tr_row < 1100";
    r.push_back(
        {.name = "union_all_parentheses_change_no_row",
         .features = SetWith(unioned, kSetOpParentheses),
         .probes =
             {Q(std::format("SELECT tr_row, tr_rider FROM ({} UNION ALL {} UNION ALL {}) AS u",
                            kLow, kMid, kHigh)),
              Q(std::format("SELECT tr_row, tr_rider FROM ({} UNION ALL ({} UNION ALL {})) AS u",
                            kLow, kMid, kHigh)),
              Q(std::format("SELECT tr_row, tr_rider FROM (({} UNION ALL {}) UNION ALL ({})) AS u",
                            kLow, kMid, kHigh)),
              Q(std::format("SELECT tr_row, tr_rider FROM ((({}) UNION ALL {}) UNION ALL {}) AS u",
                            kLow, kMid, kHigh),
                7)},
         .check = AllEqual()});
    r.push_back(
        {.name = "union_all_count_sums_its_blocks",
         .features = SetWith(unioned, kCountStar),
         .probes = {Q(std::format("SELECT COUNT(*) FROM ({} UNION ALL {}) AS u", kLow, kHigh)),
                    Q("SELECT COUNT(*) FROM trips WHERE tr_row < 100"),
                    Q("SELECT COUNT(*) FROM trips WHERE tr_row >= 2900")},
         .check = FirstEqualsSumOfRest()});

    // A condition on a block's constant tag keeps that block alone, however it is spelled, and a
    // tag no block carries keeps no row: the optimizer rules the other blocks out (ADR 0036's X7a).
    // The random differential test never tags a block with a constant, so it never rules one out.
    constexpr std::string_view kTagged =
        "SELECT tr_row, tr_rider, 'low' AS tag FROM trips WHERE tr_row < 100 UNION ALL "
        "SELECT tr_row, tr_rider, 'high' FROM trips WHERE tr_row >= 2900";
    const auto tagged = [&](std::string_view items, std::string_view where) {
      return std::format("SELECT {} FROM ({}) AS u WHERE {}", items, kTagged, where);
    };
    const slt::FeatureSet tags = SetWith(unioned, kConstant, kAlias, kStringLiteral);
    r.push_back({.name = "union_all_condition_on_a_tag_keeps_its_block",
                 .features = SetWith(tags, kIn),
                 .probes = {Q(std::string(kLow)), Q(tagged("tr_row, tr_rider", "tag = 'low'")),
                            Q(tagged("tr_row, tr_rider", "tag IN ('low', 'none')")),
                            Q(tagged("tr_row, tr_rider", "tag <> 'high'")),
                            Q(tagged("tr_row, tr_rider", "tag NOT IN ('high')")),
                            Q(tagged("tr_row, tr_rider", "tag = 'low'"), 1)},
                 .check = AllEqual()});
    r.push_back({.name = "union_all_condition_on_no_tag_keeps_no_row",
                 .features = SetWith(tags, kCountStar),
                 .probes = {Q(tagged("COUNT(*)", "tag = 'none'")),
                            Q("SELECT COUNT(*) FROM trips WHERE tr_row < 0")},
                 .check = AllEqual()});
  }

  // ---- UNION [DISTINCT] (ADR 0039's X4b) ----
  // A UNION is the distinct rows of its operands: ADR 0036's sketch, a SELECT DISTINCT over their
  // UNION ALL, which antb1 answers through another plan, gives the same rows (the in-engine oracle
  // of ADR 0039), as does any form, operand order, parenthesization and batch size; a UNION of a
  // block with itself is the block's DISTINCT. The operands overlap, so the deduplication matters.
  // INTEGER columns only: the sketch keeps the first -0.0 or NaN of any operand (divergence D29).
  {
    const slt::FeatureSet unioned = {kColumns, kMultipleItems,  kIntegerColumns, kTableName,
                                     kWhere,   kIntegerLiteral, kDerivedTable,   kUnionAll,
                                     kUnion,   kWhereAnd};
    constexpr std::string_view kLow = "SELECT tr_rider, tr_pickup FROM trips WHERE tr_row < 600";
    constexpr std::string_view kMid =
        "SELECT tr_rider, tr_pickup FROM trips WHERE tr_row >= 400 AND tr_row < 1000";
    constexpr std::string_view kHigh = "SELECT tr_rider, tr_pickup FROM trips WHERE tr_row >= 900";
    const auto over = [](std::string_view query) {
      return std::format("SELECT tr_rider, tr_pickup FROM ({}) AS u", query);
    };
    const std::string three = std::format("{} UNION {} UNION {}", kLow, kMid, kHigh);
    r.push_back({.name = "union_is_the_distinct_of_its_union_all",
                 .features = SetWith(unioned, kSelectDistinct),
                 .probes = {Q(over(three)),
                            Q(std::format("SELECT DISTINCT tr_rider, tr_pickup FROM ({} UNION ALL "
                                          "{} UNION ALL {}) AS u",
                                          kLow, kMid, kHigh))},
                 .check = AllEqual()});
    r.push_back(
        {.name = "union_forms_orders_and_parentheses_equal",
         .features = SetWith(unioned, kCommonTableExpr, kSetOpParentheses),
         .probes = {Q(over(three)),
                    Q(std::format("WITH u AS ({}) SELECT tr_rider, tr_pickup FROM u", three)),
                    Q(over(std::format("{} UNION {} UNION {}", kHigh, kLow, kMid))),
                    Q(over(std::format("{} UNION ({} UNION {})", kLow, kMid, kHigh))),
                    Q(over(std::format("({} UNION {}) UNION DISTINCT {}", kLow, kMid, kHigh))),
                    Q(over(std::format("{} UNION ({} UNION ALL {})", kLow, kMid, kHigh))),
                    Q(over(three), 1), Q(over(three), 7)},
         .check = AllEqual()});
    r.push_back(
        {.name = "union_of_a_block_with_itself_is_its_distinct",
         .features = SetWith(unioned, kSelectDistinct),
         .probes = {Q(over(std::format("{} UNION {}", kMid, kMid))),
                    Q(std::format("SELECT DISTINCT tr_rider, tr_pickup FROM ({}) AS u", kMid))},
         .check = AllEqual()});
  }

  // ---- INTERSECT [ALL] (ADR 0039's X4c) ----
  // ADR 0036's sketch, which antb1 answers through a UnionAll and a GroupAggregate, is the oracle:
  // the operands' UNION ALL with a 1-or-NULL marker per operand, grouped by every column, keeps a
  // group both markers count (INTERSECT) and as many copies as the smaller count (INTERSECT ALL,
  // compared grouped with COUNT(*)). The operands overlap; INTEGER columns only (divergence D29).
  {
    const slt::FeatureSet intersected = {kColumns,  kMultipleItems, kIntegerColumns, kTableName,
                                         kWhere,    kWhereAnd,      kIntegerLiteral, kDerivedTable,
                                         kUnionAll, kIntersect};
    constexpr std::string_view kA = "SELECT tr_pickup, tr_dropoff FROM trips WHERE tr_row < 1800";
    constexpr std::string_view kB = "SELECT tr_pickup, tr_dropoff FROM trips WHERE tr_row >= 1200";
    constexpr std::string_view kC =
        "SELECT tr_pickup, tr_dropoff FROM trips WHERE tr_row >= 600 AND tr_row < 2400";
    const auto over = [](std::string_view query) {
      return std::format("SELECT tr_pickup, tr_dropoff FROM ({}) AS u", query);
    };
    const auto marked = [&](std::string_view a, std::string_view b) {
      // Each block with its markers: a block's columns, then 1 or NULL per operand.
      const auto with = [](std::string_view block, std::string_view markers) {
        const std::size_t from = block.find(" FROM ");
        return std::format("{}, {}{}", block.substr(0, from), markers, block.substr(from));
      };
      return std::format("({} UNION ALL {})", with(a, "1 AS m0, NULL AS m1"), with(b, "NULL, 1"));
    };
    const slt::FeatureSet sketched =
        SetWith(intersected, kConstant, kAlias, kGroupBy, kHaving, kCountColumn);
    r.push_back(
        {.name = "intersect_matches_its_sketch",
         .features = sketched,
         .probes = {Q(over(std::format("{} INTERSECT {}", kA, kB))),
                    Q(std::format("SELECT tr_pickup, tr_dropoff FROM {} AS u GROUP BY tr_pickup, "
                                  "tr_dropoff HAVING COUNT(m0) > 0 AND COUNT(m1) > 0",
                                  marked(kA, kB)))},
         .check = AllEqual()});
    r.push_back(
        {.name = "intersect_all_matches_its_sketch",
         .features = SetWith(sketched, kSetOpAll, kCountStar, kCase),
         .probes = {Q(std::format("SELECT tr_pickup, tr_dropoff, COUNT(*) FROM ({} INTERSECT ALL "
                                  "{}) AS u GROUP BY tr_pickup, tr_dropoff",
                                  kA, kB)),
                    Q(std::format("SELECT tr_pickup, tr_dropoff, CASE WHEN COUNT(m0) < COUNT(m1) "
                                  "THEN COUNT(m0) ELSE COUNT(m1) END FROM {} AS u GROUP BY "
                                  "tr_pickup, tr_dropoff HAVING COUNT(m0) > 0 AND COUNT(m1) > 0",
                                  marked(kA, kB)))},
         .check = AllEqual()});
    r.push_back(
        {.name = "intersect_of_a_block_with_itself_is_its_distinct",
         .features = SetWith(intersected, kSelectDistinct),
         .probes = {Q(over(std::format("{} INTERSECT {}", kC, kC))),
                    Q(std::format("SELECT DISTINCT tr_pickup, tr_dropoff FROM ({}) AS u", kC))},
         .check = AllEqual()});
    // A ∩ (B ∪ C) = (A ∩ B) ∪ (A ∩ C), and INTERSECT binds tighter than UNION: A ∪ B ∩ C is
    // A ∪ (B ∩ C), in any form and batch size.
    r.push_back({.name = "intersect_distributes_over_union",
                 .features = SetWith(intersected, kUnion, kSetOpParentheses),
                 .probes = {Q(over(std::format("{} INTERSECT ({} UNION {})", kA, kB, kC))),
                            Q(over(std::format("({} INTERSECT {}) UNION ({} INTERSECT {})", kA, kB,
                                               kA, kC)))},
                 .check = AllEqual()});
    r.push_back({.name = "intersect_binds_tighter_than_union",
                 .features = SetWith(intersected, kUnion, kSetOpParentheses, kCommonTableExpr),
                 .probes = {Q(over(std::format("{} UNION {} INTERSECT {}", kA, kB, kC))),
                            Q(over(std::format("{} UNION ({} INTERSECT {})", kA, kB, kC))),
                            Q(std::format("WITH u AS ({} UNION {} INTERSECT {}) SELECT tr_pickup, "
                                          "tr_dropoff FROM u",
                                          kA, kB, kC)),
                            Q(over(std::format("{} INTERSECT {} UNION {}", kB, kC, kA))),
                            Q(over(std::format("{} UNION {} INTERSECT {}", kA, kB, kC)), 1),
                            Q(over(std::format("{} UNION {} INTERSECT {}", kA, kB, kC)), 7)},
                 .check = AllEqual()});
  }

  // ---- EXCEPT [ALL] (ADR 0039's X4d) ----
  // ADR 0036's marker sketch is the oracle again: a group the first marker counts and the second
  // does not (EXCEPT), and COUNT(m0) - COUNT(m1) copies where that is positive (EXCEPT ALL,
  // compared grouped with COUNT(*)). The identities of ADR 0039's Tests rebuild an operand from the
  // others.
  {
    const slt::FeatureSet excepted = {kColumns,  kMultipleItems, kIntegerColumns, kTableName,
                                      kWhere,    kWhereAnd,      kIntegerLiteral, kDerivedTable,
                                      kUnionAll, kExcept};
    constexpr std::string_view kA = "SELECT tr_pickup, tr_dropoff FROM trips WHERE tr_row < 1800";
    constexpr std::string_view kB = "SELECT tr_pickup, tr_dropoff FROM trips WHERE tr_row >= 1200";
    constexpr std::string_view kC =
        "SELECT tr_pickup, tr_dropoff FROM trips WHERE tr_row >= 600 AND tr_row < 2400";
    const auto over = [](std::string_view query) {
      return std::format("SELECT tr_pickup, tr_dropoff FROM ({}) AS u", query);
    };
    const auto marked = [](std::string_view a, std::string_view b) {
      const auto with = [](std::string_view block, std::string_view markers) {
        const std::size_t from = block.find(" FROM ");
        return std::format("{}, {}{}", block.substr(0, from), markers, block.substr(from));
      };
      return std::format("({} UNION ALL {})", with(a, "1 AS m0, NULL AS m1"), with(b, "NULL, 1"));
    };
    const slt::FeatureSet sketched =
        SetWith(excepted, kConstant, kAlias, kGroupBy, kHaving, kCountColumn);
    r.push_back(
        {.name = "except_matches_its_sketch",
         .features = sketched,
         .probes = {Q(over(std::format("{} EXCEPT {}", kA, kB))),
                    Q(std::format("SELECT tr_pickup, tr_dropoff FROM {} AS u GROUP BY tr_pickup, "
                                  "tr_dropoff HAVING COUNT(m0) > 0 AND COUNT(m1) = 0",
                                  marked(kA, kB)))},
         .check = AllEqual()});
    r.push_back(
        {.name = "except_all_matches_its_sketch",
         .features = SetWith(sketched, kSetOpAll, kCountStar, kArithmetic),
         .probes = {Q(std::format("SELECT tr_pickup, tr_dropoff, COUNT(*) FROM ({} EXCEPT ALL {}) "
                                  "AS u GROUP BY tr_pickup, tr_dropoff",
                                  kA, kC)),
                    Q(std::format(
                        "SELECT tr_pickup, tr_dropoff, COUNT(m0) - COUNT(m1) FROM {} AS u "
                        "GROUP BY tr_pickup, tr_dropoff HAVING COUNT(m0) > COUNT(m1)",
                        marked(kA, kC)))},
         .check = AllEqual()});
    // (A - B) ∪all (A ∩ B) = DISTINCT A, and (A -all B) ∪all (A ∩all B) = A as bags.
    r.push_back(
        {.name = "except_and_intersect_rebuild_the_distinct_operand",
         .features = SetWith(excepted, kIntersect, kSelectDistinct, kSetOpParentheses),
         .probes = {Q(over(
                        std::format("({} EXCEPT {}) UNION ALL ({} INTERSECT {})", kA, kB, kA, kB))),
                    Q(std::format("SELECT DISTINCT tr_pickup, tr_dropoff FROM ({}) AS u", kA))},
         .check = AllEqual()});
    r.push_back({.name = "except_all_and_intersect_all_rebuild_the_operand",
                 .features = SetWith(excepted, kIntersect, kSetOpAll, kSetOpParentheses),
                 .probes = {Q(over(std::format("({} EXCEPT ALL {}) UNION ALL ({} INTERSECT ALL {})",
                                               kA, kC, kA, kC))),
                            Q(over(kA))},
                 .check = AllEqual()});
    // (A - B) - C = A - (B ∪ C) = A - (B ∪all C), and A ∩ B = A - (A - B).
    r.push_back({.name = "except_of_two_is_except_of_their_union",
                 .features = SetWith(excepted, kUnion, kSetOpParentheses),
                 .probes = {Q(over(std::format("{} EXCEPT {} EXCEPT {}", kA, kB, kC))),
                            Q(over(std::format("{} EXCEPT ({} UNION {})", kA, kB, kC))),
                            Q(over(std::format("{} EXCEPT ({} UNION ALL {})", kA, kB, kC))),
                            Q(over(std::format("{} EXCEPT {} EXCEPT {}", kA, kB, kC)), 7)},
                 .check = AllEqual()});
    r.push_back({.name = "intersect_is_except_of_the_except",
                 .features = SetWith(excepted, kIntersect, kSetOpParentheses),
                 .probes = {Q(over(std::format("{} INTERSECT {}", kA, kB))),
                            Q(over(std::format("{} EXCEPT ({} EXCEPT {})", kA, kA, kB)))},
                 .check = AllEqual()});
    r.push_back({.name = "except_of_a_block_from_itself_is_empty",
                 .features = SetWith(excepted, kCountStar),
                 .probes = {Q(std::format("SELECT COUNT(*) FROM ({} EXCEPT {}) AS u", kC, kC)),
                            Q("SELECT COUNT(*) FROM trips WHERE tr_row < 0")},
                 .check = AllEqual()});
  }

  // ---- stars among the select items (ADR 0022 rule 5) ----
  // A star is the columns it reads, where it stands: `*` every FROM item's in FROM order, `t.*` the
  // item's, whichever name rule 1 gives it (the table's, an alias, a path's file name) and however
  // the name is spelled. A position after a star counts its columns, which the random differential
  // test never writes (its generator counts select items, not a star's columns), so these relations
  // answer it. Every ORDER BY ends in a unique column (ct_row, zn_row), for an order the answers
  // must share.
  {
    const slt::FeatureSet stars = {kColumns,        kStar,           kStarItems, kMultipleItems,
                                   kIntegerColumns, kVarcharColumns, kTableName};
    const slt::FeatureSet ordered = SetWith(stars, kOrderBy);
    // cities: ct_row, ct_id, label, ct_pop (tools/fixturegen/star.h).
    constexpr std::string_view kSpelled = "SELECT ct_pop, *, ct_id FROM cities ORDER BY ct_row";
    r.push_back(
        {.name = "star_items_are_their_columns_where_they_stand",
         .features = SetWith(ordered, kQualifiedStar, kTableAlias, kQualifiedName, kTablePath,
                             kQuotedIdentifier, kIdentifierCase, kLayout),
         .probes = {Q(std::string(kSpelled)),
                    Q("SELECT ct_pop, cities.*, ct_id FROM cities ORDER BY ct_row"),
                    Q(R"(SELECT ct_pop, "C".*, ct_id FROM cities AS c ORDER BY c.ct_row)"),
                    Q("SELECT ct_pop, CITIES . *, ct_id FROM '${FIXTURES}/star/cities.parquet' "
                      "ORDER BY ct_row"),
                    Q("SELECT ct_pop, ct_row, ct_id, label, ct_pop, ct_id FROM cities "
                      "ORDER BY ct_row"),
                    Q(std::string(kSpelled), 1)},
         .check = AllEqualInOrder()});
    // zones: zn_row, zn_id, zn_city, zn_parent, label, zn_radius, so label is column 7 after `*`.
    r.push_back({.name = "order_by_positions_after_a_star_count_its_columns",
                 .features = SetWith(ordered, kPosition, kDoubleColumns),
                 .probes = {Q("SELECT *, label FROM zones ORDER BY 7, 1"),
                            Q("SELECT *, label FROM zones ORDER BY label, zn_row"),
                            Q("SELECT *, label FROM zones ORDER BY 7, 1", 1),
                            Q("SELECT *, label FROM zones ORDER BY 7, 1", 7)},
                 .check = AllEqualInOrder()});
    r.push_back(
        {.name = "group_by_positions_after_a_star_count_its_columns",
         .features = SetWith(stars, kQualifiedStar, kGroupBy, kCountStar, kPosition),
         .probes = {Q("SELECT cities.*, COUNT(*) FROM cities GROUP BY 1, 2, 3, 4"),
                    Q("SELECT cities.*, COUNT(*) FROM cities GROUP BY ct_row, ct_id, label, "
                      "ct_pop")},
         .check = AllEqual()});
    // Over a join `*` is the items' stars in FROM order, label of each item included.
    constexpr std::string_view kJoined =
        "FROM zones AS z JOIN cities AS c ON z.zn_city = c.ct_id ORDER BY z.zn_row";
    r.push_back(
        {.name = "star_of_a_join_is_its_items_stars",
         .features =
             SetWith(ordered, kQualifiedStar, kJoinOn, kTableAlias, kQualifiedName, kDoubleColumns),
         .probes = {Q(std::format("SELECT * {}", kJoined)),
                    Q(std::format("SELECT z.*, c.* {}", kJoined)),
                    Q(std::format("SELECT z.*, ct_row, ct_id, c.label, ct_pop {}", kJoined))},
         .check = AllEqualInOrder()});
    // promos is empty, so a LEFT JOIN pads every column of its star with NULL.
    r.push_back(
        {.name = "left_join_pads_a_star_of_the_right_item",
         .features = SetWith(stars, kQualifiedStar, kLeftJoin, kTableAlias, kQualifiedName,
                             kDecimalColumns),
         .probes = {Q("SELECT t.tr_row, p.* FROM trips AS t LEFT JOIN promos AS p "
                      "ON t.tr_promo = p.pm_id"),
                    Q("SELECT t.tr_row, p.pm_row, p.pm_id, p.pm_code, p.pm_discount FROM trips "
                      "AS t LEFT JOIN promos AS p ON t.tr_promo = p.pm_id")},
         .check = AllEqual()});
  }

  // ---- the canary: a relation that stays pending for good ----
  // RelationList.PendingRelationsHoldInDuckDb validates a pending relation's SQL on DuckDB before
  // the PR that declares its features activates it, and asserts there is one to validate -- so once
  // ADR 0022's ids have all shipped it would have nothing left and fail. kWindowFunctions is the
  // one out-of-scope marker (kNeverGenerated, tests/slt/supported_features.h), so a relation
  // carrying it is pending for good and keeps that test honest. Window functions are the canary of
  // tests/slt/canary/canary_queries.sql too, for the same reason: antb1 refuses them at the OVER
  // before it resolves a name, so Evaluate reads this as pending rather than broken.
  r.push_back(
      {.name = "window_function_canary_stays_pending",
       .features = {kCountStar, kColumns, kIntegerColumns, kTableName, kDerivedTable, kOrderBy,
                    kMax, kWindowFunctions},
       .probes = {Q("SELECT COUNT(*) FROM (SELECT ROW_NUMBER() OVER () AS r FROM trips) AS s"),
                  Q("SELECT MAX(r) FROM (SELECT ROW_NUMBER() OVER (ORDER BY tr_row) AS r "
                    "FROM trips) AS s")},
       .check = AllEqual()});
  return r;
}

}  // namespace antb1::metamorphic
