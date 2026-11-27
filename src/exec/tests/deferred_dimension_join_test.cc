#include <algorithm>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <gtest/gtest.h>

#include "antb1/exec/physical_planner.h"
#include "antb1/plan/binder.h"
#include "antb1/plan/catalog.h"
#include "antb1/plan/optimizer.h"
#include "antb1/sql/parser.h"

#include "exec_test_util.h"

namespace antb1::exec {
namespace {

using testing::Int64s;
using testing::MemoryTable;
using testing::Strings;

// A separate read of just the keys can observe different values from the group payload. A
// missing payload table models a decode failure confined to that column's data pages.
class SensorPayloadTable final : public plan::Table {
 public:
  SensorPayloadTable(std::shared_ptr<MemoryTable> keys, std::shared_ptr<MemoryTable> payload)
      : keys_(std::move(keys)), payload_(std::move(payload)) {}

  const std::shared_ptr<arrow::Schema>& schema() const override { return keys_->schema(); }
  std::optional<int64_t> exact_row_count() const override { return keys_->exact_row_count(); }
  int64_t num_parts() const override { return keys_->num_parts(); }
  std::optional<int64_t> part_rows(int64_t part) const override { return keys_->part_rows(part); }
  std::optional<plan::PartStats> part_stats(int64_t part, int field) const override {
    return keys_->part_stats(part, field);
  }
  std::string Describe() const override { return "sensors"; }

 protected:
  arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> DoScan(
      const std::vector<int>& fields, int64_t size, arrow::MemoryPool* pool,
      const std::shared_ptr<plan::ScanContext>& context) const override {
    ARROW_ASSIGN_OR_RAISE(const auto table, Source(fields));
    return table->Scan(fields, size, pool, context);
  }
  arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> DoScanPart(
      int64_t part, const std::vector<int>& fields, int64_t size, arrow::MemoryPool* pool,
      const std::shared_ptr<plan::ScanContext>& context) const override {
    ARROW_ASSIGN_OR_RAISE(const auto table, Source(fields));
    return table->ScanPart(part, fields, size, pool, context);
  }

 private:
  arrow::Result<const MemoryTable*> Source(const std::vector<int>& fields) const {
    if (!std::ranges::contains(fields, 2)) {
      return keys_.get();
    }
    if (!payload_) {
      return arrow::Status::IOError("cannot decode the sensor site");
    }
    return payload_.get();
  }
  std::shared_ptr<MemoryTable> keys_;
  std::shared_ptr<MemoryTable> payload_;
};

class DeferredDimensionJoinTest : public testing::ExecTest {
 protected:
  void SetUp() override {
    const auto schema = arrow::schema({arrow::field("slot", arrow::int64()),
                                       arrow::field("reading", arrow::int64()),
                                       arrow::field("band", arrow::int64())});
    for (const int64_t factor : {1, 2}) {
      std::vector<std::optional<int64_t>> slots;
      std::vector<std::optional<int64_t>> readings;
      std::vector<std::optional<int64_t>> bands;
      for (int64_t i = 0; i < 128; ++i) {
        // The NULL key and slot 3 cannot match a sensor. Every reading for slot 4 is NULL.
        const int64_t slot = (i % 4) + 1;
        slots.emplace_back(i % 8 == 0 ? std::nullopt : std::optional(slot));
        const int64_t reading = slot == 3 ? 200000 : factor * slot;
        readings.emplace_back(slot == 4 ? std::nullopt : std::optional(reading));
        bands.emplace_back((i / 8) % 2);
      }
      const auto batch =
          arrow::RecordBatch::Make(schema, 128, {Int64s(slots), Int64s(readings), Int64s(bands)});
      ASSERT_TRUE(catalog_
                      .Register(factor == 1 ? "calibration" : "samples",
                                std::make_shared<MemoryTable>(
                                    schema, arrow::RecordBatchVector{batch}, true))
                      .ok());
    }
  }

  static std::shared_ptr<MemoryTable> Sensors(std::optional<int64_t> last_slot,
                                              const std::optional<std::string>& last_code,
                                              const std::optional<std::string>& last_site) {
    const auto schema =
        arrow::schema({arrow::field("slot", arrow::int64()), arrow::field("code", arrow::binary()),
                       arrow::field("site", arrow::binary())});
    // Put any duplicate in a different part and batch from the first occurrence.
    arrow::RecordBatchVector parts{
        arrow::RecordBatch::Make(
            schema, 3,
            {Int64s({1, 4, 5}), Strings({"m", "d", "k"}), Strings({"lab", std::nullopt, "hall"})}),
        arrow::RecordBatch::Make(
            schema, 1, {Int64s({last_slot}), Strings({last_code}), Strings({last_site})})};
    return std::make_shared<MemoryTable>(schema, std::move(parts), true);
  }

  void RegisterSensors(std::optional<int64_t> last_slot,
                       const std::optional<std::string>& last_code,
                       const std::optional<std::string>& last_site) {
    // The control uses two independent registrations of the same data, so a dimension common to
    // both branches cannot be factored out. Keep its sort, aggregates and row multiplicities.
    for (const char* name : {"sensors", "sensor_copy"}) {
      ASSERT_TRUE(catalog_.Register(name, Sensors(last_slot, last_code, last_site)).ok());
    }
  }

  static std::string Query(std::string_view second_table, std::string_view second_join = "JOIN",
                           std::string_view predicates = "",
                           std::string_view outputs = "a.code, a.site, a.total, b.total") {
    return std::format(
        "WITH a AS (SELECT d.code, d.site, SUM(f.reading) AS total FROM calibration f "
        "JOIN sensors d ON f.slot=d.slot GROUP BY d.code, d.site), "
        "b AS (SELECT d.code, d.site, SUM(f.reading) AS total FROM samples f "
        "{} {} d ON f.slot=d.slot GROUP BY d.code, d.site) "
        "SELECT {} FROM a JOIN b ON a.code=b.code {} ORDER BY {} LIMIT 50",
        second_join, second_table, outputs, predicates, outputs);
  }

  arrow::Result<std::shared_ptr<arrow::Table>> Execute(std::string_view sql, ProfileNode& profile) {
    const auto parsed = sql::Parse(sql);
    if (!parsed) {
      return arrow::Status::Invalid("sensor query did not parse: ", parsed.error().message);
    }
    ARROW_ASSIGN_OR_RAISE(auto bound, plan::Bind(*parsed, catalog_));
    ARROW_ASSIGN_OR_RAISE(auto optimized, plan::Optimize(bound));
    ARROW_ASSIGN_OR_RAISE(auto op, BuildPhysicalPlan(optimized, &profile));
    return Drain(*op, {.pool = arrow::default_memory_pool(), .batch_size = 2});
  }

  static std::string TaggedQuery(std::string_view second_table) {
    return std::format(
        "WITH tagged AS (SELECT 'setup' AS phase, d.code, d.site, SUM(f.reading) AS total "
        "FROM calibration f JOIN sensors d ON f.slot=d.slot GROUP BY d.code,d.site "
        "UNION ALL SELECT 'run' AS phase, d.code, d.site, SUM(f.reading) AS total "
        "FROM samples f JOIN {} d ON f.slot=d.slot GROUP BY d.code,d.site) "
        "SELECT a.code,b.site,a.total,b.total FROM tagged a JOIN tagged b ON a.code=b.code "
        "WHERE a.phase='setup' AND b.phase='run' "
        "ORDER BY a.code,b.site,a.total,b.total LIMIT 50",
        second_table);
  }

  // Retain a fact-side grouping key independent of the sensor, plus a literal identifying
  // the reading stream. Joining only on code matches every pair of bands for that sensor.
  static std::string BandQuery(std::string_view second_table, std::string_view condition = "") {
    const auto stream = [](std::string_view facts, std::string_view sensors) {
      return std::format(
          "SELECT d.code,d.site,d.slot AS pos,f.band,SUM(f.reading) AS total,'{}' AS origin "
          "FROM {} f JOIN {} d ON f.slot=d.slot GROUP BY d.code,d.site,d.slot,f.band",
          facts, facts, sensors);
    };
    constexpr std::string_view kOutputs = "a.code,a.band,b.band,a.total,b.total,a.origin,b.origin";
    return std::format(
        "WITH a AS ({}), b AS ({}) SELECT {} FROM a JOIN b ON a.code=b.code {} "
        "ORDER BY {} LIMIT 50",
        stream("calibration", "sensors"), stream("samples", second_table), kOutputs, condition,
        kOutputs);
  }

  void Check(bool verified, int64_t expected_rows, bool tagged = false) {
    const auto query = [tagged](std::string_view second_table) {
      return tagged ? TaggedQuery(second_table) : Query(second_table);
    };
    CheckQueries(query("sensors"), query("sensor_copy"), verified, expected_rows);
  }

  void CheckQueries(const std::string& sql, const std::string& ordinary, bool verified,
                    int64_t expected_rows) {
    ProfileNode control;
    const auto expected = Execute(ordinary, control);
    ASSERT_TRUE(expected.ok()) << expected.status();
    ASSERT_EQ((*expected)->num_rows(), expected_rows);
    EXPECT_NE(control.name(), "DeferredDimensionJoin");

    ProfileNode candidate;
    const auto actual = Execute(sql, candidate);
    ASSERT_TRUE(actual.ok()) << actual.status();
    EXPECT_EQ(candidate.name(), "DeferredDimensionJoin");
    const std::string_view counter = verified ? "dimension_verified" : "dimension_fallbacks";
    bool counted = false;
    for (const auto& metric : candidate.metrics()) {
      if (metric.name == counter) {
        EXPECT_EQ(metric.value, 1);
        counted = true;
      }
    }
    EXPECT_TRUE(counted) << counter;
    EXPECT_TRUE((*actual)->Equals(**expected));
  }

  plan::Catalog catalog_;
};

TEST_F(DeferredDimensionJoinTest, UniqueDimensionDefersLookupAcrossGroupedJoins) {
  ASSERT_NO_FATAL_FAILURE(RegisterSensors(2, "a", "yard"));
  Check(true, 3);
}

TEST_F(DeferredDimensionJoinTest, PhaseRestrictionsPreserveLookupColumnsAcrossUnionBranches) {
  ASSERT_NO_FATAL_FAILURE(RegisterSensors(2, "a", "yard"));
  Check(true, 3, /*tagged=*/true);
}

TEST_F(DeferredDimensionJoinTest, MatchingGroupsKeepEveryPairOfBandsAndEachStreamsLiteral) {
  ASSERT_NO_FATAL_FAILURE(RegisterSensors(2, "a", "yard"));
  CheckQueries(BandQuery("sensors"), BandQuery("sensor_copy"), true, 12);
}

TEST_F(DeferredDimensionJoinTest, PayloadPredicatesStayAtTheirOriginalGroupedInputsOrJoin) {
  ASSERT_NO_FATAL_FAILURE(RegisterSensors(2, "a", "yard"));
  for (const auto& [condition, rows] :
       {std::pair{"WHERE a.site='lab'", 4}, std::pair{"WHERE a.total > a.pos", 8},
        std::pair{"AND a.site <> b.code", 8}}) {
    SCOPED_TRACE(condition);
    ProfileNode control;
    const auto expected = Execute(BandQuery("sensor_copy", condition), control);
    ASSERT_TRUE(expected.ok()) << expected.status();
    ASSERT_EQ((*expected)->num_rows(), rows);
    ProfileNode candidate;
    const auto actual = Execute(BandQuery("sensors", condition), candidate);
    ASSERT_TRUE(actual.ok()) << actual.status();
    EXPECT_NE(candidate.name(), "DeferredDimensionJoin");
    EXPECT_TRUE((*actual)->Equals(**expected));
  }
}

TEST_F(DeferredDimensionJoinTest, DuplicateBusinessKeyPreservesCrossMatches) {
  ASSERT_NO_FATAL_FAILURE(RegisterSensors(2, "m", "yard"));
  // The two sites for the shared code yield four combinations alongside the all-NULL
  // measure group from slot 4.
  Check(false, 5);
}

TEST_F(DeferredDimensionJoinTest, VerifyKeysOnTheSameReadAsTheDimensionPayload) {
  const auto keys = Sensors(2, "a", "yard");
  const auto payload = Sensors(2, "m", "yard");
  ASSERT_TRUE(
      catalog_.Register("sensors", std::make_shared<SensorPayloadTable>(keys, payload)).ok());
  ASSERT_TRUE(catalog_.Register("sensor_copy", payload).ok());
  Check(false, 5);
}

TEST_F(DeferredDimensionJoinTest, VerifyABusinessKeyEvenWhenOnlyThePayloadIsReturned) {
  ASSERT_NO_FATAL_FAILURE(RegisterSensors(2, "m", "yard"));
  CheckQueries(Query("sensors", "JOIN", "", "a.site"), Query("sensor_copy", "JOIN", "", "a.site"),
               false, 5);
}

TEST_F(DeferredDimensionJoinTest, UnprojectedGroupingColumnPreservesItsScanError) {
  const auto keys = Sensors(2, "a", "yard");
  for (const char* name : {"sensors", "sensor_copy"}) {
    ASSERT_TRUE(catalog_.Register(name, std::make_shared<SensorPayloadTable>(keys, nullptr)).ok());
  }
  // Keep site in both GROUP BY clauses but omit it from both the output and the sort.
  for (const char* name : {"sensor_copy", "sensors"}) {
    ProfileNode profile;
    const auto result = Execute(Query(name, "JOIN", "", "a.code, a.total, b.total"), profile);
    EXPECT_TRUE(result.status().IsIOError()) << result.status();
    EXPECT_TRUE(result.status().message().contains("cannot decode the sensor site"));
  }
}

TEST_F(DeferredDimensionJoinTest, DuplicateDimensionRowPreservesItsContributionToBothSums) {
  ASSERT_NO_FATAL_FAILURE(RegisterSensors(1, "m", "lab"));
  Check(false, 2);
}

TEST_F(DeferredDimensionJoinTest, NullSurrogateUsesOriginalJoin) {
  ASSERT_NO_FATAL_FAILURE(RegisterSensors(std::nullopt, "a", "yard"));
  Check(false, 2);
}

TEST_F(DeferredDimensionJoinTest, NullBusinessKeyUsesOriginalJoin) {
  ASSERT_NO_FATAL_FAILURE(RegisterSensors(2, std::nullopt, "yard"));
  Check(false, 2);
}

TEST_F(DeferredDimensionJoinTest, OuterDimensionJoinKeepsTheOriginalPlan) {
  ASSERT_NO_FATAL_FAILURE(RegisterSensors(2, "a", "yard"));
  ProfileNode profile;
  const auto actual = Execute(Query("sensors", "LEFT JOIN"), profile);
  ASSERT_TRUE(actual.ok()) << actual.status();
  EXPECT_NE(profile.name(), "DeferredDimensionJoin");
}

TEST_F(DeferredDimensionJoinTest, TopNWithPartialOrderingKeepsTheOriginalPlan) {
  ASSERT_NO_FATAL_FAILURE(RegisterSensors(2, "a", "yard"));
  auto query = Query("sensors");
  query.replace(query.find("ORDER BY"), std::string::npos, "ORDER BY a.site LIMIT 1");
  ProfileNode profile;
  const auto actual = Execute(query, profile);
  ASSERT_TRUE(actual.ok()) << actual.status();
  EXPECT_NE(profile.name(), "DeferredDimensionJoin");
}

TEST_F(DeferredDimensionJoinTest, RejectsOnlyErrorsFromMatchingDimensionKeys) {
  ASSERT_NO_FATAL_FAILURE(RegisterSensors(2, "a", "yard"));
  // The orphaned slot's total does not fit SMALLINT. It was discarded before the
  // written query compared results. Adding an offset also makes a matched slot overflow.
  for (const int offset : {0, 32700}) {
    SCOPED_TRACE(offset);
    const auto predicate = std::format("AND CAST(b.total + {} AS SMALLINT) > 0", offset);
    ProfileNode baseline;
    const auto reference = Execute(Query("sensor_copy", "JOIN", predicate), baseline);
    ProfileNode candidate;
    const auto result = Execute(Query("sensors", "JOIN", predicate), candidate);
    if (offset == 0) {
      ASSERT_TRUE(reference.ok()) << reference.status();
      ASSERT_TRUE(result.ok()) << result.status();
      EXPECT_EQ((*result)->num_rows(), 2);
      EXPECT_TRUE((*result)->Equals(**reference));
    } else {
      ASSERT_FALSE(reference.ok());
      EXPECT_TRUE(reference.status().message().contains("range")) << reference.status();
      EXPECT_EQ(result.status().ToString(), reference.status().ToString());
    }
  }
}

}  // namespace
}  // namespace antb1::exec
