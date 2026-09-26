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
};

inline constexpr std::size_t kFeatureCount = static_cast<std::size_t>(Feature::kSemicolon) + 1;

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

// What antb1 answers today: SELECT COUNT(*) [[AS] alias] FROM <name | 'path'> (ClickBench Q0), in
// any case, quoting and layout. The binder accepts the whole target grammar, but the executor runs
// only COUNT(*) without WHERE (from metadata) so far. Extend it in the PR that implements a
// feature.
inline constexpr FeatureSet kSupportedFeatures = {
    Feature::kCountStar,        Feature::kAlias,       Feature::kTableName,
    Feature::kTablePath,        Feature::kKeywordCase, Feature::kIdentifierCase,
    Feature::kQuotedIdentifier, Feature::kLayout,      Feature::kSemicolon,
};

}  // namespace antb1::slt
