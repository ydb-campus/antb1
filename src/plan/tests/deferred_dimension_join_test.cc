#include "antb1/plan/deferred_dimension_join.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <gtest/gtest.h>

#include "antb1/common/narrow.h"
#include "antb1/plan/catalog.h"
#include "antb1/plan/logical_plan.h"

#include "plan_test_util.h"

namespace antb1::plan {
namespace {

class DeferredDimensionJoinPlanTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto schema = arrow::schema(
        {arrow::field("slot", arrow::int64()), arrow::field("code", arrow::large_utf8()),
         arrow::field("previous_slot", arrow::int64()), arrow::field("label", arrow::binary()),
         arrow::field("samples", arrow::list(arrow::int64()))});
    const auto sensors = std::make_shared<testing::FakeTable>(schema, 10);
    const auto archived = std::make_shared<testing::FakeTable>(schema, 10);
    const auto readings = std::make_shared<testing::FakeTable>(
        arrow::schema({arrow::field("slot", arrow::int64()), arrow::field("sample", arrow::int64()),
                       arrow::field("temperature", arrow::float64())}),
        1000);
    sensors->WithDistinctCount(0, 0, 10);
    sensors->WithDistinctCount(0, 2, 10);
    archived->WithDistinctCount(0, 0, 10);
    readings->WithDistinctCount(0, 0, 10);
    readings->WithDistinctCount(0, 1, 2);
    ASSERT_TRUE(catalog_.Register("sensors", sensors).ok());
    ASSERT_TRUE(catalog_.Register("archived", archived).ok());
    ASSERT_TRUE(catalog_.Register("readings", readings).ok());
    const auto bound = testing::BindSql(
        "WITH counts AS (SELECT d.code, COUNT(f.sample) AS n FROM sensors d JOIN readings f "
        "ON d.slot=f.slot GROUP BY d.code) SELECT a.code, b.n FROM counts a JOIN counts b "
        "ON a.code=b.code ORDER BY a.code, b.n",
        catalog_);
    ASSERT_TRUE(bound.ok()) << bound.status();
    plan_ = Optimized(*bound);
    ASSERT_TRUE(FindDeferredDimensionJoin(plan_).has_value());
  }

  struct RightDimension {
    std::string_view table = "sensors";
    std::string_view surrogate = "slot";
    std::string_view business = "code";
  };

  void BuildBranches(std::string_view aggregate, RightDimension right) {
    const auto bound = testing::BindSql(
        std::format("WITH a AS (SELECT d.code, {0} AS n FROM readings f JOIN sensors d "
                    "ON f.slot=d.slot GROUP BY d.code), "
                    "b AS (SELECT d.{3} AS code, {0} AS n FROM readings f JOIN {1} d "
                    "ON f.slot=d.{2} GROUP BY d.{3}) "
                    "SELECT a.code,a.n,b.n FROM a JOIN b ON a.code=b.code ORDER BY a.code,a.n,b.n",
                    aggregate, right.table, right.surrogate, right.business),
        catalog_);
    ASSERT_TRUE(bound.ok()) << bound.status();
    plan_ = Optimized(*bound);
  }

  template <class F>
  LogicalPlan WithOuterJoin(F change) const {
    const auto& output = std::get<ProjectNode>(*plan_.root);
    const auto& sort = std::get<SortNode>(*output.input);
    auto join = std::get<JoinNode>(*sort.input);
    change(join);
    auto child = std::make_shared<const LogicalNode>(std::move(join));
    auto root = WithInputs(output, {WithInputs(sort, {std::move(child)})});
    return {.root = std::move(root), .output = plan_.output};
  }

  template <class F>
  void ChangeOneGroup(F change, bool resolve = true) {
    bool changed = false;
    const auto rewrite = [&](auto& self, const LogicalNodePtr& node) -> LogicalNodePtr {
      if (!changed) {
        if (const auto* group = std::get_if<GroupAggregateNode>(node.get())) {
          auto copy = *group;
          if (change(copy)) {
            changed = true;
            return std::make_shared<const LogicalNode>(std::move(copy));
          }
        }
      }
      auto inputs = InputsOf(*node);
      for (auto& input : inputs) {
        input = self(self, input);
      }
      return WithInputs(*node, std::move(inputs));
    };
    plan_.root = rewrite(rewrite, plan_.root);
    ASSERT_TRUE(changed);
    if (resolve) {
      plan_ = ResolvePositions(plan_);
    }
  }

  Catalog catalog_;
  LogicalPlan plan_;
};

TEST_F(DeferredDimensionJoinPlanTest, SingletonMinAndMaxPreserveExactValues) {
  for (const std::string_view aggregate : {"MIN(f.sample)", "MAX(f.sample)"}) {
    SCOPED_TRACE(aggregate);
    ASSERT_NO_FATAL_FAILURE(BuildBranches(aggregate, {}));
    EXPECT_TRUE(FindDeferredDimensionJoin(plan_));
  }
  ASSERT_NO_FATAL_FAILURE(BuildBranches("MAX(f.temperature)", {}));
  EXPECT_FALSE(FindDeferredDimensionJoin(plan_));  // the grouped representative of signed zero
}

TEST_F(DeferredDimensionJoinPlanTest, BothBranchesMustDescribeTheSameDimensionMapping) {
  for (const auto& right :
       {RightDimension{.table = "archived"}, RightDimension{.surrogate = "previous_slot"},
        RightDimension{.business = "label"}}) {
    SCOPED_TRACE(std::format("{}.{} -> {}", right.table, right.surrogate, right.business));
    ASSERT_NO_FATAL_FAILURE(BuildBranches("SUM(f.sample)", right));
    EXPECT_FALSE(FindDeferredDimensionJoin(plan_));
  }
}

TEST_F(DeferredDimensionJoinPlanTest, RequiresOneOrdinaryEqualKeyAtTheOuterJoin) {
  EXPECT_FALSE(FindDeferredDimensionJoin(
      WithOuterJoin([](JoinNode& join) { join.keys.front().null_equal = true; })));
  EXPECT_FALSE(FindDeferredDimensionJoin(
      WithOuterJoin([](JoinNode& join) { join.keys.push_back(join.keys.front()); })));
  EXPECT_FALSE(FindDeferredDimensionJoin(
      WithOuterJoin([](JoinNode& join) { join.verify_dimension_business_field = 0; })));
}

TEST_F(DeferredDimensionJoinPlanTest, ExhaustedColumnIdsDeclineWithoutWrapping) {
  auto output = std::get<ProjectNode>(*plan_.root);
  const ColumnId last{std::numeric_limits<std::uint32_t>::max()};
  output.ids.front() = last;
  plan_.output.front().id = last;
  plan_.root = std::make_shared<const LogicalNode>(std::move(output));
  ASSERT_FALSE(PositionMismatch(plan_));
  EXPECT_FALSE(FindDeferredDimensionJoin(plan_));
}

TEST_F(DeferredDimensionJoinPlanTest, CountMergeRequiresANonNullPartial) {
  const auto original = plan_;
  for (const auto kind : {AggKind::kCountStar, AggKind::kMin}) {
    plan_ = original;
    ChangeOneGroup([kind](GroupAggregateNode& group) {
      auto& state = group.aggregates.front();
      if (state.kind != AggKind::kCount) {
        return false;
      }
      state.kind = kind;
      if (kind == AggKind::kCountStar) {
        state.arg.reset();
      }
      return true;
    });
    // An all-NULL minimum would be passed through as NULL. SUM_COUNTS returns zero there.
    EXPECT_EQ(FindDeferredDimensionJoin(plan_).has_value(), kind == AggKind::kCountStar);
  }
}

TEST_F(DeferredDimensionJoinPlanTest, RejectsInvalidAndIncompatibleDimensionFieldMappings) {
  const auto original = plan_;
  // Negative/out of range, an integer where the group reads a string, and an unsupported list.
  for (const int replacement : {-1, 5, 2, 4}) {
    SCOPED_TRACE(replacement);
    plan_ = original;
    ChangeOneGroup(
        [replacement](GroupAggregateNode& group) {
          const auto* join = std::get_if<JoinNode>(group.input.get());
          if (join == nullptr) {
            return false;
          }
          auto inputs = InputsOf(*group.input);
          for (auto& input : inputs) {
            if (const auto* scan = std::get_if<ScanNode>(input.get())) {
              auto broken = *scan;
              std::ranges::replace(broken.fields, 1, replacement);
              input = std::make_shared<const LogicalNode>(std::move(broken));
              group.input = WithInputs(*join, std::move(inputs));
              return true;
            }
          }
          return false;
        },
        /*resolve=*/false);
    EXPECT_FALSE(FindDeferredDimensionJoin(plan_));
  }
}

TEST_F(DeferredDimensionJoinPlanTest, NeedsEveryPartialKeyThatTheDimensionDoesNotDetermine) {
  const auto fresh = ColumnId{std::to_underlying(MaxColumnId(*plan_.root)) + 1};
  ChangeOneGroup([&](GroupAggregateNode& group) {
    if (group.aggregates.front().kind != AggKind::kCount) {
      return false;
    }
    // Two different readings from one sensor now make two partials. The original outer
    // aggregate must merge their counts, even when that sensor identifies exactly one row.
    group.keys.push_back(*group.aggregates.front().arg);
    group.key_ids.push_back(fresh);
    return true;
  });
  EXPECT_FALSE(FindDeferredDimensionJoin(plan_));
}

TEST_F(DeferredDimensionJoinPlanTest, OnlyAnInnerJoinPropagatesTheFixedKeyFromItsRight) {
  const auto bound = testing::BindSql(
      "WITH counts AS (SELECT d.code,f.sample,COUNT(f.sample) AS n FROM readings f JOIN sensors d "
      "ON f.slot=d.slot GROUP BY d.code,f.sample HAVING f.sample=7) "
      "SELECT a.code,a.n,b.n FROM counts a JOIN counts b ON a.code=b.code ORDER BY a.code,a.n,b.n",
      catalog_);
  ASSERT_TRUE(bound.ok()) << bound.status();
  const auto original = Optimized(*bound);
  ASSERT_TRUE(FindDeferredDimensionJoin(original));
  const auto next = std::to_underlying(MaxColumnId(*original.root)) + 1;
  for (const auto kind : {JoinKind::kInner, JoinKind::kLeft}) {
    plan_ = original;
    ChangeOneGroup([&](GroupAggregateNode& group) {
      if (group.aggregates.front().kind != AggKind::kCount) {
        return false;
      }
      const auto slot = std::ranges::find(group.keys, "slot", &BoundColumn::name);
      if (slot == group.keys.end()) {
        return false;
      }
      // The new fact side groups by its own slot and looks up the sample through its
      // reading. A left join can produce both a match and NULL for the same grouped slot.
      auto left = std::make_shared<const LogicalNode>(
          ScanNode{.table = catalog_.Find("readings"),
                   .fields = {0, 1},
                   .ids = {ColumnId{next}, ColumnId{next + 1}}});
      auto reading = *slot;
      reading.id = ColumnId{next + 1};
      group.input = std::make_shared<const LogicalNode>(
          JoinNode{.kind = kind,
                   .left = std::move(left),
                   .right = group.input,
                   .keys = {{.left = reading, .right = *slot}}});
      slot->id = ColumnId{next};
      return true;
    });
    EXPECT_EQ(FindDeferredDimensionJoin(plan_).has_value(), kind == JoinKind::kInner);
  }
}

TEST_F(DeferredDimensionJoinPlanTest, VerifiesTheBusinessKeyOnTheFinalDimensionBuild) {
  const auto fast = FindDeferredDimensionJoin(plan_);
  if (!fast) {
    FAIL() << "expected a deferred dimension lookup";
  }
  EXPECT_FALSE(PositionMismatch(*fast).has_value());
  const auto& output = std::get<ProjectNode>(*fast->root);
  const auto& order = std::get<SortNode>(*output.input);
  const auto& lookup = std::get<JoinNode>(*order.input);
  ASSERT_EQ(lookup.build, BuildSide::kRight);
  const int position = lookup.verify_dimension_business_field.value_or(-1);
  ASSERT_GE(position, 0);
  const auto& scan = std::get<ScanNode>(*lookup.right);
  const auto business = scan.fields.at(Narrow<std::size_t>(position));
  EXPECT_EQ(scan.table->schema()->field(business)->name(), "code");
}

TEST_F(DeferredDimensionJoinPlanTest, RepeatedPassThroughIdAtRootPreservesOriginalBindings) {
  auto output = std::get<ProjectNode>(*plan_.root);
  output.ids.front() = output.columns.front().id;
  plan_.output.front().id = output.ids.front();
  output.ids.push_back(output.ids.front());
  output.columns.push_back(output.columns.front());
  if (!output.constants.empty()) {
    output.constants.push_back(output.constants.front());
  }
  plan_.output.push_back(plan_.output.front());
  plan_.root = std::make_shared<const LogicalNode>(std::move(output));
  ASSERT_FALSE(PositionMismatch(plan_).has_value());
  EXPECT_FALSE(FindDeferredDimensionJoin(plan_));
}

TEST_F(DeferredDimensionJoinPlanTest, CountingASinglePartialIsNotItsValue) {
  ChangeOneGroup([](GroupAggregateNode& group) {
    if (group.aggregates.front().kind != AggKind::kSumCounts) {
      return false;
    }
    // Both results are BIGINT; COUNT of a non-NULL partial gives one even when the partial
    // itself holds a larger count.
    group.aggregates.front().kind = AggKind::kCount;
    return true;
  });
  EXPECT_FALSE(FindDeferredDimensionJoin(plan_));
}

}  // namespace
}  // namespace antb1::plan
