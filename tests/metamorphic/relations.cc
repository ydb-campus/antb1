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
    {.table = "empty", .path = "empty.parquet"},
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

}  // namespace

Check AllEqual() {
  return [](std::span<const ResultSet> answers) -> std::optional<std::string> {
    const auto render = [](const ResultSet& r) {
      return slt::RenderBlock(r, slt::SortMode::kRowSort, 0);
    };
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
      if (auto diff = slt::CompareBlocks(first, block, letters, slt::SortMode::kRowSort,
                                         slt::kDefaultRelTolerance)) {
        const std::size_t row = diff->first_row.value_or(0);
        return std::format("answer {} differs from answer 0: {} (row {}: {} vs {})", i,
                           diff->reason, row, row < first.size() ? first[row] : "(none)",
                           row < block.size() ? block[row] : "(none)");
      }
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
  return r;
}

}  // namespace antb1::metamorphic
