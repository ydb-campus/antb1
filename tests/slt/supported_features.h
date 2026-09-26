#pragma once

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
// gets a new Feature, a name in FeatureName() and generator support in runner/query_gen.cc.

namespace antb1::slt {

enum class Feature : std::uint8_t {
  // SELECT list
  kCountStar,      // COUNT(*)
  kCountColumn,    // COUNT(col)
  kSum,            // SUM(numeric col)
  kAvg,            // AVG(numeric col)
  kMin,            // MIN(col)
  kMax,            // MAX(col)
  kColumns,        // plain column references (a projection)
  kStar,           // SELECT *
  kMultipleItems,  // more than one select item
  kAlias,          // <item> [AS] alias
  // Column types a query reads (select list or WHERE); SELECT * reads every column
  kIntegerColumns,  // SMALLINT, INTEGER, BIGINT, USMALLINT
  kDoubleColumns,   // DOUBLE
  kVarcharColumns,  // VARCHAR (Parquet BYTE_ARRAY, with or without UTF8)
  kDateColumns,     // DATE (EventDate with the clickbench option)
  // FROM
  kTableName,  // a registered table
  kTablePath,  // '<file or glob>'
  // WHERE
  kWhere,            // WHERE column <op> literal (=, <>, !=, <, <=, >, >=)
  kWhereAnd,         // several comparisons joined by AND
  kLiteralFirst,     // literal <op> column
  kIntegerLiteral,   // 42
  kDecimalLiteral,   // 4.25
  kNegativeLiteral,  // -42, -4.25
  kStringLiteral,    // 'text' (also a date as 'YYYY-MM-DD' compared with a DATE column)
  kDateLiteral,      // DATE 'YYYY-MM-DD'
  // LIMIT
  kLimit,  // LIMIT n
  // Lexical variants
  kKeywordCase,       // keywords in lower or mixed case
  kIdentifierCase,    // table and column names in another case than declared
  kQuotedIdentifier,  // "quoted" table and column names
  kLayout,            // newlines, tabs, -- and /* */ comments between tokens
  kSemicolon,         // a trailing ';'
  // Out-of-scope marker: never in kSupportedFeatures and never generated. The harness self-tests
  // tag
  // their "pending" canary query with it, so that path stays tested while the slice grammar is
  // complete.
  kGroupBy,  // GROUP BY (not supported)
};

inline constexpr std::size_t kFeatureCount = static_cast<std::size_t>(Feature::kGroupBy) + 1;

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
    case Feature::kColumns:
      return "columns";
    case Feature::kStar:
      return "star";
    case Feature::kMultipleItems:
      return "multiple_items";
    case Feature::kAlias:
      return "alias";
    case Feature::kIntegerColumns:
      return "integer_columns";
    case Feature::kDoubleColumns:
      return "double_columns";
    case Feature::kVarcharColumns:
      return "varchar_columns";
    case Feature::kDateColumns:
      return "date_columns";
    case Feature::kTableName:
      return "table_name";
    case Feature::kTablePath:
      return "table_path";
    case Feature::kWhere:
      return "where";
    case Feature::kWhereAnd:
      return "where_and";
    case Feature::kLiteralFirst:
      return "literal_first";
    case Feature::kIntegerLiteral:
      return "integer_literal";
    case Feature::kDecimalLiteral:
      return "decimal_literal";
    case Feature::kNegativeLiteral:
      return "negative_literal";
    case Feature::kStringLiteral:
      return "string_literal";
    case Feature::kDateLiteral:
      return "date_literal";
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
  }
  return "?";
}

class FeatureSet {
 public:
  constexpr FeatureSet() = default;
  constexpr FeatureSet(std::initializer_list<Feature> features) {
    for (const Feature f : features) {
      Add(f);
    }
  }

  static constexpr FeatureSet All() {
    FeatureSet all;
    for (std::size_t i = 0; i < kFeatureCount; ++i) {
      all.Add(static_cast<Feature>(i));
    }
    return all;
  }

  constexpr void Add(Feature f) { bits_ |= Bit(f); }
  constexpr void Add(FeatureSet other) { bits_ |= other.bits_; }
  [[nodiscard]] constexpr bool Has(Feature f) const { return (bits_ & Bit(f)) != 0; }
  // Whether every feature of `other` is in this set.
  [[nodiscard]] constexpr bool Contains(FeatureSet other) const {
    return (other.bits_ & ~bits_) == 0;
  }
  [[nodiscard]] constexpr FeatureSet Minus(FeatureSet other) const {
    FeatureSet out;
    out.bits_ = bits_ & ~other.bits_;
    return out;
  }
  [[nodiscard]] constexpr bool empty() const { return bits_ == 0; }
  [[nodiscard]] constexpr std::size_t size() const {
    std::size_t n = 0;
    for (std::size_t i = 0; i < kFeatureCount; ++i) {
      n += Has(static_cast<Feature>(i)) ? 1U : 0U;
    }
    return n;
  }
  friend constexpr bool operator==(FeatureSet, FeatureSet) = default;

  // "count_star, table_name" (declaration order); "none" for the empty set.
  [[nodiscard]] std::string Names() const {
    std::string out;
    for (std::size_t i = 0; i < kFeatureCount; ++i) {
      const auto f = static_cast<Feature>(i);
      if (Has(f)) {
        out += (out.empty() ? "" : ", ") + std::string(FeatureName(f));
      }
    }
    return out.empty() ? "none" : out;
  }

 private:
  static constexpr std::uint64_t Bit(Feature f) {
    return std::uint64_t{1} << static_cast<unsigned>(f);
  }

  std::uint64_t bits_ = 0;
};

// Out-of-scope markers: valid in `-- features:` tags, never generated (see Feature::kGroupBy).
inline constexpr FeatureSet kNeverGenerated = {Feature::kGroupBy};

// What antb1 answers today: the whole slice grammar of docs/sql-subset.md (global aggregates,
// projections, WHERE conjunctions of column <op> literal, LIMIT) over every column type, in any
// case, quoting and layout. Listed feature by feature, so a Feature added to the vocabulary for new
// grammar stays unsupported until the PR that implements it declares it here.
inline constexpr FeatureSet kSupportedFeatures = {
    Feature::kCountStar,
    Feature::kCountColumn,
    Feature::kSum,
    Feature::kAvg,
    Feature::kMin,
    Feature::kMax,
    Feature::kColumns,
    Feature::kStar,
    Feature::kMultipleItems,
    Feature::kAlias,
    Feature::kIntegerColumns,
    Feature::kDoubleColumns,
    Feature::kVarcharColumns,
    Feature::kDateColumns,
    Feature::kTableName,
    Feature::kTablePath,
    Feature::kWhere,
    Feature::kWhereAnd,
    Feature::kLiteralFirst,
    Feature::kIntegerLiteral,
    Feature::kDecimalLiteral,
    Feature::kNegativeLiteral,
    Feature::kStringLiteral,
    Feature::kDateLiteral,
    Feature::kLimit,
    Feature::kKeywordCase,
    Feature::kIdentifierCase,
    Feature::kQuotedIdentifier,
    Feature::kLayout,
    Feature::kSemicolon,
};

}  // namespace antb1::slt
