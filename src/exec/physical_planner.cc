#include "antb1/exec/physical_planner.h"

#include <memory>
#include <variant>

#include <arrow/api.h>

#include "antb1/exec/row_count.h"

namespace antb1::exec {
namespace {

template <class... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};

}  // namespace

arrow::Result<std::unique_ptr<Operator>> BuildPhysicalPlan(const plan::LogicalPlan& plan) {
  if (!plan.root || plan.output.empty()) {
    return arrow::Status::Invalid("empty logical plan");
  }
  return std::visit(
      Overloaded{[&](const plan::RowCountNode& node) -> arrow::Result<std::unique_ptr<Operator>> {
        const auto rows = node.table->exact_row_count();
        if (!rows) {
          return arrow::Status::Invalid("RowCount over a table without an exact row count");
        }
        return std::make_unique<RowCountOperator>(plan.output.front().name, *rows);
      }},
      *plan.root);
}

}  // namespace antb1::exec
