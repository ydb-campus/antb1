#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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

class UnionAggregateTest : public testing::ExecTest {
 protected:
  void SetUp() override {
    const auto schema =
        arrow::schema({arrow::field("k", arrow::int64()), arrow::field("v", arrow::int64())});
    // Distinct branch inputs overlap. The dimension repeats a key, contains a NULL group,
    // and omits several fact keys. One measure group contains only NULLs.
    for (int64_t offset : {0, 1}) {
      arrow::RecordBatchVector parts;
      for (int64_t p = 0; p < 4; ++p) {
        std::vector<std::optional<int64_t>> keys;
        std::vector<std::optional<int64_t>> values;
        for (int64_t i = 0; i < 8192; ++i) {
          const auto k = (i + offset + p) % 7;
          keys.emplace_back(k == 6 ? std::nullopt : std::optional(k));
          values.emplace_back(k == 1 ? std::nullopt : std::optional((i % 5) - 2));
        }
        parts.push_back(arrow::RecordBatch::Make(schema, 8192,
                                                 {testing::Int64s(keys), testing::Int64s(values)}));
      }
      ASSERT_TRUE(catalog_
                      .Register(offset == 0 ? "a" : "b", std::make_shared<testing::MemoryTable>(
                                                             schema, std::move(parts), true))
                      .ok());
    }
    auto batch = arrow::RecordBatch::Make(schema, 6,
                                          {testing::Int64s({0, 0, 1, 2, 3, std::nullopt}),
                                           testing::Int64s({1, 2, 1, 2, std::nullopt, 2})});
    ASSERT_TRUE(catalog_
                    .Register("dim", std::make_shared<testing::MemoryTable>(
                                         schema, arrow::RecordBatchVector{batch}, true))
                    .ok());
  }

  arrow::Result<plan::LogicalPlan> Bound(std::string_view outputs) {
    auto stmt =
        sql::Parse(std::format("SELECT d.v, {} FROM (SELECT v,k FROM a UNION ALL "
                               "SELECT v,k FROM b) f JOIN dim d ON d.k=f.k "
                               "GROUP BY d.v ORDER BY d.v",
                               outputs));
    if (!stmt) {
      return arrow::Status::Invalid("test SQL failed to parse");
    }
    return plan::Bind(*stmt, catalog_);
  }

  static arrow::Result<std::shared_ptr<arrow::Table>> Rows(const plan::LogicalPlan& logical) {
    ARROW_ASSIGN_OR_RAISE(auto op, BuildPhysicalPlan(logical));
    ExecContext ctx{.pool = arrow::default_memory_pool()};
    return Drain(*op, ctx);
  }

  // Verify the placement at the union itself: identical operator counts can mean opposite plans.
  static const plan::UnionAllNode* FindUnion(const plan::LogicalNode& node) {
    if (const auto* branches = std::get_if<plan::UnionAllNode>(&node)) {
      return branches;
    }
    for (const auto& child : plan::InputsOf(node)) {
      if (const auto* found = FindUnion(*child)) {
        return found;
      }
    }
    return nullptr;
  }

  plan::Catalog catalog_;
};

TEST_F(UnionAggregateTest, ConditionalSumsUseOnePartialForEachDistinctMeasure) {
  const auto bound = Bound(
      "SUM(CASE WHEN d.v=1 THEN f.v END), "
      "SUM(CASE WHEN d.v=2 THEN f.v END), "
      "SUM(CASE WHEN d.v=1 THEN f.k END)");
  ASSERT_TRUE(bound.ok()) << bound.status();
  plan::LogicalPlan fallback;
  const auto opt = plan::Optimize(*bound, {.memory_limit = 1024 * 1024 * 32}, &fallback);
  ASSERT_TRUE(opt.ok()) << opt.status();
  ASSERT_TRUE(fallback.root);
  const auto* branches = FindUnion(*opt->root);
  ASSERT_NE(branches, nullptr);
  ASSERT_EQ(branches->inputs.size(), 2U);
  for (const auto& input : branches->inputs) {
    const auto* partial = std::get_if<plan::GroupAggregateNode>(input.get());
    ASSERT_NE(partial, nullptr);
    ASSERT_EQ(partial->aggregates.size(), 2U);
    EXPECT_EQ(partial->keys.size(), 1U);
    EXPECT_TRUE(std::holds_alternative<plan::ProjectNode>(*partial->input));
  }
  const auto actual = Rows(*opt);
  const auto original = Rows(fallback);
  ASSERT_TRUE(actual.ok()) << actual.status();
  ASSERT_TRUE(original.ok()) << original.status();
  EXPECT_EQ((*actual)->num_rows(), 3);
  EXPECT_TRUE((*actual)->Equals(**original));
}

TEST_F(UnionAggregateTest, BranchJoinsMergeCountsAndExtremaInOriginalOutputTypes) {
  const auto bound = Bound(
      "COUNT(*), COUNT(f.v), MIN(f.v), MAX(f.v), "
      "SUM(CASE WHEN d.v=1 THEN f.v ELSE 1 END)");
  ASSERT_TRUE(bound.ok()) << bound.status();
  plan::LogicalPlan fallback;
  const auto opt =
      plan::Optimize(*bound, {.threads = 4, .memory_limit = 1024 * 1024 * 32}, &fallback);
  ASSERT_TRUE(opt.ok()) << opt.status();
  ASSERT_TRUE(fallback.root);
  const auto* branches = FindUnion(*opt->root);
  ASSERT_NE(branches, nullptr);
  for (const auto& arm : branches->inputs) {
    const auto* partial = std::get_if<plan::GroupAggregateNode>(arm.get());
    ASSERT_NE(partial, nullptr);
    const auto* compute = std::get_if<plan::ComputeNode>(partial->input.get());
    ASSERT_NE(compute, nullptr);
    EXPECT_TRUE(std::holds_alternative<plan::JoinNode>(*compute->input));
  }
  const auto actual = Rows(*opt);
  const auto original = Rows(fallback);
  ASSERT_TRUE(actual.ok()) << actual.status();
  ASSERT_TRUE(original.ok()) << original.status();
  EXPECT_TRUE((*actual)->Equals(**original));

  for (const auto options :
       {plan::BindOptions{.threads = 1}, plan::BindOptions{.threads = 4, .batch_size = 4},
        plan::BindOptions{.threads = 4, .memory_limit = 128}}) {
    const auto refused = plan::Optimize(*bound, options, &fallback);
    EXPECT_FALSE(fallback.root);
    ASSERT_TRUE(refused.ok());
    const auto* refused_union = FindUnion(*refused->root);
    ASSERT_NE(refused_union, nullptr);
    EXPECT_TRUE(std::holds_alternative<plan::ProjectNode>(*refused_union->inputs.front()));
  }
}

TEST_F(UnionAggregateTest, UnsafeOrFactDependentConditionalSumsKeepTheOriginalPlan) {
  const auto verify = [&](std::string_view expr, int threads) {
    const auto bound = Bound(expr);
    ASSERT_TRUE(bound.ok()) << bound.status();
    const auto opt = plan::Optimize(*bound, {.threads = threads});
    ASSERT_TRUE(opt.ok()) << opt.status();
    const auto* branches = FindUnion(*opt->root);
    ASSERT_NE(branches, nullptr);
    EXPECT_TRUE(std::holds_alternative<plan::ProjectNode>(*branches->inputs.front())) << expr;
  };
  for (const int threads : {1, 4}) {
    verify("SUM(CASE WHEN d.v=1 THEN f.v / 2 END)", threads);
    verify("SUM(CASE WHEN d.v=1 THEN CAST(f.v AS DECIMAL(36,2)) END)", threads);
  }
  verify("SUM(CASE WHEN f.k=1 THEN f.v END)", 1);
  verify("SUM(CASE WHEN d.v=1 THEN f.v ELSE 0 END)", 1);
}

}  // namespace
}  // namespace antb1::exec
