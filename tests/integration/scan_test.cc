// io::ParquetTable::Scan over the Parquet fixtures (label integration): every table, every field,
// odd batch sizes; the hits-like variants (split into files, REQUIRED + UTF8) scan to the same
// values.

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <gtest/gtest.h>

#include "antb1/common/int128.h"
#include "antb1/io/parquet_table.h"
#include "antb1/plan/types.h"

#include "integration_util.h"

namespace antb1::integration {
namespace {

std::shared_ptr<io::ParquetTable> OpenFixture(std::string_view file, bool clickbench = false) {
  io::ParquetTableOptions options;
  if (clickbench) {
    options.overrides.push_back({.column = "EventDate", .type = plan::LogicalType::kDate});
  }
  auto table = io::ParquetTable::Open({Fixture(file)}, options);
  EXPECT_TRUE(table.ok()) << file << ": " << table.status().ToString();
  return table.ok() ? *table : nullptr;
}

std::vector<int> AllFields(const plan::Table& table) {
  std::vector<int> fields(static_cast<std::size_t>(table.schema()->num_fields()));
  std::ranges::iota(fields, 0);
  return fields;
}

// Every batch of a scan, checked; nullptr on an error.
std::shared_ptr<arrow::Table> ScanAll(const plan::Table& table, const std::vector<int>& fields,
                                      int64_t batch_size) {
  auto reader = table.Scan(fields, batch_size);
  EXPECT_TRUE(reader.ok()) << reader.status().ToString();
  if (!reader.ok()) {
    return nullptr;
  }
  std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
  while (true) {
    std::shared_ptr<arrow::RecordBatch> batch;
    const arrow::Status st = (*reader)->ReadNext(&batch);
    EXPECT_TRUE(st.ok()) << st.ToString();
    if (!st.ok()) {
      return nullptr;
    }
    if (batch == nullptr) {
      break;
    }
    EXPECT_GT(batch->num_rows(), 0);
    EXPECT_LE(batch->num_rows(), batch_size);
    EXPECT_TRUE(batch->ValidateFull().ok());
    batches.push_back(std::move(batch));
  }
  auto result = arrow::Table::FromRecordBatches((*reader)->schema(), batches);
  EXPECT_TRUE(result.ok()) << result.status().ToString();
  return result.ok() ? *result : nullptr;
}

// The tables of the star schema (tools/fixturegen/star.h).
constexpr auto kStarFiles = std::to_array<std::string_view>(
    {"star/trips.parquet", "star/riders.parquet", "star/drivers.parquet", "star/zones.parquet",
     "star/cities.parquet", "star/shifts.parquet", "star/tariffs.parquet", "star/promos.parquet"});

TEST(Scan, EveryFixtureTableInTheEngineView) {
  std::vector<std::string_view> files = {"hits_like.parquet",
                                         "hits_like_nulls.parquet",
                                         "hits_like_split/part-*.parquet",
                                         "hits_like_required.parquet",
                                         "edge.parquet",
                                         "empty.parquet"};
  files.insert(files.end(), kStarFiles.begin(), kStarFiles.end());
  for (const std::string_view file : files) {
    for (const bool clickbench : {false, true}) {
      if (clickbench && (file == "edge.parquet" || file.starts_with("star/"))) {
        continue;  // no EventDate column
      }
      const auto table = OpenFixture(file, clickbench);
      ASSERT_NE(table, nullptr);
      const auto scanned = ScanAll(*table, AllFields(*table), 777);
      ASSERT_NE(scanned, nullptr) << file;
      EXPECT_EQ(scanned->num_rows(), table->exact_row_count()) << file;
      EXPECT_TRUE(scanned->schema()->Equals(*table->schema())) << file;
      for (const auto& field : scanned->schema()->fields()) {
        EXPECT_TRUE(plan::FromArrow(*field->type()).ok()) << file << " " << field->ToString();
      }
    }
  }
}

TEST(Scan, VariantsHoldTheSameValues) {
  const auto base = OpenFixture("hits_like.parquet", /*clickbench=*/true);
  ASSERT_NE(base, nullptr);
  const auto expected = ScanAll(*base, AllFields(*base), 65536);
  ASSERT_NE(expected, nullptr);
  for (const std::string_view file :
       {"hits_like_split/part-*.parquet", "hits_like_required.parquet"}) {
    const auto table = OpenFixture(file, /*clickbench=*/true);
    ASSERT_NE(table, nullptr);
    const auto actual = ScanAll(*table, AllFields(*table), 1000);
    ASSERT_NE(actual, nullptr);
    ASSERT_EQ(actual->num_columns(), expected->num_columns()) << file;
    for (int i = 0; i < expected->num_columns(); ++i) {
      EXPECT_TRUE(actual->column(i)->Equals(*expected->column(i)))
          << file << " column " << expected->schema()->field(i)->name();
    }
  }
}

TEST(Scan, ProjectionsReadOnlyTheRequestedFieldsInOrder) {
  const auto table = OpenFixture("hits_like_split/part-*.parquet", /*clickbench=*/true);
  ASSERT_NE(table, nullptr);
  const auto& schema = *table->schema();
  const int event_date = schema.GetFieldIndex("EventDate");
  const int title = schema.GetFieldIndex("Title");
  const int user_id = schema.GetFieldIndex("UserID");
  ASSERT_GE(event_date, 0);
  ASSERT_GE(title, 0);
  ASSERT_GE(user_id, 0);
  const auto all = ScanAll(*table, AllFields(*table), 4096);
  const auto some = ScanAll(*table, {user_id, event_date, title}, 333);
  ASSERT_NE(all, nullptr);
  ASSERT_NE(some, nullptr);
  ASSERT_EQ(some->num_columns(), 3);
  EXPECT_EQ(some->schema()->field(0)->name(), "UserID");
  EXPECT_EQ(some->schema()->field(1)->type()->id(), arrow::Type::DATE32);
  EXPECT_EQ(some->schema()->field(2)->type()->id(), arrow::Type::BINARY);
  EXPECT_TRUE(some->column(0)->Equals(*all->column(user_id)));
  EXPECT_TRUE(some->column(1)->Equals(*all->column(event_date)));
  EXPECT_TRUE(some->column(2)->Equals(*all->column(title)));
  // No fields: only row counts.
  const auto none = ScanAll(*table, {}, 999);
  ASSERT_NE(none, nullptr);
  EXPECT_EQ(none->num_columns(), 0);
  EXPECT_EQ(none->num_rows(), 10'000);
}

// The value of row i of an integer-valued engine column (SMALLINT, INTEGER, BIGINT, USMALLINT,
// DATE as days).
Int128 IntegerAt(const arrow::Array& column, int64_t i) {
  switch (column.type_id()) {
    case arrow::Type::INT16:
      return static_cast<const arrow::Int16Array&>(column).Value(i);
    case arrow::Type::INT32:
      return static_cast<const arrow::Int32Array&>(column).Value(i);
    case arrow::Type::INT64:
      return static_cast<const arrow::Int64Array&>(column).Value(i);
    case arrow::Type::UINT16:
      return static_cast<const arrow::UInt16Array&>(column).Value(i);
    case arrow::Type::DATE32:
      return static_cast<const arrow::Date32Array&>(column).Value(i);
    default:
      ADD_FAILURE() << "not integer-valued: " << column.type()->ToString();
      return 0;
  }
}

// Statistics for skipping parts (plan::Table::part_stats) are exact wherever a fixture's footer
// gives them: for every table, part and field that has them, the part's rows hold exactly that
// NULL count and, over the non-NULL values, that min and max. Fields of other types give none.
TEST(Scan, PartStatisticsMatchThePartsRows) {
  // Among the star tables: SMALLINT, DATE and sparse BIGINT keys, a last part of one row (drivers),
  // DECIMAL and DOUBLE columns, which give none, and an empty table.
  std::vector<std::pair<std::string_view, bool>> files = {{"hits_like.parquet", true},
                                                          {"hits_like_nulls.parquet", true},
                                                          {"hits_like_required.parquet", true},
                                                          {"hits_like_split/part-*.parquet", true},
                                                          {"edge.parquet", false},
                                                          {"floats.parquet", false}};
  for (const std::string_view file : kStarFiles) {
    files.emplace_back(file, false);
  }
  int64_t checked = 0;
  for (const auto& [file, clickbench] : files) {
    const auto table = OpenFixture(file, clickbench);
    ASSERT_NE(table, nullptr);
    for (int field = 0; field < table->schema()->num_fields(); ++field) {
      const auto type = plan::FromArrow(*table->schema()->field(field)->type());
      const bool integer_valued =
          type.ok() && ((plan::IsInteger(*type) && *type != plan::LogicalType::kHugeInt) ||
                        *type == plan::LogicalType::kDate);
      for (int64_t part = 0; part < table->num_parts(); ++part) {
        const auto stats = table->part_stats(part, field);
        if (!integer_valued) {
          EXPECT_FALSE(stats.has_value()) << file << " field " << field;
          continue;
        }
        if (!stats.has_value()) {
          continue;
        }
        auto reader = table->ScanPart(part, {field}, 1024);
        ASSERT_TRUE(reader.ok()) << reader.status().ToString();
        std::optional<Int128> min;
        std::optional<Int128> max;
        int64_t nulls = 0;
        int64_t rows = 0;
        while (true) {
          std::shared_ptr<arrow::RecordBatch> batch;
          ASSERT_TRUE((*reader)->ReadNext(&batch).ok());
          if (batch == nullptr) {
            break;
          }
          const auto& column = *batch->column(0);
          rows += column.length();
          for (int64_t i = 0; i < column.length(); ++i) {
            if (column.IsNull(i)) {
              ++nulls;
              continue;
            }
            const Int128 v = IntegerAt(column, i);
            min = min.has_value() ? std::min(*min, v) : v;
            max = max.has_value() ? std::max(*max, v) : v;
          }
        }
        EXPECT_EQ(stats.value().rows, rows) << file << " " << part << " " << field;
        EXPECT_EQ(stats.value().null_count, nulls) << file << " " << part << " " << field;
        EXPECT_EQ(stats.value().min, min) << file << " " << part << " " << field;
        EXPECT_EQ(stats.value().max, max) << file << " " << part << " " << field;
        ++checked;
      }
    }
  }
  EXPECT_GT(checked, 50) << "the fixtures have integer columns with statistics";
  const auto edge = OpenFixture("edge.parquet");
  ASSERT_NE(edge, nullptr);
  for (const auto& [part, field] : std::vector<std::pair<int64_t, int>>{
           {-1, 0}, {edge->num_parts(), 0}, {0, -1}, {0, edge->schema()->num_fields()}}) {
    EXPECT_FALSE(edge->part_stats(part, field).has_value()) << part << " " << field;
  }
}

}  // namespace
}  // namespace antb1::integration
