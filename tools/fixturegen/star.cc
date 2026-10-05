#include "star.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <arrow/api.h>

namespace antb1::fixturegen {
namespace {

constexpr int64_t kMarch2026 = 20'513;             // 2026-03-01 in days since 1970-01-01
constexpr int64_t kJanuary2025 = 20'089;           // 2025-01-01
constexpr int64_t kBeyondInteger = 4'294'968'296;  // 2^32 + 1000: 1000 in its low 32 bits

// Row i of a column: an integer (days for a DATE, the unscaled value for a DECIMAL, eighths for a
// DOUBLE) or a string; std::nullopt is NULL.
using Cell = std::optional<int64_t>;
using Text = std::optional<std::string_view>;
constexpr Cell kNull = std::nullopt;

constexpr Cell NullIf(bool null, int64_t value) { return null ? kNull : Cell(value); }

constexpr Text NullIf(bool null, std::string_view value) {
  return null ? Text(std::nullopt) : Text(value);
}

// values[k % N] for k >= 0.
template <class T, std::size_t N>
constexpr T Nth(const std::array<T, N>& values, int64_t k) {
  return values[static_cast<std::size_t>(k) % N];
}

struct StarColumn {
  std::string name;
  std::shared_ptr<arrow::DataType> type;
  std::function<Cell(int64_t)> ints;   // every type but VARCHAR
  std::function<Text(int64_t)> texts;  // VARCHAR
};

template <class Builder>
arrow::Status AppendInt(Builder& builder, int64_t value) {
  if constexpr (std::is_same_v<Builder, arrow::Int64Builder>) {
    return builder.Append(value);
  } else if constexpr (std::is_same_v<Builder, arrow::Int16Builder>) {
    return builder.Append(static_cast<int16_t>(value));
  } else if constexpr (std::is_same_v<Builder, arrow::Decimal128Builder>) {
    return builder.Append(arrow::Decimal128(value));
  } else if constexpr (std::is_same_v<Builder, arrow::DoubleBuilder>) {
    return builder.Append(static_cast<double>(value) / 8);
  } else {  // INTEGER and DATE
    return builder.Append(static_cast<int32_t>(value));
  }
}

template <class Builder>
arrow::Result<std::shared_ptr<arrow::Array>> FillInts(Builder& builder, const StarColumn& column,
                                                      int64_t rows) {
  if (!column.ints) {
    return arrow::Status::Invalid("star column ", column.name, " has no values");
  }
  ARROW_RETURN_NOT_OK(builder.Reserve(rows));
  for (int64_t i = 0; i < rows; ++i) {
    const Cell value = column.ints(i);
    ARROW_RETURN_NOT_OK(value.has_value() ? AppendInt(builder, *value) : builder.AppendNull());
  }
  return builder.Finish();
}

arrow::Result<std::shared_ptr<arrow::Array>> FillTexts(const StarColumn& column, int64_t rows) {
  if (!column.texts) {
    return arrow::Status::Invalid("star column ", column.name, " has no values");
  }
  arrow::StringBuilder builder;
  ARROW_RETURN_NOT_OK(builder.Reserve(rows));
  for (int64_t i = 0; i < rows; ++i) {
    const Text value = column.texts(i);
    ARROW_RETURN_NOT_OK(value.has_value() ? builder.Append(*value) : builder.AppendNull());
  }
  return builder.Finish();
}

arrow::Result<std::shared_ptr<arrow::Array>> ColumnArray(const StarColumn& column, int64_t rows) {
  switch (column.type->id()) {
    case arrow::Type::INT16: {
      arrow::Int16Builder builder;
      return FillInts(builder, column, rows);
    }
    case arrow::Type::INT32: {
      arrow::Int32Builder builder;
      return FillInts(builder, column, rows);
    }
    case arrow::Type::INT64: {
      arrow::Int64Builder builder;
      return FillInts(builder, column, rows);
    }
    case arrow::Type::DATE32: {
      arrow::Date32Builder builder;
      return FillInts(builder, column, rows);
    }
    case arrow::Type::DECIMAL128: {
      arrow::Decimal128Builder builder(column.type);
      return FillInts(builder, column, rows);
    }
    case arrow::Type::DOUBLE: {
      arrow::DoubleBuilder builder;
      return FillInts(builder, column, rows);
    }
    case arrow::Type::STRING:
      return FillTexts(column, rows);
    default:
      return arrow::Status::Invalid("star column ", column.name, ": no values of type ",
                                    column.type->ToString());
  }
}

// A table of `rows` rows: <prefix>row (INTEGER, not nullable, 0 to rows - 1), then `columns`.
arrow::Result<StarTable> MakeStar(std::string name, std::string prefix, int64_t rows,
                                  int64_t row_group_rows, const std::vector<StarColumn>& columns) {
  arrow::Int32Builder ids;
  ARROW_RETURN_NOT_OK(ids.Reserve(rows));
  for (int64_t i = 0; i < rows; ++i) {
    ARROW_RETURN_NOT_OK(ids.Append(static_cast<int32_t>(i)));
  }
  ARROW_ASSIGN_OR_RAISE(auto id_array, ids.Finish());
  arrow::FieldVector fields;
  arrow::ArrayVector arrays;
  fields.reserve(columns.size() + 1);
  arrays.reserve(columns.size() + 1);
  fields.push_back(arrow::field(prefix + "row", arrow::int32(), /*nullable=*/false));
  arrays.push_back(std::move(id_array));
  for (const StarColumn& column : columns) {
    ARROW_ASSIGN_OR_RAISE(auto array, ColumnArray(column, rows));
    fields.push_back(arrow::field(column.name, column.type));
    arrays.push_back(std::move(array));
  }
  auto table = arrow::Table::Make(arrow::schema(std::move(fields)), arrays, rows);
  return StarTable{.name = std::move(name),
                   .prefix = std::move(prefix),
                   .table = std::move(table),
                   .row_group_rows = row_group_rows};
}

// ---- trips: the fact table ----

// The tariff codes of trips: every code of tariffs but NULL, then two that match none.
constexpr auto kTripTariffs = std::to_array<std::string_view>(
    {"STD", "NIGHT", "AIRPORT", "Std", "", "XL ", "Ж-1", "POOL", "std", "XL"});
// DECIMAL(4,2) unscaled: 1.00 to 2.50, then 0.88, which tariffs' 0.875 rounds to.
constexpr auto kTripRates = std::to_array<int64_t>({100, 125, 150, 175, 200, 225, 250, 88});

arrow::Result<StarTable> Trips() {
  return MakeStar(
      "trips", "tr_", 3000, 1200,
      {
          {.name = "tr_rider",
           .type = arrow::int64(),
           .ints = [](int64_t i) -> Cell {
             if (i % 23 == 4) {
               return kNull;
             }
             if (i % 101 == 9) {
               return 900 + (i % 7);  // beyond the riders' range
             }
             return ((7 * i) % 236) + 1;
           }},
          {.name = "tr_driver",
           .type = arrow::int32(),
           .ints = [](int64_t i) -> Cell {
             if (i % 29 == 6) {
               return kNull;
             }
             const int64_t k = (11 * i) % 100;
             const bool shifted = k >= 90 && k <= 95;  // drivers 90..95 get no trip
             return 1000 + (37 * (shifted ? k + 10 : k));
           }},
          {.name = "tr_pickup",
           .type = arrow::int32(),
           .ints = [](int64_t i) { return Cell((i % 43) + 1); }},
          {.name = "tr_dropoff",
           .type = arrow::int32(),
           .ints = [](int64_t i) { return NullIf(i % 17 == 11, ((13 * i) % 40) + 1); }},
          {.name = "tr_day",
           .type = arrow::date32(),
           .ints = [](int64_t i) { return NullIf(i % 37 == 20, kMarch2026 + (i % 31)); }},
          {.name = "tr_tariff",
           .type = arrow::utf8(),
           .texts = [](int64_t i) { return NullIf(i % 19 == 3, Nth(kTripTariffs, 3 * i)); }},
          {.name = "tr_rate",
           .type = arrow::decimal128(4, 2),
           .ints = [](int64_t i) { return NullIf(i % 13 == 6, Nth(kTripRates, i / 7)); }},
          {.name = "tr_promo",
           .type = arrow::int32(),
           .ints = [](int64_t i) { return NullIf(i % 50 != 0, ((i / 50) % 4) + 1); }},
          {.name = "tr_fare",
           .type = arrow::decimal128(9, 2),
           .ints = [](int64_t i) { return NullIf(i % 31 == 15, 350 + (25 * ((29 * i) % 97))); }},
          {.name = "tr_distance",
           .type = arrow::float64(),
           .ints = [](int64_t i) { return NullIf(i % 41 == 30, ((5 * i) % 160) + 1); }},
      });
}

// ---- dimensions ----

constexpr int64_t kRiderGroup = 64;
constexpr auto kTiers = std::to_array<std::string_view>({"basic", "plus", "gold"});

arrow::Result<StarTable> Riders() {
  return MakeStar("riders", "rd_", 240, kRiderGroup,
                  {
                      {.name = "rd_id",
                       .type = arrow::int64(),
                       .ints =
                           [](int64_t i) {
                             // The key of the row before, twice across a row group boundary.
                             const bool repeats =
                                 i % 40 == 7 || i == kRiderGroup || i == 3 * kRiderGroup;
                             return Cell(repeats ? i : i + 1);
                           }},
                      {.name = "rd_city",
                       .type = arrow::int16(),
                       .ints = [](int64_t i) { return NullIf(i % 29 == 13, (i % 3) + 1); }},
                      {.name = "rd_tier",
                       .type = arrow::utf8(),
                       .texts = [](int64_t i) { return Text(Nth(kTiers, i / 2)); }},
                      {.name = "rd_joined",
                       .type = arrow::date32(),
                       .ints = [](int64_t i) { return Cell(kJanuary2025 + ((13 * i) % 450)); }},
                  });
}

arrow::Result<StarTable> Drivers() {
  return MakeStar("drivers", "dv_", 97, 32,
                  {
                      {.name = "dv_id",
                       .type = arrow::int64(),
                       .ints = [](int64_t i) -> Cell {
                         if (i == 50) {
                           return kNull;
                         }
                         return i == 96 ? kBeyondInteger : 1000 + (37 * i);
                       }},
                      {.name = "dv_city",
                       .type = arrow::int16(),
                       .ints = [](int64_t i) { return NullIf(i % 11 == 10, (i % 4) + 1); }},
                      {.name = "dv_since",
                       .type = arrow::date32(),
                       .ints = [](int64_t i) { return Cell(kJanuary2025 + ((11 * i) % 365)); }},
                  });
}

constexpr auto kZoneLabels = std::to_array<std::string_view>(
    {"center", "north", "south", "airport", "docks", "park", "market"});

arrow::Result<StarTable> Zones() {
  return MakeStar(
      "zones", "zn_", 40, 16,
      {
          {.name = "zn_id", .type = arrow::int32(), .ints = [](int64_t i) { return Cell(i + 1); }},
          {.name = "zn_city",
           .type = arrow::int16(),
           .ints = [](int64_t i) -> Cell {
             if (i == 37) {
               return kNull;
             }
             return i == 38 ? 9 : (i % 5) + 1;  // 9: no such city
           }},
          {.name = "zn_parent",
           .type = arrow::int32(),
           .ints = [](int64_t i) -> Cell {
             if (i < 5) {
               return kNull;
             }
             return i == 39 ? 77 : (i % 5) + 1;  // 77: no such zone
           }},
          {.name = "label",
           .type = arrow::utf8(),
           .texts = [](int64_t i) { return NullIf(i % 8 == 7, Nth(kZoneLabels, i % 8)); }},
          {.name = "zn_radius",
           .type = arrow::float64(),
           .ints = [](int64_t i) { return NullIf(i % 13 == 12, 2 * ((i % 12) + 2)); }},
      });
}

constexpr auto kCityLabels = std::to_array<std::string_view>(
    {"center", "harbor", "hills", "airport", "old town", "outskirts"});
constexpr auto kCityPopulations =
    std::to_array<Cell>({120'000, 45'000, 30'000, kNull, 8'000, 2'500});

arrow::Result<StarTable> Cities() {
  return MakeStar(
      "cities", "ct_", static_cast<int64_t>(kCityLabels.size()), 4,
      {
          {.name = "ct_id", .type = arrow::int16(), .ints = [](int64_t i) { return Cell(i + 1); }},
          {.name = "label",
           .type = arrow::utf8(),
           .texts = [](int64_t i) { return Text(Nth(kCityLabels, i)); }},
          {.name = "ct_pop",
           .type = arrow::int32(),
           .ints = [](int64_t i) { return Nth(kCityPopulations, i); }},
      });
}

// Rows 498 and 499 repeat the key columns of rows 3 and 4.
constexpr int64_t ShiftSource(int64_t i) { return i >= 498 ? i - 495 : i; }

arrow::Result<StarTable> Shifts() {
  return MakeStar("shifts", "sh_", 500, 128,
                  {
                      {.name = "sh_driver",
                       .type = arrow::int32(),
                       .ints =
                           [](int64_t i) {
                             const int64_t src = ShiftSource(i);
                             return NullIf(src % 89 == 5, 1000 + (37 * (src % 90)));
                           }},
                      {.name = "sh_day",
                       .type = arrow::date32(),
                       .ints =
                           [](int64_t i) {
                             const int64_t src = ShiftSource(i);
                             const int64_t day = ((7 * (src % 90)) + (11 * (src / 90))) % 30;
                             return NullIf(src % 97 == 13, kMarch2026 + day);
                           }},
                      {.name = "sh_hours",
                       .type = arrow::decimal128(4, 1),
                       .ints = [](int64_t i) { return NullIf(i % 23 == 7, 40 + ((5 * i) % 81)); }},
                  });
}

constexpr auto kTariffCodes =
    std::to_array<Text>({"STD", "NIGHT", "AIRPORT", "Std", "", "XL ", "Ж-1", std::nullopt, "POOL"});
// DECIMAL(5,3) unscaled.
constexpr auto kTariffRates =
    std::to_array<Cell>({1000, 1500, 2000, 1250, 2125, 2500, 1750, kNull, 875});
constexpr auto kTariffLabels = std::to_array<std::string_view>(
    {"standard", "night", "airport", "standard", "center", "xl", "center", "unknown", "pool"});

arrow::Result<StarTable> Tariffs() {
  return MakeStar("tariffs", "tf_", static_cast<int64_t>(kTariffCodes.size()), 4,
                  {
                      {.name = "tf_code",
                       .type = arrow::utf8(),
                       .texts = [](int64_t i) { return Nth(kTariffCodes, i); }},
                      {.name = "tf_rate",
                       .type = arrow::decimal128(5, 3),
                       .ints = [](int64_t i) { return Nth(kTariffRates, i); }},
                      {.name = "tf_label",
                       .type = arrow::utf8(),
                       .texts = [](int64_t i) { return Text(Nth(kTariffLabels, i)); }},
                  });
}

arrow::Result<StarTable> Promos() {
  const auto none = [](int64_t /*i*/) { return kNull; };
  return MakeStar("promos", "pm_", 0, 4,
                  {
                      {.name = "pm_id", .type = arrow::int32(), .ints = none},
                      {.name = "pm_code",
                       .type = arrow::utf8(),
                       .texts = [](int64_t /*i*/) { return Text(std::nullopt); }},
                      {.name = "pm_discount", .type = arrow::decimal128(4, 2), .ints = none},
                  });
}

}  // namespace

arrow::Result<std::vector<StarTable>> MakeStarTables() {
  using Maker = arrow::Result<StarTable> (*)();
  constexpr auto kMakers =
      std::to_array<Maker>({Trips, Riders, Drivers, Zones, Cities, Shifts, Tariffs, Promos});
  std::vector<StarTable> tables;
  tables.reserve(kMakers.size());
  for (const Maker make : kMakers) {
    ARROW_ASSIGN_OR_RAISE(auto table, make());
    tables.push_back(std::move(table));
  }
  return tables;
}

}  // namespace antb1::fixturegen
