#include "clickbench.h"

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine.h"
#include "query_file.h"
#include "ratchet.h"

namespace antb1::slt {
namespace {

constexpr Workload kClickBench{
    .summary = "CLICKBENCH",
    .commit_key = "clickbench_commit",
    .first = 0,
    .rejections_clean = true,
    .unclean_rule =
        "a query outside the supported subset must fail cleanly (Unsupported, a "
        "parse or a bind error)",
    .table = "the ClickBench status table",
    .running_lines = false,
};

}  // namespace

std::expected<ClickBenchStatus, std::string> ParseClickBenchStatus(std::string_view json) {
  auto status = ParseRatchetStatus(json, kClickBench);
  if (!status.has_value()) {
    return std::unexpected(status.error());
  }
  return ClickBenchStatus{.commit = std::move(status->commit), .pass = std::move(status->pass)};
}

ClickBenchStats RunClickBench(const std::vector<Statement>& queries, const ClickBenchStatus& status,
                              Engine& antb1, Engine& oracle, const ClickBenchOptions& options,
                              std::string& out) {
  const RatchetStats stats =
      RunRatchet(queries, RatchetStatus{.commit = status.commit, .pass = status.pass}, kClickBench,
                 antb1, oracle,
                 RatchetOptions{.redact = options.redact,
                                .only = options.only,
                                .status_path = options.status_path,
                                .command = options.command,
                                .clock = {}},
                 [&out](std::string_view text) { out += text; });
  return ClickBenchStats{.queries = stats.queries,
                         .passed = stats.passed,
                         .unsupported = stats.unsupported,
                         .rejected = stats.rejected,
                         .failed = stats.failed};
}

}  // namespace antb1::slt
