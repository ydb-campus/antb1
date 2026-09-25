// io::ParquetTable::Scan over the Parquet fixtures (label integration): every table, every field,
// odd batch sizes; the hits-like variants (split into files, REQUIRED + UTF8) scan to the same
// values.

#include <array>
#include <cstdint>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

#include <arrow/api.h>
#include <gtest/gtest.h>

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

TEST(Scan, EveryFixtureTableInTheEngineView) {
  for (const std::string_view file :
       {"hits_like.parquet", "hits_like_nulls.parquet", "hits_like_split/part-*.parquet",
        "hits_like_required.parquet", "edge.parquet", "empty.parquet"}) {
    for (const bool clickbench : {false, true}) {
      if (clickbench && file == "edge.parquet") {
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

}  // namespace
}  // namespace antb1::integration
