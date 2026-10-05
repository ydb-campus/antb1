#pragma once

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

// The SQL feature vocabulary of the test harness, and THE declaration of what antb1 answers today.
//
// kSupportedFeatures is the single source of truth for
//   - `antb1-slt diff` (random differential test vs DuckDB, runner/difftest.h): most queries are
//     generated from the supported features only; the rest sample the full target grammar
//     (docs/sql-subset.md) to count antb1 Unsupported answers. An Unsupported answer to a query
//     that uses only supported features is a failure;
//   - tests/metamorphic: a relation whose features are all supported must hold; any other relation
//     is pending and must still get at least one Unsupported answer.
//
// A slice PR that implements a feature adds it to kSupportedFeatures in the same PR. New grammar
// gets a new Feature (above the out-of-scope marker), a name in FeatureName() and generator support
// in runner/query_gen.cc, or, while a later PR teaches the generator, a place in
// kGeneratorPending.

namespace antb1::slt {

enum class Feature : std::uint8_t {
  // SELECT list
  kCountStar,           // COUNT(*)
  kCountColumn,         // COUNT(col)
  kSum,                 // SUM(numeric col)
  kAvg,                 // AVG(numeric col)
  kMin,                 // MIN(col)
  kMax,                 // MAX(col)
  kCountDistinct,       // COUNT(DISTINCT col)
  kColumns,             // plain column references (a projection)
  kStar,                // SELECT *
  kMultipleItems,       // more than one select item
  kAlias,               // <item> [AS] alias
  kConstant,            // a literal select item (an integer, a string or a DATE)
  kArithmetic,          // + - * / // % and unary - of a column and a constant
  kStringFunctions,     // strlen(varchar) and regexp_replace(varchar, 'pattern', 'replacement')
  kCase,                // CASE WHEN <condition> THEN .. [ELSE ..] END
  kBooleanExpressions,  // OR, NOT and parenthesized AND in conditions (WHERE, HAVING, CASE WHEN)
  kTimestamps,          // toDateTime(integer), EXTRACT(field FROM ..) and date_trunc('unit', ..)
  // Column types a query reads (select list or WHERE); SELECT * reads every column
  kIntegerColumns,  // SMALLINT, INTEGER, BIGINT, USMALLINT
  kDoubleColumns,   // DOUBLE
  kVarcharColumns,  // VARCHAR (Parquet BYTE_ARRAY, with or without UTF8)
  kDateColumns,     // DATE (EventDate with the clickbench option)
  kDecimalColumns,  // DECIMAL(p,s), p <= 38: literals, keys, MIN, MAX, COUNT, SUM, AVG, + - * / //
                    // % by integer and decimal literals, comparisons with other numbers
  // FROM
  kTableName,      // a registered table
  kTablePath,      // '<file or glob>'
  kCommaJoin,      // FROM a, b and FROM a CROSS JOIN b
  kJoinOn,         // FROM a [INNER] JOIN b ON ...
  kLeftJoin,       // FROM a LEFT [OUTER] JOIN b ON ...
  kTableAlias,     // FROM t [AS] a
  kQualifiedName,  // t.x
  // WHERE
  kWhere,            // WHERE column <op> literal (=, <>, !=, <, <=, >, >=)
  kWhereAnd,         // several comparisons joined by AND
  kLike,             // VARCHAR column [NOT] LIKE 'pattern' in WHERE
  kIn,               // column [NOT] IN (literal, ...) in WHERE
  kBetween,          // column [NOT] BETWEEN literal AND literal in WHERE
  kCompareColumns,   // column <op> column in WHERE (a DECIMAL against another column of numbers)
  kLiteralFirst,     // literal <op> column
  kIntegerLiteral,   // 42
  kDecimalLiteral,   // 4.25
  kNegativeLiteral,  // -42, -4.25
  kStringLiteral,    // 'text' (also a date as 'YYYY-MM-DD' compared with a DATE column)
  kDateLiteral,      // DATE 'YYYY-MM-DD'
  kCastDate,  // CAST('YYYY-MM-DD' AS DATE) and 'YYYY-MM-DD'::DATE, wherever DATE 'YYYY-MM-DD' is
  // LIMIT
  kLimit,     // LIMIT n
  kGroupBy,   // GROUP BY on plain columns
  kPosition,  // GROUP BY or ORDER BY a position in the select list
  // ORDER BY
  kOrderBy,     // ORDER BY columns, aliases or aggregates, [ASC | DESC]
  kNullsOrder,  // NULLS FIRST / NULLS LAST
  kOffset,      // OFFSET m (with or without LIMIT)
  // HAVING
  kHaving,  // HAVING aggregate/key <op> literal [AND ...] (comparisons, LIKE, IN)
  // Lexical variants
  kKeywordCase,       // keywords in lower or mixed case
  kIdentifierCase,    // table and column names in another case than declared
  kQuotedIdentifier,  // "quoted" table and column names
  kLayout,            // newlines, tabs, -- and /* */ comments between tokens
  kSemicolon,         // a trailing ';'
  // Out-of-scope marker, the last Feature (new ones go above it): never in kSupportedFeatures and
  // never generated. The harness self-tests tag their "pending" canary query with it, so that path
  // stays tested.
  kWindowFunctions,  // f(...) OVER (...), out of scope for good
};

// The number of features: the marker is the last one (checked below FeatureName()).
inline constexpr std::size_t kFeatureCount =
    static_cast<std::size_t>(Feature::kWindowFunctions) + 1;

constexpr std::string_view FeatureName(Feature feature) {
  switch (feature) {
    case Feature::kCountStar:
      return "count_star";
    case Feature::kCountColumn:
      return "count_column";
    case Feature::kSum:
      return "sum";
    case Feature::kAvg:
      return "avg";
    case Feature::kMin:
      return "min";
    case Feature::kMax:
      return "max";
    case Feature::kCountDistinct:
      return "count_distinct";
    case Feature::kColumns:
      return "columns";
    case Feature::kStar:
      return "star";
    case Feature::kMultipleItems:
      return "multiple_items";
    case Feature::kAlias:
      return "alias";
    case Feature::kConstant:
      return "constant";
    case Feature::kArithmetic:
      return "arithmetic";
    case Feature::kStringFunctions:
      return "string_functions";
    case Feature::kCase:
      return "case";
    case Feature::kBooleanExpressions:
      return "boolean_expressions";
    case Feature::kTimestamps:
      return "timestamps";
    case Feature::kIntegerColumns:
      return "integer_columns";
    case Feature::kDoubleColumns:
      return "double_columns";
    case Feature::kVarcharColumns:
      return "varchar_columns";
    case Feature::kDateColumns:
      return "date_columns";
    case Feature::kDecimalColumns:
      return "decimal_columns";
    case Feature::kTableName:
      return "table_name";
    case Feature::kTablePath:
      return "table_path";
    case Feature::kCommaJoin:
      return "comma_join";
    case Feature::kJoinOn:
      return "join_on";
    case Feature::kLeftJoin:
      return "left_join";
    case Feature::kTableAlias:
      return "table_alias";
    case Feature::kQualifiedName:
      return "qualified_name";
    case Feature::kWhere:
      return "where";
    case Feature::kWhereAnd:
      return "where_and";
    case Feature::kLike:
      return "like";
    case Feature::kIn:
      return "in";
    case Feature::kLiteralFirst:
      return "literal_first";
    case Feature::kIntegerLiteral:
      return "integer_literal";
    case Feature::kBetween:
      return "between";
    case Feature::kCompareColumns:
      return "compare_columns";
    case Feature::kDecimalLiteral:
      return "decimal_literal";
    case Feature::kNegativeLiteral:
      return "negative_literal";
    case Feature::kStringLiteral:
      return "string_literal";
    case Feature::kDateLiteral:
      return "date_literal";
    case Feature::kCastDate:
      return "cast_date";
    case Feature::kLimit:
      return "limit";
    case Feature::kKeywordCase:
      return "keyword_case";
    case Feature::kIdentifierCase:
      return "identifier_case";
    case Feature::kQuotedIdentifier:
      return "quoted_identifier";
    case Feature::kLayout:
      return "layout";
    case Feature::kSemicolon:
      return "semicolon";
    case Feature::kGroupBy:
      return "group_by";
    case Feature::kPosition:
      return "position";
    case Feature::kOrderBy:
      return "order_by";
    case Feature::kNullsOrder:
      return "nulls_order";
    case Feature::kOffset:
      return "offset";
    case Feature::kHaving:
      return "having";
    case Feature::kWindowFunctions:
      return "window_functions";
  }
  return "?";
}

// kFeatureCount counts every Feature: no enumerator follows the marker (the switch above names
// every enumerator, -Wswitch).
static_assert(FeatureName(static_cast<Feature>(kFeatureCount)) == "?",
              "a Feature follows kWindowFunctions: declare new features above the marker");

// A set of the values 0 .. N - 1 of the feature enum F, one bit each, for any N: FeatureSet below,
// and a wider enum in tests/supported_features_test.cc. Names() spells a member with
// FeatureName(F), found by argument-dependent lookup.
template <typename F, std::size_t N>
class BasicFeatureSet {
 public:
  constexpr BasicFeatureSet() = default;
  constexpr BasicFeatureSet(std::initializer_list<F> features) {
    for (const F f : features) {
      Add(f);
    }
  }

  static constexpr BasicFeatureSet All() {
    BasicFeatureSet all;
    all.bits_.set();
    return all;
  }

  constexpr void Add(F f) { bits_.set(Index(f)); }
  constexpr void Add(BasicFeatureSet other) { bits_ |= other.bits_; }
  [[nodiscard]] constexpr bool Has(F f) const { return bits_.test(Index(f)); }
  // Whether every feature of `other` is in this set.
  [[nodiscard]] constexpr bool Contains(BasicFeatureSet other) const {
    return (other.bits_ & ~bits_).none();
  }
  [[nodiscard]] constexpr BasicFeatureSet Minus(BasicFeatureSet other) const {
    BasicFeatureSet out;
    out.bits_ = bits_ & ~other.bits_;
    return out;
  }
  [[nodiscard]] constexpr bool empty() const { return bits_.none(); }
  [[nodiscard]] constexpr std::size_t size() const { return bits_.count(); }
  friend constexpr bool operator==(BasicFeatureSet, BasicFeatureSet) = default;

  // "count_star, table_name" (declaration order); "none" for the empty set.
  [[nodiscard]] std::string Names() const {
    std::string out;
    for (std::size_t i = 0; i < N; ++i) {
      if (bits_.test(i)) {
        out += out.empty() ? "" : ", ";
        out += FeatureName(static_cast<F>(i));
      }
    }
    return out.empty() ? "none" : out;
  }

 private:
  // std::bitset::set and test throw std::out_of_range for a value of N or more.
  static constexpr std::size_t Index(F f) { return static_cast<std::size_t>(f); }

  std::bitset<N> bits_;
};

using FeatureSet = BasicFeatureSet<Feature, kFeatureCount>;

// Out-of-scope markers, valid in `-- features:` tags and never generated (see the Feature enum).
inline constexpr FeatureSet kNeverGenerated = {Feature::kWindowFunctions};

// Grammar that the parser accepts ahead of the random generator (runner/query_gen.cc), which
// learns it in a later PR (ADR 0022: T1 the joins, aliases and qualified names, T2 LEFT JOIN) and
// then removes it from this set. Valid in `-- features:` tags, and never in kSupportedFeatures
// while pending (checked below), so that no feature is declared supported before it is generated.
inline constexpr FeatureSet kGeneratorPending = {Feature::kCommaJoin, Feature::kJoinOn,
                                                 Feature::kLeftJoin, Feature::kTableAlias,
                                                 Feature::kQualifiedName};

// What antb1 answers today: the whole slice grammar of docs/sql-subset.md (global and grouped
// aggregates, projections, WHERE conjunctions of column <op> literal, ORDER BY, LIMIT and OFFSET)
// over every column type, in any case, quoting and layout. Listed feature by feature, so a Feature
// added to the vocabulary for new grammar stays unsupported until the PR that implements it
// declares it here.
inline constexpr FeatureSet kSupportedFeatures = {
    Feature::kCountStar,
    Feature::kCountColumn,
    Feature::kSum,
    Feature::kAvg,
    Feature::kMin,
    Feature::kMax,
    Feature::kCountDistinct,
    Feature::kColumns,
    Feature::kStar,
    Feature::kMultipleItems,
    Feature::kAlias,
    Feature::kConstant,
    Feature::kArithmetic,
    Feature::kStringFunctions,
    Feature::kCase,
    Feature::kBooleanExpressions,
    Feature::kTimestamps,
    Feature::kIntegerColumns,
    Feature::kDoubleColumns,
    Feature::kVarcharColumns,
    Feature::kDateColumns,
    Feature::kDecimalColumns,
    Feature::kTableName,
    Feature::kTablePath,
    Feature::kWhere,
    Feature::kWhereAnd,
    Feature::kLike,
    Feature::kIn,
    Feature::kBetween,
    Feature::kCompareColumns,
    Feature::kLiteralFirst,
    Feature::kIntegerLiteral,
    Feature::kDecimalLiteral,
    Feature::kNegativeLiteral,
    Feature::kStringLiteral,
    Feature::kDateLiteral,
    Feature::kCastDate,
    Feature::kLimit,
    Feature::kGroupBy,
    Feature::kPosition,
    Feature::kOrderBy,
    Feature::kNullsOrder,
    Feature::kOffset,
    Feature::kHaving,
    Feature::kKeywordCase,
    Feature::kIdentifierCase,
    Feature::kQuotedIdentifier,
    Feature::kLayout,
    Feature::kSemicolon,
};
static_assert(kSupportedFeatures.Minus(kNeverGenerated) == kSupportedFeatures,
              "kSupportedFeatures must not declare an out-of-scope marker (kNeverGenerated)");
static_assert(
    kSupportedFeatures.Minus(kGeneratorPending) == kSupportedFeatures,
    "kSupportedFeatures must not declare a feature the generator lacks (kGeneratorPending)");

}  // namespace antb1::slt
