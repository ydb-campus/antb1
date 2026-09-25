#pragma once

#include <memory>
#include <string>

#include <arrow/status.h>

#include "antb1/common/source_span.h"
#include "antb1/sql/error.h"

// The boundary between the Arrow-free front end (std::expected<..., sql::ParseError>) and the
// arrow::Status world of plan/exec/engine. Parse, unsupported and bind errors carry a
// SqlErrorDetail with the source span; the CLI uses it to choose the exit code
// (docs/adr/0005-error-boundary.md).

namespace antb1::plan {

class SqlErrorDetail final : public arrow::StatusDetail {
 public:
  enum class Kind { kParse, kUnsupported, kBind };

  SqlErrorDetail(Kind kind, SourceSpan span) : kind_(kind), span_(span) {}

  const char* type_id() const override { return "antb1::plan::SqlErrorDetail"; }
  std::string ToString() const override;

  [[nodiscard]] Kind kind() const { return kind_; }
  [[nodiscard]] SourceSpan span() const { return span_; }

 private:
  Kind kind_;
  SourceSpan span_;
};

arrow::Status ToArrowStatus(const sql::ParseError& error);
arrow::Status BindError(const std::string& message, SourceSpan span);
arrow::Status UnsupportedError(const std::string& message, SourceSpan span);

// The SqlErrorDetail attached to status, or nullptr.
std::shared_ptr<const SqlErrorDetail> GetSqlError(const arrow::Status& status);

}  // namespace antb1::plan
