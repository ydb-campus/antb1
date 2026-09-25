#include "antb1/plan/sql_status.h"

#include <format>
#include <memory>
#include <string>

#include <arrow/status.h>

namespace antb1::plan {

std::string SqlErrorDetail::ToString() const {
  const char* kind = "bind";
  switch (kind_) {
    case Kind::kParse:
      kind = "parse";
      break;
    case Kind::kUnsupported:
      kind = "unsupported";
      break;
    case Kind::kBind:
      break;
  }
  return std::format("{} error at offset {} (length {})", kind, span_.offset, span_.length);
}

arrow::Status ToArrowStatus(const sql::ParseError& error) {
  if (error.kind == sql::ParseError::Kind::kUnsupported) {
    return UnsupportedError(error.message, error.span);
  }
  return arrow::Status::Invalid(error.message)
      .WithDetail(std::make_shared<SqlErrorDetail>(SqlErrorDetail::Kind::kParse, error.span));
}

arrow::Status BindError(const std::string& message, SourceSpan span) {
  return arrow::Status::Invalid(message).WithDetail(
      std::make_shared<SqlErrorDetail>(SqlErrorDetail::Kind::kBind, span));
}

arrow::Status UnsupportedError(const std::string& message, SourceSpan span) {
  return arrow::Status::NotImplemented(message).WithDetail(
      std::make_shared<SqlErrorDetail>(SqlErrorDetail::Kind::kUnsupported, span));
}

std::shared_ptr<const SqlErrorDetail> GetSqlError(const arrow::Status& status) {
  const auto& detail = status.detail();
  if (!detail || std::string_view(detail->type_id()) != "antb1::plan::SqlErrorDetail") {
    return nullptr;
  }
  return std::static_pointer_cast<const SqlErrorDetail>(detail);
}

}  // namespace antb1::plan
