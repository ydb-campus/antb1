// Inner hash joins over the star schema's Parquet files (label integration, one thread). No SQL
// reaches a join until roadmap PR J2b, so the plans are built by hand and run through
// exec::BuildPhysicalPlan: dense keys (the direct layout), repeated and NULL keys, a two-column
// key, VARCHAR keys, the empty dimension, a self-join, a chain of two joins, and part pruning and
// filter pushdown on both sides. The rows are checked against a nested-loop join of plain scans,
// in order, and their counts against DuckDB's for the same joins of the same files, which pending
// J2b records of tests/slt/cases/joins/ hold too (inner.slt, and names.slt for trips with zones).
// The counts depend on the fixtures (tools/fixturegen/star.h): after a change of the fixture
// digest (tests/harness), `pixi run slt-complete` rewrites those records from DuckDB, and the
// counts here follow them.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/compute/initialize.h>
#include <gtest/gtest.h>

#include "antb1/common/int128.h"
#include "antb1/exec/operator.h"
#include "antb1/exec/physical_planner.h"
#include "antb1/exec/profile.h"
#include "antb1/io/parquet_table.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/sql_status.h"
#include "antb1/plan/types.h"

#include "integration_util.h"

namespace antb1::integration {
namespace {

using Rows = std::vector<std::vector<std::string>>;

class JoinTest : public ::testing::Test {
 protected:
  // The operators call Arrow compute kernels (engine::Session::Make initializes them otherwise).
  static void SetUpTestSuite() { ASSERT_TRUE(arrow::compute::Initialize().ok()); }
};

// The table's rows, each cell as text: "null" for NULL, a VARCHAR in quotes, anything else as Arrow
// prints its scalar.
Rows RowsOf(const arrow::Table& table) {
  Rows rows(static_cast<std::size_t>(table.num_rows()));
  for (int c = 0; c < table.num_columns(); ++c) {
    for (int64_t r = 0; r < table.num_rows(); ++r) {
      auto scalar = table.column(c)->GetScalar(r);
      EXPECT_TRUE(scalar.ok()) << scalar.status().ToString();
      std::string cell = "null";
      if ((*scalar)->is_valid) {
        cell = (*scalar)->type->id() == arrow::Type::BINARY
                   ? "'" + static_cast<const arrow::BinaryScalar&>(**scalar).value->ToString() + "'"
                   : (*scalar)->ToString();
      }
      rows[static_cast<std::size_t>(r)].push_back(std::move(cell));
    }
  }
  return rows;
}

// The inner join of `probe` and `build` on these key cells (equal, NULL never matching): every
// probe row in order, each with its matches in build order, its cells first.
Rows ReferenceJoin(const Rows& probe, const Rows& build, const std::vector<std::size_t>& probe_keys,
                   const std::vector<std::size_t>& build_keys) {
  Rows out;
  for (const std::vector<std::string>& p : probe) {
    for (const std::vector<std::string>& b : build) {
      bool match = true;
      for (std::size_t k = 0; k < probe_keys.size() && match; ++k) {
        const std::string& x = p[probe_keys[k]];
        match = x != "null" && x == b[build_keys[k]];
      }
      if (match) {
        std::vector<std::string> row = p;
        row.insert(row.end(), b.begin(), b.end());
        out.push_back(std::move(row));
      }
    }
  }
  return out;
}

// Some columns of a star table (star/<name>.parquet), by name, as a Scan reads them.
struct Side {
  Side(std::string table_name, std::vector<std::string> column_names)
      : name(std::move(table_name)), columns(std::move(column_names)) {
    auto opened = io::ParquetTable::Open({Fixture("star/" + name + ".parquet")});
    EXPECT_TRUE(opened.ok()) << name << ": " << opened.status().ToString();
    if (opened.ok()) {
      table = *opened;
    }
  }

  [[nodiscard]] std::vector<int> Fields() const {
    std::vector<int> fields;
    for (const std::string& column : columns) {
      fields.push_back(table->schema()->GetFieldIndex(column));
      EXPECT_GE(fields.back(), 0) << name << "." << column;
    }
    return fields;
  }

  [[nodiscard]] plan::LogicalNodePtr Scan() const {
    return std::make_shared<const plan::LogicalNode>(
        plan::ScanNode{.table = table, .table_name = name, .fields = Fields()});
  }

  // Column `column` of the scan, at `offset` in a join's output.
  [[nodiscard]] plan::BoundColumn Column(const std::string& column, int offset = 0) const {
    const auto at = std::ranges::find(columns, column);
    EXPECT_NE(at, columns.end()) << name << "." << column;
    const int index = static_cast<int>(at - columns.begin());
    auto type =
        plan::FromArrow(*table->schema()->field(Fields()[static_cast<std::size_t>(index)])->type());
    EXPECT_TRUE(type.ok()) << type.status().ToString();
    return plan::BoundColumn{.index = offset + index, .name = column, .type = *type};
  }

  // The rows of a plain scan of the columns.
  [[nodiscard]] Rows Read() const {
    auto reader = table->Scan(Fields(), 1024);
    EXPECT_TRUE(reader.ok()) << reader.status().ToString();
    auto read = (*reader)->ToTable();
    EXPECT_TRUE(read.ok()) << read.status().ToString();
    return RowsOf(**read);
  }

  std::string name;
  std::vector<std::string> columns;
  std::shared_ptr<io::ParquetTable> table;
};

plan::LogicalNodePtr Node(plan::LogicalNode node) {
  return std::make_shared<const plan::LogicalNode>(std::move(node));
}

// The inner join of `probe` (left) and `build` (right) where probe_keys[k] equals build_keys[k].
plan::LogicalNodePtr JoinOf(plan::LogicalNodePtr probe, plan::LogicalNodePtr build,
                            const std::vector<plan::BoundColumn>& probe_keys,
                            const std::vector<plan::BoundColumn>& build_keys) {
  plan::JoinNode join{.kind = plan::JoinKind::kInner,
                      .left = std::move(probe),
                      .right = std::move(build),
                      .keys = {},
                      .residual = {},
                      .build = plan::BuildSide::kRight,
                      .span = {}};
  for (std::size_t k = 0; k < probe_keys.size(); ++k) {
    join.keys.push_back(plan::JoinKey{.left = probe_keys[k], .right = build_keys[k]});
  }
  return Node(std::move(join));
}

// `column` <op> `value`, a predicate the scan can apply (an INTEGER column, as the binder folds
// a literal into the column's type).
plan::Predicate Compare(const plan::BoundColumn& column, plan::CompareOp op, int64_t value) {
  return plan::Predicate{.kind = plan::Predicate::Kind::kCompare,
                         .column = column,
                         .op = op,
                         .constant = plan::Constant{.type = column.type, .value = Int128{value}},
                         .span = {}};
}

// The rows of the plan of `root` (`width` columns), on one thread, profiled into `profile`.
arrow::Result<Rows> RunPlan(const plan::LogicalNodePtr& root, std::size_t width,
                            exec::ProfileNode* profile = nullptr) {
  plan::LogicalPlan plan{.root = root, .output = {}};
  for (std::size_t i = 0; i < width; ++i) {
    plan.output.push_back({.name = "c" + std::to_string(i), .type = plan::LogicalType::kBigInt});
  }
  ARROW_ASSIGN_OR_RAISE(std::unique_ptr<exec::Operator> op, exec::BuildPhysicalPlan(plan, profile));
  exec::ExecContext ctx;
  ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::Table> table, exec::Drain(*op, ctx));
  return RowsOf(*table);
}

std::optional<int64_t> MetricOf(const exec::ProfileNode& node, const std::string& name) {
  for (const exec::ProfileMetric& metric : node.metrics()) {
    if (metric.name == name) {
      return metric.value;
    }
  }
  return std::nullopt;
}

// trips joins zones on tr_pickup = zn_id: zones' ids are 1..40, unique and dense, so the build
// has the direct layout; pickups 41 to 43 match no zone.
TEST_F(JoinTest, DenseKeysUseTheDirectLayout) {
  const Side trips("trips", {"tr_row", "tr_pickup"});
  const Side zones("zones", {"zn_id", "label"});
  exec::ProfileNode profile;
  auto rows = RunPlan(
      JoinOf(trips.Scan(), zones.Scan(), {trips.Column("tr_pickup")}, {zones.Column("zn_id")}), 4,
      &profile);
  ASSERT_TRUE(rows.ok()) << rows.status().ToString();
  EXPECT_EQ(rows->size(), 2793U);  // DuckDB
  EXPECT_EQ(*rows, ReferenceJoin(trips.Read(), zones.Read(), {1}, {0}));
  ASSERT_EQ(profile.children().size(), 2U);
  const exec::ProfileNode& build = *profile.children()[1];
  EXPECT_EQ(build.name(), "HashBuild");
  EXPECT_EQ(MetricOf(build, "direct"), 1);
  EXPECT_EQ(MetricOf(build, "unique"), 1);
  EXPECT_EQ(build.rows(), 40);
}

// trips joins riders on tr_rider = rd_id: 8 rider ids repeat (two matches each) and 8 are missing,
// some riders' ids lie beyond the riders, and some trips have no rider (NULL never matches). The
// matches of each trip come in the riders' order.
TEST_F(JoinTest, RepeatedAndNullKeys) {
  const Side trips("trips", {"tr_row", "tr_rider"});
  const Side riders("riders", {"rd_id", "rd_tier"});
  exec::ProfileNode profile;
  auto rows = RunPlan(
      JoinOf(trips.Scan(), riders.Scan(), {trips.Column("tr_rider")}, {riders.Column("rd_id")}), 4,
      &profile);
  ASSERT_TRUE(rows.ok()) << rows.status().ToString();
  EXPECT_EQ(rows->size(), 2841U);  // DuckDB
  EXPECT_EQ(*rows, ReferenceJoin(trips.Read(), riders.Read(), {1}, {0}));
  EXPECT_EQ(MetricOf(*profile.children()[1], "unique"), 0);
}

// trips joins shifts on (tr_driver, tr_day) = (sh_driver, sh_day): an INTEGER and a DATE key,
// either of them NULL in some rows; two shifts repeat a pair.
TEST_F(JoinTest, TwoColumnKey) {
  const Side trips("trips", {"tr_row", "tr_driver", "tr_day"});
  const Side shifts("shifts", {"sh_driver", "sh_day", "sh_hours"});
  auto rows = RunPlan(
      JoinOf(trips.Scan(), shifts.Scan(), {trips.Column("tr_driver"), trips.Column("tr_day")},
             {shifts.Column("sh_driver"), shifts.Column("sh_day")}),
      6);
  ASSERT_TRUE(rows.ok()) << rows.status().ToString();
  EXPECT_EQ(rows->size(), 447U);  // DuckDB
  EXPECT_EQ(*rows, ReferenceJoin(trips.Read(), shifts.Read(), {1, 2}, {0, 1}));
}

// trips joins tariffs on tr_tariff = tf_code: VARCHAR keys compared by their bytes ('' is a
// value, 'XL ' is not 'XL', Std is not STD), NULL on both sides.
TEST_F(JoinTest, VarcharKeys) {
  const Side trips("trips", {"tr_row", "tr_tariff"});
  const Side tariffs("tariffs", {"tf_code", "tf_label"});
  auto rows = RunPlan(JoinOf(trips.Scan(), tariffs.Scan(), {trips.Column("tr_tariff")},
                             {tariffs.Column("tf_code")}),
                      4);
  ASSERT_TRUE(rows.ok()) << rows.status().ToString();
  EXPECT_EQ(rows->size(), 2274U);  // DuckDB
  EXPECT_EQ(*rows, ReferenceJoin(trips.Read(), tariffs.Read(), {1}, {0}));
}

// trips joins the empty promos: the build holds no row, so the probe never opens its scans and
// reads no row of trips.
TEST_F(JoinTest, EmptyDimensionReadsNoProbeRows) {
  const Side trips("trips", {"tr_row", "tr_promo"});
  const Side promos("promos", {"pm_id", "pm_code"});
  exec::ProfileNode profile;
  auto rows = RunPlan(
      JoinOf(trips.Scan(), promos.Scan(), {trips.Column("tr_promo")}, {promos.Column("pm_id")}), 4,
      &profile);
  ASSERT_TRUE(rows.ok()) << rows.status().ToString();
  EXPECT_TRUE(rows->empty());  // DuckDB: 0 rows
  ASSERT_EQ(profile.children().size(), 2U);
  const exec::ProfileNode& probe = *profile.children()[0];
  EXPECT_EQ(probe.name(), "HashJoin");
  ASSERT_EQ(probe.children().size(), 1U);
  const exec::ProfileNode& scan = *probe.children()[0];
  EXPECT_EQ(scan.name(), "Scan");
  EXPECT_EQ(scan.rows(), 0);
  EXPECT_EQ(scan.instances(), 0);
  EXPECT_EQ(profile.children()[1]->rows(), 0);
}

// zones joins itself on zn_parent = zn_id (a self-reference): two scans of the same table, each
// with its own parts.
TEST_F(JoinTest, SelfJoin) {
  const Side children("zones", {"zn_id", "zn_parent"});
  const Side parents("zones", {"zn_id", "label"});
  auto rows = RunPlan(JoinOf(children.Scan(), parents.Scan(), {children.Column("zn_parent")},
                             {parents.Column("zn_id")}),
                      4);
  ASSERT_TRUE(rows.ok()) << rows.status().ToString();
  EXPECT_EQ(rows->size(), 34U);  // DuckDB
  EXPECT_EQ(*rows, ReferenceJoin(children.Read(), parents.Read(), {1}, {0}));
}

// trips joins zones on tr_pickup = zn_id, then cities on zn_city = ct_id (SMALLINT keys): two
// probes in one pipeline over trips' parts, cities' build prepared first.
TEST_F(JoinTest, ChainOfTwoJoins) {
  const Side trips("trips", {"tr_row", "tr_pickup"});
  const Side zones("zones", {"zn_id", "zn_city"});
  const Side cities("cities", {"ct_id", "ct_pop"});
  exec::ProfileNode profile;
  auto rows = RunPlan(JoinOf(JoinOf(trips.Scan(), zones.Scan(), {trips.Column("tr_pickup")},
                                    {zones.Column("zn_id")}),
                             cities.Scan(), {zones.Column("zn_city", 2)}, {cities.Column("ct_id")}),
                      6, &profile);
  ASSERT_TRUE(rows.ok()) << rows.status().ToString();
  EXPECT_EQ(rows->size(), 2655U);  // DuckDB
  EXPECT_EQ(*rows, ReferenceJoin(ReferenceJoin(trips.Read(), zones.Read(), {1}, {0}), cities.Read(),
                                 {3}, {0}));
  ASSERT_EQ(profile.children().size(), 3U);
  EXPECT_EQ(profile.children()[1]->rows(), 6);   // cities
  EXPECT_EQ(profile.children()[2]->rows(), 40);  // zones
}

// trips with tr_row >= 2400 joins zones with zn_id <= 10: each side's scan skips the 2 row groups
// its predicate rules out (trips' groups of 1200 rows, zones' of 16) and applies it while it reads.
TEST_F(JoinTest, PruningAndPushdownOnBothSides) {
  const Side trips("trips", {"tr_row", "tr_pickup"});
  const Side zones("zones", {"zn_id", "label"});
  const auto late_trips = Node(plan::FilterNode{
      .input = trips.Scan(),
      .predicates = {Compare(trips.Column("tr_row"), plan::CompareOp::kGe, 2400)}});
  const auto first_zones = Node(
      plan::FilterNode{.input = zones.Scan(),
                       .predicates = {Compare(zones.Column("zn_id"), plan::CompareOp::kLe, 10)}});
  exec::ProfileNode profile;
  auto rows =
      RunPlan(JoinOf(late_trips, first_zones, {trips.Column("tr_pickup")}, {zones.Column("zn_id")}),
              4, &profile);
  ASSERT_TRUE(rows.ok()) << rows.status().ToString();
  EXPECT_EQ(rows->size(), 140U);  // DuckDB
  Rows probe;
  for (const std::vector<std::string>& row : trips.Read()) {
    if (std::stoll(row[0]) >= 2400) {
      probe.push_back(row);
    }
  }
  Rows build;
  for (const std::vector<std::string>& row : zones.Read()) {
    if (std::stoll(row[0]) <= 10) {
      build.push_back(row);
    }
  }
  EXPECT_EQ(*rows, ReferenceJoin(probe, build, {1}, {0}));
  ASSERT_EQ(profile.children().size(), 2U);
  EXPECT_EQ(MetricOf(profile, "skipped"), 2);
  EXPECT_EQ(MetricOf(*profile.children()[1], "skipped"), 2);
  for (const exec::ProfileNode* side : profile.children()) {
    const exec::ProfileNode& filter = *side->children()[0];
    ASSERT_EQ(filter.name(), "Filter");
    EXPECT_TRUE(filter.children()[0]->detail().ends_with(", 1 pushed predicate"))
        << filter.children()[0]->detail();
  }
}

// The references of the star schema whose keys differ in type are malformed joins until roadmap
// PR J2b casts their keys to one type: INTEGER drivers against BIGINT ids, DECIMAL(4,2) rates
// against DECIMAL(5,3) ones. Invalid, not unsupported.
TEST_F(JoinTest, KeysOfTwoTypesAreInvalid) {
  const Side trips("trips", {"tr_driver", "tr_rate"});
  const Side shifts("shifts", {"sh_driver"});
  const Side drivers("drivers", {"dv_id"});
  const Side tariffs("tariffs", {"tf_rate"});
  for (const auto& [probe, probe_key, build, build_key] :
       {std::tuple(&trips, "tr_driver", &drivers, "dv_id"),
        std::tuple(&shifts, "sh_driver", &drivers, "dv_id"),
        std::tuple(&trips, "tr_rate", &tariffs, "tf_rate")}) {
    const auto rows = RunPlan(JoinOf(probe->Scan(), build->Scan(), {probe->Column(probe_key)},
                                     {build->Column(build_key)}),
                              probe->columns.size() + build->columns.size());
    EXPECT_TRUE(rows.status().IsInvalid()) << probe_key << ": " << rows.status().ToString();
    EXPECT_EQ(plan::GetSqlError(rows.status()), nullptr) << probe_key;
  }
}

}  // namespace
}  // namespace antb1::integration
