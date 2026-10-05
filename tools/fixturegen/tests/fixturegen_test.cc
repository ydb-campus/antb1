#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <gtest/gtest.h>
#include <parquet/file_reader.h>
#include <parquet/metadata.h>

#include "fixtures.h"
#include "hits_schema.h"
#include "star.h"

namespace antb1::fixturegen {
namespace {

namespace fs = std::filesystem;

std::shared_ptr<arrow::Table> Hits(HitsVariant variant, Nulls nulls, int64_t first, int64_t n) {
  auto table = MakeHitsTable(variant, nulls, first, n);
  EXPECT_TRUE(table.ok()) << table.status().ToString();
  return table.ValueOrDie();
}

std::string ReadBytes(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}

TEST(SplitMix64, MatchesTheReferenceSequence) {
  SplitMix64 rng(0);
  EXPECT_EQ(rng.Next(), 0xE220A8397B1DCDAFULL);
  EXPECT_EQ(rng.Next(), 0x6E789E6AA1B965F4ULL);
  EXPECT_EQ(rng.Next(), 0x06C45D188009454FULL);
}

TEST(HitsSchema, Has105UniqueColumns) {
  const auto columns = HitsColumns();
  ASSERT_EQ(columns.size(), kHitsColumnCount);
  std::set<std::string_view> names;
  for (const auto& c : columns) {
    names.insert(c.name);
  }
  EXPECT_EQ(names.size(), kHitsColumnCount);
  EXPECT_EQ(columns[0].name, "WatchID");
  EXPECT_EQ(columns[5].name, "EventDate");
  EXPECT_EQ(columns[5].type, HitsType::kUInt16);
  EXPECT_EQ(columns[104].name, "CLID");
}

TEST(HitsSchema, VariantsDifferInNullabilityAndStringType) {
  const auto partitioned = HitsArrowSchema(HitsVariant::kPartitioned);
  const auto single = HitsArrowSchema(HitsVariant::kSingleFile);
  EXPECT_TRUE(partitioned->GetFieldByName("Title")->type()->Equals(arrow::binary()));
  EXPECT_TRUE(single->GetFieldByName("Title")->type()->Equals(arrow::utf8()));
  for (const auto& f : partitioned->fields()) {
    EXPECT_TRUE(f->nullable()) << f->name();
  }
  for (const auto& f : single->fields()) {
    EXPECT_FALSE(f->nullable()) << f->name();
  }
}

TEST(HitsTable, IsDeterministicAndIndependentOfSlicing) {
  const auto full = Hits(HitsVariant::kPartitioned, Nulls::kNone, 0, kHitsRows);
  const auto again = Hits(HitsVariant::kPartitioned, Nulls::kNone, 0, kHitsRows);
  EXPECT_TRUE(full->Equals(*again));
  const auto middle = Hits(HitsVariant::kPartitioned, Nulls::kNone, 1000, 3000);
  EXPECT_TRUE(middle->Equals(*full->Slice(1000, 3000)));
}

TEST(HitsTable, RequiredVariantHasTheSameValues) {
  const auto partitioned = Hits(HitsVariant::kPartitioned, Nulls::kNone, 0, 500);
  const auto single = Hits(HitsVariant::kSingleFile, Nulls::kNone, 0, 500);
  for (int c = 0; c < partitioned->num_columns(); ++c) {
    const auto a = partitioned->column(c)->chunk(0);
    auto b = single->column(c)->chunk(0);
    if (b->type_id() == arrow::Type::STRING) {
      b = b->View(arrow::binary()).ValueOrDie();
    }
    EXPECT_TRUE(a->Equals(*b)) << partitioned->field(c)->name();
    EXPECT_EQ(a->null_count(), 0);
  }
}

TEST(HitsTable, NullsVariantMasksTheSameValues) {
  const auto base = Hits(HitsVariant::kPartitioned, Nulls::kNone, 0, kHitsRows);
  const auto nulls = Hits(HitsVariant::kPartitioned, Nulls::kSprinkled, 0, kHitsRows);
  for (int c = 0; c < base->num_columns(); ++c) {
    const auto a = base->column(c)->chunk(0);
    const auto b = nulls->column(c)->chunk(0);
    const std::string& name = base->field(c)->name();
    EXPECT_TRUE(b->IsNull(0)) << name;
    if (name == "SocialAction" || name == "HistoryLength") {
      EXPECT_EQ(b->null_count(), kHitsRows) << name;
    } else if (name == "CounterID") {
      EXPECT_EQ(b->null_count(), 1);
    } else {
      EXPECT_GT(b->null_count(), kHitsRows / 20) << name;
      EXPECT_LT(b->null_count(), kHitsRows / 8) << name;
    }
    for (int64_t r = 0; r < kHitsRows; ++r) {
      if (b->IsValid(r)) {
        ASSERT_TRUE(a->RangeEquals(*b, r, r + 1, r)) << name << " row " << r;
      }
    }
  }
}

TEST(HitsTable, UserIdSumOverflowsInt64AndEventDateIsJuly2013) {
  const auto hits = Hits(HitsVariant::kPartitioned, Nulls::kNone, 0, kHitsRows);
  const auto& user_id =
      static_cast<const arrow::Int64Array&>(*hits->GetColumnByName("UserID")->chunk(0));
  int64_t sum = 0;
  bool overflow = false;
  int maxima = 0;
  int minima = 0;
  for (int64_t r = 0; r < user_id.length(); ++r) {
    overflow = __builtin_add_overflow(sum, user_id.Value(r), &sum) || overflow;
    maxima += user_id.Value(r) == std::numeric_limits<int64_t>::max() ? 1 : 0;
    minima += user_id.Value(r) == std::numeric_limits<int64_t>::min() ? 1 : 0;
  }
  EXPECT_TRUE(overflow);
  EXPECT_EQ(maxima, 2);
  EXPECT_EQ(minima, 1);

  const auto& date =
      static_cast<const arrow::UInt16Array&>(*hits->GetColumnByName("EventDate")->chunk(0));
  const auto* begin = date.raw_values();
  const auto [lo, hi] = std::minmax_element(begin, begin + date.length());
  EXPECT_EQ(*lo, 15'887);  // 2013-07-01
  EXPECT_EQ(*hi, 15'917);  // 2013-07-31
}

TEST(EdgeTable, HasExtremesNullsAndNoNaN) {
  auto edge = MakeEdgeTable().ValueOrDie();
  ASSERT_EQ(edge->num_rows(), 12);
  ASSERT_EQ(edge->num_columns(), 8);
  const auto& i64 = static_cast<const arrow::Int64Array&>(*edge->GetColumnByName("i64")->chunk(0));
  EXPECT_EQ(i64.Value(0), std::numeric_limits<int64_t>::min());
  EXPECT_EQ(i64.Value(1), std::numeric_limits<int64_t>::max());
  const auto& d = static_cast<const arrow::DoubleArray&>(*edge->GetColumnByName("d")->chunk(0));
  for (int64_t r = 0; r < d.length(); ++r) {
    if (d.IsValid(r)) {
      EXPECT_FALSE(std::isnan(d.Value(r))) << "row " << r;
    }
  }
  EXPECT_DOUBLE_EQ(d.Value(1), 1.0 / 3.0);
  for (int c = 1; c < edge->num_columns(); ++c) {
    EXPECT_TRUE(edge->column(c)->chunk(0)->IsNull(5)) << edge->field(c)->name();
  }
}

// ---- the star schema (star.h) ----

std::vector<StarTable> Star() {
  auto tables = MakeStarTables();
  EXPECT_TRUE(tables.ok()) << tables.status().ToString();
  return tables.ok() ? *std::move(tables) : std::vector<StarTable>{};
}

const StarTable& Named(const std::vector<StarTable>& tables, std::string_view name) {
  static const StarTable missing{.name = "missing",
                                 .prefix = "",
                                 .table = arrow::Table::MakeEmpty(arrow::schema({})).ValueOrDie(),
                                 .row_group_rows = 1};
  const auto it = std::ranges::find(tables, name, &StarTable::name);
  if (it == tables.end()) {
    ADD_FAILURE() << "no star table " << name;
    return missing;
  }
  return *it;
}

// A cell as an integer: SMALLINT, INTEGER, BIGINT, DATE (days), DECIMAL (the unscaled value).
std::optional<int64_t> IntAt(const arrow::Array& a, int64_t i) {
  if (a.IsNull(i)) {
    return std::nullopt;
  }
  switch (a.type_id()) {
    case arrow::Type::INT16:
      return static_cast<const arrow::Int16Array&>(a).Value(i);
    case arrow::Type::INT32:
      return static_cast<const arrow::Int32Array&>(a).Value(i);
    case arrow::Type::INT64:
      return static_cast<const arrow::Int64Array&>(a).Value(i);
    case arrow::Type::DATE32:
      return static_cast<const arrow::Date32Array&>(a).Value(i);
    case arrow::Type::DECIMAL128:
      return static_cast<int64_t>(
          arrow::Decimal128(static_cast<const arrow::Decimal128Array&>(a).GetValue(i)));
    default:
      ADD_FAILURE() << "not an integer-valued type: " << a.type()->ToString();
      return std::nullopt;
  }
}

// A key cell as text: an integer or a day number in decimal, a DECIMAL's unscaled value at scale 3
// (the widest star scale, so that DECIMAL(4,2) and DECIMAL(5,3) keys compare by value), a string
// as its bytes; std::nullopt for NULL.
std::optional<std::string> KeyText(const arrow::Array& a, int64_t i) {
  if (a.IsNull(i)) {
    return std::nullopt;
  }
  if (a.type_id() == arrow::Type::STRING) {
    return std::string(static_cast<const arrow::StringArray&>(a).GetView(i));
  }
  std::optional<int64_t> value = IntAt(a, i);
  if (!value.has_value()) {
    return std::nullopt;
  }
  if (a.type_id() == arrow::Type::DECIMAL128) {
    for (int scale = static_cast<const arrow::Decimal128Type&>(*a.type()).scale(); scale < 3;
         ++scale) {
      *value *= 10;
    }
  }
  return std::to_string(*value);
}

// Row by row, the key of `columns` (joined by '|'), std::nullopt where one of them is NULL.
std::vector<std::optional<std::string>> Keys(const StarTable& t,
                                             const std::vector<std::string>& columns) {
  std::vector<std::optional<std::string>> keys(static_cast<std::size_t>(t.table->num_rows()),
                                               std::string());
  for (std::size_t c = 0; c < columns.size(); ++c) {
    const auto column = t.table->GetColumnByName(columns[c]);
    if (column == nullptr || column->num_chunks() != 1) {
      ADD_FAILURE() << t.name << ": no column " << columns[c] << " in one chunk";
      return {};
    }
    for (std::size_t r = 0; r < keys.size(); ++r) {
      const auto cell = KeyText(*column->chunk(0), static_cast<int64_t>(r));
      if (!cell.has_value()) {
        keys[r] = std::nullopt;
      } else if (keys[r].has_value()) {
        keys[r] = c == 0 ? *cell : *keys[r] + "|" + *cell;
      }
    }
  }
  return keys;
}

// The integers of one column (IntAt), NULL left out.
std::vector<int64_t> Ints(const StarTable& t, const std::string& name) {
  std::vector<int64_t> values;
  const auto column = t.table->GetColumnByName(name);
  if (column == nullptr) {
    ADD_FAILURE() << t.name << ": no column " << name;
    return values;
  }
  for (const auto& chunk : column->chunks()) {
    for (int64_t i = 0; i < chunk->length(); ++i) {
      if (const auto v = IntAt(*chunk, i); v.has_value()) {
        values.push_back(*v);
      }
    }
  }
  return values;
}

int64_t CountOf(const std::vector<int64_t>& values, int64_t v) {
  return std::ranges::count(values, v);
}

TEST(StarSchema, TablesRowsAndRowGroups) {
  const auto tables = Star();
  struct Expected {
    std::string_view name;
    std::string_view prefix;
    int64_t rows = 0;
    int64_t group = 0;
  };
  constexpr auto kExpected = std::to_array<Expected>({
      {.name = "trips", .prefix = "tr_", .rows = 3000, .group = 1200},
      {.name = "riders", .prefix = "rd_", .rows = 240, .group = 64},
      {.name = "drivers", .prefix = "dv_", .rows = 97, .group = 32},
      {.name = "zones", .prefix = "zn_", .rows = 40, .group = 16},
      {.name = "cities", .prefix = "ct_", .rows = 6, .group = 4},
      {.name = "shifts", .prefix = "sh_", .rows = 500, .group = 128},
      {.name = "tariffs", .prefix = "tf_", .rows = 9, .group = 4},
      {.name = "promos", .prefix = "pm_", .rows = 0, .group = 4},
  });
  ASSERT_EQ(tables.size(), kExpected.size());
  std::set<int64_t> row_counts;
  for (std::size_t k = 0; k < tables.size(); ++k) {
    const StarTable& t = tables[k];
    const Expected& e = kExpected[k];
    EXPECT_EQ(t.name, e.name);
    EXPECT_EQ(t.prefix, e.prefix);
    EXPECT_EQ(t.table->num_rows(), e.rows) << t.name;
    EXPECT_EQ(t.row_group_rows, e.group) << t.name;
    EXPECT_TRUE(t.table->ValidateFull().ok()) << t.name;
    row_counts.insert(t.table->num_rows());
    if (e.rows > 0) {
      // Several row groups, the last one shorter.
      EXPECT_LT(t.row_group_rows, e.rows) << t.name;
      EXPECT_NE(e.rows % t.row_group_rows, 0) << t.name;
    }
    // <prefix>row numbers the rows from 0.
    const auto& first = t.table->schema()->field(0);
    EXPECT_EQ(first->name(), std::string(e.prefix) + "row");
    EXPECT_TRUE(first->type()->Equals(arrow::int32())) << t.name;
    EXPECT_FALSE(first->nullable()) << t.name;
    const auto ids = Ints(t, first->name());
    ASSERT_EQ(std::ssize(ids), e.rows) << t.name;
    for (std::size_t r = 0; r < ids.size(); ++r) {
      ASSERT_EQ(ids[r], static_cast<int64_t>(r)) << t.name;
    }
  }
  EXPECT_EQ(row_counts.size(), tables.size()) << "row counts all differ";
}

TEST(StarSchema, ColumnNamesCarryPrefixesButOne) {
  const auto tables = Star();
  std::map<std::string, std::vector<std::string>> owners;  // column name -> its tables
  for (const StarTable& t : tables) {
    for (int c = 0; c < t.table->num_columns(); ++c) {
      const auto& field = t.table->schema()->field(c);
      const std::string& name = field->name();
      EXPECT_TRUE(name == "label" || name.starts_with(t.prefix)) << t.name << "." << name;
      EXPECT_TRUE(std::ranges::all_of(name,
                                      [](char ch) {
                                        return (ch >= 'a' && ch <= 'z') ||
                                               (ch >= '0' && ch <= '9') || ch == '_';
                                      }))
          << t.name << "." << name << ": a lower-case plain identifier";
      EXPECT_EQ(field->nullable(), c > 0) << t.name << "." << name;
      owners[name].push_back(t.name);
    }
  }
  for (const auto& [name, in] : owners) {
    if (name == "label") {
      EXPECT_EQ(in, (std::vector<std::string>{"zones", "cities"}));
    } else {
      EXPECT_EQ(in.size(), 1U) << name;
    }
  }
  // engine::Session reads EventDate per session (the clickbench option); no star table has one.
  EXPECT_FALSE(owners.contains("eventdate"));
}

// The references of tests/slt/tables.txt and what they hold: NULL keys (at most one row in 8),
// keys without a match (dangling) and referenced keys without a referencing row.
TEST(StarSchema, KeysHoldNullDanglingAndDuplicateValues) {
  const auto tables = Star();
  struct Ref {
    std::string_view table;
    std::vector<std::string> columns;
    std::string_view ref_table;
    std::vector<std::string> ref_columns;
    bool nulls = true;
    bool dangling = true;
    bool unreferenced = true;
  };
  const std::vector<Ref> refs = {
      {.table = "trips", .columns = {"tr_rider"}, .ref_table = "riders", .ref_columns = {"rd_id"}},
      {.table = "trips",
       .columns = {"tr_driver"},
       .ref_table = "drivers",
       .ref_columns = {"dv_id"}},
      {.table = "trips",
       .columns = {"tr_pickup"},
       .ref_table = "zones",
       .ref_columns = {"zn_id"},
       .nulls = false,
       .unreferenced = false},
      {.table = "trips",
       .columns = {"tr_dropoff"},
       .ref_table = "zones",
       .ref_columns = {"zn_id"},
       .dangling = false,
       .unreferenced = false},
      {.table = "trips",
       .columns = {"tr_driver", "tr_day"},
       .ref_table = "shifts",
       .ref_columns = {"sh_driver", "sh_day"}},
      {.table = "trips",
       .columns = {"tr_tariff"},
       .ref_table = "tariffs",
       .ref_columns = {"tf_code"},
       .unreferenced = false},
      {.table = "trips",
       .columns = {"tr_rate"},
       .ref_table = "tariffs",
       .ref_columns = {"tf_rate"}},
      {.table = "riders",
       .columns = {"rd_city"},
       .ref_table = "cities",
       .ref_columns = {"ct_id"},
       .dangling = false},
      {.table = "drivers",
       .columns = {"dv_city"},
       .ref_table = "cities",
       .ref_columns = {"ct_id"},
       .dangling = false},
      {.table = "zones", .columns = {"zn_city"}, .ref_table = "cities", .ref_columns = {"ct_id"}},
      {.table = "zones", .columns = {"zn_parent"}, .ref_table = "zones", .ref_columns = {"zn_id"}},
      {.table = "shifts",
       .columns = {"sh_driver"},
       .ref_table = "drivers",
       .ref_columns = {"dv_id"}},
  };
  for (const Ref& ref : refs) {
    const StarTable& from = Named(tables, ref.table);
    const auto keys = Keys(from, ref.columns);
    std::set<std::string> targets;
    for (const auto& key : Keys(Named(tables, ref.ref_table), ref.ref_columns)) {
      if (key.has_value()) {
        targets.insert(*key);
      }
    }
    int64_t nulls = 0;
    int64_t dangling = 0;
    std::set<std::string> referenced;
    for (const auto& key : keys) {
      if (!key.has_value()) {
        ++nulls;
      } else if (targets.contains(*key)) {
        referenced.insert(*key);
      } else {
        ++dangling;
      }
    }
    const std::string what = std::string(ref.table) + "." + ref.columns.front();
    EXPECT_EQ(nulls > 0, ref.nulls) << what << ": " << nulls << " NULL keys";
    EXPECT_LE(8 * nulls, from.table->num_rows()) << what << ": too many NULL keys";
    EXPECT_EQ(dangling > 0, ref.dangling) << what << ": " << dangling << " dangling keys";
    EXPECT_EQ(referenced.size() < targets.size(), ref.unreferenced) << what;
  }

  // Every tr_promo value dangles: promos is empty.
  EXPECT_EQ(Named(tables, "promos").table->num_rows(), 0);
  EXPECT_EQ(Named(tables, "promos").table->num_columns(), 4);
  EXPECT_EQ(Ints(Named(tables, "trips"), "tr_promo").size(), 60U);

  // riders: a dense key 1..240 with 8 repeated keys, 2 of them across a row group boundary.
  const StarTable& riders = Named(tables, "riders");
  const auto rd_id = Ints(riders, "rd_id");
  ASSERT_EQ(rd_id.size(), 240U);
  std::vector<int64_t> repeated;
  std::vector<int64_t> missing;
  for (int64_t id = 1; id <= 240; ++id) {
    const int64_t n = CountOf(rd_id, id);
    if (n > 1) {
      repeated.push_back(id);
    } else if (n == 0) {
      missing.push_back(id);
    }
  }
  EXPECT_EQ(repeated, (std::vector<int64_t>{7, 47, 64, 87, 127, 167, 192, 207}));
  EXPECT_EQ(missing, (std::vector<int64_t>{8, 48, 65, 88, 128, 168, 193, 208}));
  int across = 0;
  for (std::size_t r = 1; r < rd_id.size(); ++r) {
    const bool boundary = std::cmp_equal(r % static_cast<std::size_t>(riders.row_group_rows), 0);
    across += boundary && rd_id[r] == rd_id[r - 1] ? 1 : 0;
  }
  EXPECT_EQ(across, 2);
  const auto tr_rider = Ints(Named(tables, "trips"), "tr_rider");
  for (const int64_t id : {237, 238, 239, 240}) {
    EXPECT_EQ(CountOf(tr_rider, id), 0) << "rider " << id << " has no trip";
  }
  EXPECT_GT(CountOf(tr_rider, 64), 0) << "a trip of a repeated rider key";
  EXPECT_GT(std::ranges::count_if(tr_rider, [](int64_t id) { return id > 240; }), 0);

  // drivers: one NULL key and one beyond INTEGER, 2^32 + 1000, alone in the last row group; it
  // wraps to 1000 in 32 bits, which 29 trips reference. Drivers 90..95 have no trip.
  const StarTable& drivers = Named(tables, "drivers");
  const auto dv_id = Ints(drivers, "dv_id");
  EXPECT_EQ(std::ssize(dv_id), drivers.table->num_rows() - 1) << "one NULL key";
  const auto beyond = std::ranges::count_if(
      dv_id, [](int64_t id) { return id > std::numeric_limits<int32_t>::max(); });
  EXPECT_EQ(beyond, 1);
  EXPECT_EQ(dv_id.back(), 4'294'967'296 + 1000);
  EXPECT_EQ(drivers.table->num_rows() % drivers.row_group_rows, 1);
  EXPECT_EQ(static_cast<uint32_t>(dv_id.back()), 1000U);
  const auto tr_driver = Ints(Named(tables, "trips"), "tr_driver");
  EXPECT_EQ(CountOf(tr_driver, 1000), 29);
  for (int64_t k = 90; k <= 95; ++k) {
    EXPECT_EQ(CountOf(tr_driver, 1000 + (37 * k)), 0) << "driver " << k << " has no trip";
    EXPECT_EQ(CountOf(dv_id, 1000 + (37 * k)), 1) << "driver " << k;
  }

  // zones and cities: the dangling values; city 6 has no reference, city 5 only from zones.
  const StarTable& zones = Named(tables, "zones");
  std::vector<int64_t> pickups = Ints(Named(tables, "trips"), "tr_pickup");
  std::erase_if(pickups, [](int64_t id) { return id <= 40; });
  std::ranges::sort(pickups);
  pickups.erase(std::ranges::unique(pickups).begin(), pickups.end());
  EXPECT_EQ(pickups, (std::vector<int64_t>{41, 42, 43}));
  EXPECT_EQ(CountOf(Ints(zones, "zn_city"), 9), 1);
  EXPECT_EQ(CountOf(Ints(zones, "zn_parent"), 77), 1);
  for (const auto& [table, column] : std::to_array<std::pair<std::string_view, std::string_view>>(
           {{"riders", "rd_city"}, {"drivers", "dv_city"}, {"zones", "zn_city"}})) {
    const auto cities = Ints(Named(tables, table), std::string(column));
    EXPECT_EQ(CountOf(cities, 6), 0) << column;
    EXPECT_EQ(CountOf(cities, 5) > 0, column == "zn_city") << column;
  }

  // shifts: exactly 2 repeated (sh_driver, sh_day) pairs, rows 498 and 499 (copies of rows 3 and
  // 4); no shift on March 31, which trips have.
  const auto shift_keys = Keys(Named(tables, "shifts"), {"sh_driver", "sh_day"});
  std::map<std::string, std::vector<std::size_t>> rows_of;
  for (std::size_t r = 0; r < shift_keys.size(); ++r) {
    if (shift_keys[r].has_value()) {
      rows_of[*shift_keys[r]].push_back(r);
    }
  }
  std::vector<std::vector<std::size_t>> repeated_pairs;
  for (const auto& [key, rows] : rows_of) {
    if (rows.size() > 1) {
      repeated_pairs.push_back(rows);
    }
  }
  std::ranges::sort(repeated_pairs);
  EXPECT_EQ(repeated_pairs, (std::vector<std::vector<std::size_t>>{{3, 498}, {4, 499}}));
  constexpr int64_t kMarch31 = 20'513 + 30;
  EXPECT_EQ(std::ranges::max(Ints(Named(tables, "shifts"), "sh_day")), kMarch31 - 1);
  EXPECT_GT(CountOf(Ints(Named(tables, "trips"), "tr_day"), kMarch31), 0);

  // tariffs: VARCHAR misses by case and trailing space; DECIMAL(4,2) misses, and 0.875
  // (DECIMAL(5,3)), which has no 2-digit twin but rounds to tr_rate's 0.88.
  std::set<std::string> codes;
  for (const auto& code : Keys(Named(tables, "tariffs"), {"tf_code"})) {
    codes.insert(code.value_or("NULL"));
  }
  EXPECT_EQ(codes, (std::set<std::string>{"STD", "NIGHT", "AIRPORT", "Std", "", "XL ", "Ж-1",
                                          "NULL", "POOL"}));
  std::set<std::string> code_misses;
  for (const auto& code : Keys(Named(tables, "trips"), {"tr_tariff"})) {
    if (code.has_value() && !codes.contains(*code)) {
      code_misses.insert(*code);
    }
  }
  EXPECT_EQ(code_misses, (std::set<std::string>{"XL", "std"}));
  const auto tr_rate = Ints(Named(tables, "trips"), "tr_rate");
  const auto tf_rate = Ints(Named(tables, "tariffs"), "tf_rate");
  std::set<int64_t> rate_misses;
  for (const int64_t rate : tr_rate) {
    if (CountOf(tf_rate, rate * 10) == 0) {
      rate_misses.insert(rate);
    }
  }
  EXPECT_EQ(rate_misses, (std::set<int64_t>{88, 225}));
  EXPECT_EQ(CountOf(tf_rate, 875), 1);
  EXPECT_EQ(CountOf(tf_rate, 2125), 1);

  // Keys of two types, and the unscaled DECIMAL ranges.
  const auto type_of = [&](std::string_view table, const std::string& column) {
    return Named(tables, table).table->schema()->GetFieldByName(column)->type()->ToString();
  };
  EXPECT_EQ(type_of("trips", "tr_driver"), "int32");
  EXPECT_EQ(type_of("drivers", "dv_id"), "int64");
  EXPECT_EQ(type_of("trips", "tr_rate"), "decimal128(4, 2)");
  EXPECT_EQ(type_of("tariffs", "tf_rate"), "decimal128(5, 3)");
  EXPECT_EQ(type_of("cities", "ct_id"), "int16");
  const auto fares = Ints(Named(tables, "trips"), "tr_fare");
  EXPECT_EQ(std::ranges::min(fares), 350);  // 3.50
  EXPECT_EQ(std::ranges::max(fares), 2750);
  const auto hours = Ints(Named(tables, "shifts"), "sh_hours");
  EXPECT_EQ(std::ranges::min(hours), 40);  // 4.0
  EXPECT_EQ(std::ranges::max(hours), 120);
}

TEST(StarSchema, DoublesAreExactEighths) {
  const auto tables = Star();
  std::vector<std::set<double>> columns;
  for (const auto& [table, column] : std::to_array<std::pair<std::string_view, std::string_view>>(
           {{"trips", "tr_distance"}, {"zones", "zn_radius"}})) {
    const auto chunked = Named(tables, table).table->GetColumnByName(std::string(column));
    ASSERT_NE(chunked, nullptr) << column;
    const auto& values = static_cast<const arrow::DoubleArray&>(*chunked->chunk(0));
    EXPECT_GT(values.null_count(), 0) << column;
    std::set<double>& distinct = columns.emplace_back();
    for (int64_t i = 0; i < values.length(); ++i) {
      if (values.IsNull(i)) {
        continue;
      }
      const double v = values.Value(i);
      EXPECT_FALSE(std::isnan(v)) << column << " row " << i;
      EXPECT_FALSE(std::signbit(v)) << column << " row " << i << ": no negative value, no -0.0";
      EXPECT_EQ(v * 8, std::floor(v * 8)) << column << " row " << i;
      distinct.insert(v);
    }
  }
  ASSERT_EQ(columns.size(), 2U);
  std::vector<double> equal;
  std::ranges::set_intersection(columns[0], columns[1], std::back_inserter(equal));
  EXPECT_EQ(equal, (std::vector<double>{0.75, 2.0, 3.25}));
}

class FixtureFilesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // ctest runs every test case in its own process, concurrently, with the build tree of the
    // preset as working directory: one directory per test case and preset (not a shared /tmp path).
    dir_ = fs::current_path() / "fixturegen_test_tmp" /
           ::testing::UnitTest::GetInstance()->current_test_info()->name();
    fs::remove_all(dir_);
    auto written = WriteAllFixtures(dir_ / "a");
    ASSERT_TRUE(written.ok()) << written.status().ToString();
    files_ = *written;
  }
  void TearDown() override { fs::remove_all(dir_); }

  fs::path dir_;
  std::vector<FixtureFile> files_;
};

TEST_F(FixtureFilesTest, AreByteIdenticalAcrossRuns) {
  ASSERT_TRUE(WriteAllFixtures(dir_ / "b").ok());
  ASSERT_EQ(files_.size(), 19U);
  for (const auto& f : files_) {
    const std::string a = ReadBytes(dir_ / "a" / f.path);
    ASSERT_FALSE(a.empty()) << f.path;
    EXPECT_EQ(a, ReadBytes(dir_ / "b" / f.path)) << f.path;
  }
}

TEST_F(FixtureFilesTest, HaveTheDeclaredLayout) {
  int64_t split_rows = 0;
  for (const auto& f : files_) {
    auto metadata =
        parquet::ParquetFileReader::OpenFile((dir_ / "a" / f.path).string())->metadata();
    EXPECT_EQ(metadata->num_rows(), f.rows) << f.path;
    EXPECT_EQ(metadata->num_row_groups(), f.row_groups) << f.path;
    const auto kv = metadata->key_value_metadata();
    EXPECT_TRUE(kv == nullptr || kv->FindKey("ARROW:schema") < 0) << f.path;
    for (int g = 0; g < metadata->num_row_groups(); ++g) {
      EXPECT_EQ(metadata->RowGroup(g)->ColumnChunk(0)->compression(), parquet::Compression::SNAPPY)
          << f.path;
    }
    if (f.path.starts_with("hits_like_split/")) {
      split_rows += f.rows;
    }
  }
  EXPECT_EQ(split_rows, kHitsRows);
  const auto hits = std::ranges::find(files_, std::string("hits_like.parquet"), &FixtureFile::path);
  ASSERT_NE(hits, files_.end());
  EXPECT_EQ(hits->rows, kHitsRows);
  EXPECT_EQ(hits->row_groups, 4);
}

TEST_F(FixtureFilesTest, CheckSchemaAcceptsPartitionedLayoutOnly) {
  for (const auto* name : {"hits_like.parquet", "hits_like_nulls.parquet", "empty.parquet",
                           "hits_like_split/part-3.parquet"}) {
    auto diffs = CheckHitsSchema((dir_ / "a" / name).string());
    ASSERT_TRUE(diffs.ok()) << diffs.status().ToString();
    EXPECT_TRUE(diffs->empty()) << name << ": " << diffs->front();
  }
  auto required = CheckHitsSchema((dir_ / "a" / "hits_like_required.parquet").string());
  ASSERT_TRUE(required.ok());
  // 105 repetition differences plus logical and converted type differences for 28 strings.
  EXPECT_EQ(required->size(), 105U + (2U * 28U));
  auto edge = CheckHitsSchema((dir_ / "a" / "edge.parquet").string());
  ASSERT_TRUE(edge.ok());
  EXPECT_EQ(edge->front(), "column count: expected 105, got 8");
  EXPECT_TRUE(CheckHitsSchema((dir_ / "missing.parquet").string()).status().IsIOError());
}

TEST_F(FixtureFilesTest, RemovesStaleStarFiles) {
  // A table an older generator wrote would be picked up by globs and the digest; other files stay.
  std::ofstream(dir_ / "a" / "star" / "stale.parquet") << "old";
  std::ofstream(dir_ / "a" / "star" / "notes.txt") << "kept";
  ASSERT_TRUE(WriteAllFixtures(dir_ / "a").ok());
  EXPECT_FALSE(fs::exists(dir_ / "a" / "star" / "stale.parquet"));
  EXPECT_TRUE(fs::exists(dir_ / "a" / "star" / "notes.txt"));
  int star_files = 0;
  for (const auto& entry : fs::directory_iterator(dir_ / "a" / "star")) {
    star_files += entry.path().extension() == ".parquet" ? 1 : 0;
  }
  EXPECT_EQ(star_files, 8);
  EXPECT_EQ(std::ranges::count_if(files_,
                                  [](const FixtureFile& f) { return f.path.starts_with("star/"); }),
            8);
}

}  // namespace
}  // namespace antb1::fixturegen
