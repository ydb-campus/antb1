#include "antb1/plan/explain.h"

#include <format>
#include <string>
#include <variant>

namespace antb1::plan {
namespace {

template <class... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};

}  // namespace

std::string Explain(const LogicalPlan& plan) {
  std::string out = "Output:";
  for (const auto& col : plan.output) {
    out += std::format(" {}:{}", col.name, ToString(col.type));
  }
  out += '\n';
  out += std::visit(Overloaded{[](const RowCountNode& node) {
                      return std::format("RowCount table={} source={}\n", node.table_name,
                                         node.table->Describe());
                    }},
                    *plan.root);
  return out;
}

}  // namespace antb1::plan
