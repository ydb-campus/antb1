#include "hits_relations.h"

#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "relations.h"
#include "supported_features.h"

namespace antb1::metamorphic {
namespace {

using enum slt::Feature;

// Batch sizes of the invariance relations: odd sizes that split row groups and pages at random
// places, and the default. (Batch size 1 would take minutes on a whole partition.)
constexpr std::array<int64_t, 3> kHitsBatchSizes = {1024, 7919, kDefaultBatchSize};

Probe Q(std::string sql, int64_t batch_size = kDefaultBatchSize) {
  return Probe{.sql = std::move(sql), .batch_size = batch_size};
}

// A column of hits and a literal that splits its values (TLP-lite), with the features they need.
struct Split {
  std::string_view name;  // relation name suffix
  std::string_view column;
  std::string_view literal;
  slt::FeatureSet features;  // the column type and the literal kind
};

constexpr std::array<Split, 6> kSplits = {{
    {.name = "CounterID",
     .column = "CounterID",
     .literal = "100000",
     .features = {kIntegerColumns, kIntegerLiteral}},
    {.name = "UserID_negative_literal",
     .column = "UserID",
     .literal = "-2305843009213693952",
     .features = {kIntegerColumns, kIntegerLiteral, kNegativeLiteral}},
    {.name = "ResolutionWidth_decimal_literal",
     .column = "ResolutionWidth",
     .literal = "1279.5",
     .features = {kIntegerColumns, kDecimalLiteral}},
    {.name = "URL",
     .column = "URL",
     .literal = "'https://'",
     .features = {kVarcharColumns, kStringLiteral}},
    {.name = "EventDate_date_literal",
     .column = "EventDate",
     .literal = "DATE '2013-07-16'",
     .features = {kDateColumns, kDateLiteral}},
    {.name = "EventDate_string_literal",
     .column = "EventDate",
     .literal = "'2013-07-24'",
     .features = {kDateColumns, kStringLiteral}},
}};

}  // namespace

std::vector<Relation> HitsRelations() {
  std::vector<Relation> r;

  // ---- active: COUNT(*) (answered from the Parquet footers) ----
  r.push_back({.name = "hits_lexical_forms_equal",
               .features = {kCountStar, kTableName, kKeywordCase, kIdentifierCase,
                            kQuotedIdentifier, kLayout, kSemicolon},
               .probes = {Q("SELECT COUNT(*) FROM \"hits\""), Q("select count(*) from HITS"),
                          Q("SELECT COUNT(*) FROM \"Hits\";"),
                          Q("SELECT\n\tCOUNT ( * ) -- all rows\nFROM /* the table */ hits ;")},
               .check = AllEqual()});
  Relation count_batches{.name = "hits_batch_size_invariance_count",
                         .features = {kCountStar, kTableName, kQuotedIdentifier},
                         .probes = {},
                         .check = AllEqual()};
  for (const int64_t batch : kHitsBatchSizes) {
    count_batches.probes.push_back(Q("SELECT COUNT(*) FROM \"hits\"", batch));
  }
  r.push_back(std::move(count_batches));

  // ---- pending: need features beyond COUNT(*) (active once supported_features.h has them) ----
  for (const auto& s : kSplits) {
    slt::FeatureSet features = {kCountStar, kCountColumn, kWhere, kTableName};
    features.Add(s.features);
    // TLP-lite: the non-NULL values of a column split into two partitions (a NULL predicate
    // rejects the row, so NULLs are in neither).
    r.push_back(
        {.name = std::format("hits_tlp_lt_ge_{}", s.name),
         .features = features,
         .probes = {Q(std::format("SELECT COUNT({}) FROM hits", s.column)),
                    Q(std::format("SELECT COUNT(*) FROM hits WHERE {} < {}", s.column, s.literal)),
                    Q(std::format("SELECT COUNT(*) FROM hits WHERE {} >= {}", s.column,
                                  s.literal))},
         .check = FirstEqualsSumOfRest()});
  }
  for (const auto& [column, literal, type] :
       {std::tuple{"OS", "2", slt::FeatureSet{kIntegerColumns, kIntegerLiteral}},
        std::tuple{"Title", "''", slt::FeatureSet{kVarcharColumns, kStringLiteral}}}) {
    slt::FeatureSet features = {kCountStar, kCountColumn, kWhere, kTableName};
    features.Add(type);
    r.push_back(
        {.name = std::format("hits_tlp_eq_ne_{}", column),
         .features = features,
         .probes = {Q(std::format("SELECT COUNT({}) FROM hits", column)),
                    Q(std::format("SELECT COUNT(*) FROM hits WHERE {} = {}", column, literal)),
                    Q(std::format("SELECT COUNT(*) FROM hits WHERE {} <> {}", column, literal))},
         .check = FirstEqualsSumOfRest()});
  }
  r.push_back(
      {.name = "hits_and_is_symmetric",
       .features = {kCountStar, kWhere, kWhereAnd, kIntegerColumns, kIntegerLiteral, kTableName},
       .probes = {Q("SELECT COUNT(*) FROM hits WHERE CounterID < 100000 AND IsRefresh = 0"),
                  Q("SELECT COUNT(*) FROM hits WHERE IsRefresh = 0 AND CounterID < 100000")},
       .check = AllEqual()});
  r.push_back({.name = "hits_literal_first_is_mirrored",
               .features = {kCountStar, kWhere, kLiteralFirst, kIntegerColumns, kIntegerLiteral,
                            kTableName},
               .probes = {Q("SELECT COUNT(*) FROM hits WHERE ResolutionHeight > 900"),
                          Q("SELECT COUNT(*) FROM hits WHERE 900 < ResolutionHeight")},
               .check = AllEqual()});
  r.push_back(
      {.name = "hits_limit_returns_min_of_n_and_rows",
       .features = {kCountStar, kColumns, kIntegerColumns, kLimit, kTableName, kQuotedIdentifier},
       .probes = {Q("SELECT COUNT(*) FROM \"hits\""), Q("SELECT WatchID FROM hits LIMIT 0"),
                  Q("SELECT WatchID FROM hits LIMIT 7"), Q("SELECT WatchID FROM hits LIMIT 1000"),
                  Q("SELECT WatchID FROM hits LIMIT 7", 1024)},
       .check = RowCountsAreMinOf({0, 7, 1000, 7})});
  r.push_back(
      {.name = "hits_projection_rows_equal_count",
       .features = {kCountStar, kColumns, kWhere, kIntegerColumns, kIntegerLiteral, kTableName},
       .probes = {Q("SELECT COUNT(*) FROM hits WHERE RegionID = 1000"),
                  Q("SELECT WatchID FROM hits WHERE RegionID = 1000"),
                  Q("SELECT WatchID FROM hits WHERE RegionID = 1000", 7919)},
       .check = RowCountsEqualFirst()});
  for (const auto& [column, literal] :
       {std::pair{"ResolutionWidth", "1280"}, std::pair{"UserID", "0"}}) {
    r.push_back(
        {.name = std::format("hits_sum_is_sum_of_partitions_{}", column),
         .features = {kSum, kWhere, kIntegerColumns, kIntegerLiteral, kTableName},
         .probes = {Q(std::format("SELECT SUM({}) FROM hits", column)),
                    Q(std::format("SELECT SUM({0}) FROM hits WHERE {0} < {1}", column, literal)),
                    Q(std::format("SELECT SUM({0}) FROM hits WHERE {0} >= {1}", column, literal))},
         .check = FirstEqualsSumOfRest()});
  }
  r.push_back(
      {.name = "hits_min_is_min_of_partitions",
       .features = {kMin, kWhere, kDateColumns, kDateLiteral, kTableName},
       .probes = {Q("SELECT MIN(EventDate) FROM hits"),
                  Q("SELECT MIN(EventDate) FROM hits WHERE EventDate < DATE '2013-07-16'"),
                  Q("SELECT MIN(EventDate) FROM hits WHERE EventDate >= DATE '2013-07-16'")},
       .check = FirstEqualsMinOfRest()});
  r.push_back({.name = "hits_max_is_max_of_partitions",
               .features = {kMax, kWhere, kVarcharColumns, kStringLiteral, kTableName},
               .probes = {Q("SELECT MAX(URL) FROM hits"),
                          Q("SELECT MAX(URL) FROM hits WHERE URL < 'https://'"),
                          Q("SELECT MAX(URL) FROM hits WHERE URL >= 'https://'")},
               .check = FirstEqualsMaxOfRest()});

  const std::string aggregates =
      "SELECT COUNT(*), COUNT(Referer), SUM(RegionID), AVG(WindowClientWidth), MIN(EventDate), "
      "MAX(Title) FROM hits";
  Relation aggregate_batches{
      .name = "hits_batch_size_invariance_aggregates",
      .features = {kCountStar, kCountColumn, kSum, kAvg, kMin, kMax, kMultipleItems,
                   kIntegerColumns, kVarcharColumns, kDateColumns, kTableName},
      .probes = {},
      .check = AllEqual()};
  Relation filter_batches{
      .name = "hits_batch_size_invariance_filter",
      .features = {kCountStar, kWhere, kWhereAnd, kIntegerColumns, kIntegerLiteral, kTableName},
      .probes = {},
      .check = AllEqual()};
  for (const int64_t batch : kHitsBatchSizes) {
    aggregate_batches.probes.push_back(Q(aggregates, batch));
    filter_batches.probes.push_back(
        Q("SELECT COUNT(*) FROM hits WHERE RegionID < 1000 AND IsMobile = 1", batch));
  }
  r.push_back(std::move(aggregate_batches));
  r.push_back(std::move(filter_batches));
  return r;
}

}  // namespace antb1::metamorphic
