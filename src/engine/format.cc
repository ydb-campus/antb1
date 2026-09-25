#include "antb1/engine/format.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <arrow/api.h>
#include <arrow/util/decimal.h>

namespace antb1::engine {
namespace {

std::string FormatDouble(double v) {
  if (std::isnan(v)) {
    return "nan";
  }
  if (std::isinf(v)) {
    return v > 0 ? "inf" : "-inf";
  }
  std::array<char, 64> buf{};
  auto [end, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), v);
  return ec == std::errc{} ? std::string(buf.data(), end) : std::format("{}", v);
}

std::string FormatDate(int32_t days) {
  const std::chrono::sys_days d{std::chrono::days{days}};
  const std::chrono::year_month_day ymd{d};
  return std::format("{:04}-{:02}-{:02}", static_cast<int>(ymd.year()),
                     static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()));
}

template <class ArrayType>
auto Value(const arrow::Array& column, int64_t row) {
  return static_cast<const ArrayType&>(column).Value(row);
}

bool IsJsonNumber(plan::LogicalType type) {
  return plan::IsNumeric(type) && type != plan::LogicalType::kHugeInt;
}

// JSON string body with control characters escaped and invalid UTF-8 bytes rendered as \xHH text.
std::string JsonEscape(std::string_view s) {
  std::string out;
  std::size_t i = 0;
  while (i < s.size()) {
    const unsigned c = static_cast<unsigned char>(s[i]);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
      ++i;
    } else if (c < 0x20U) {
      out += std::format("\\u{:04x}", c);
      ++i;
    } else if (c < 0x80U) {
      out += static_cast<char>(c);
      ++i;
    } else {
      std::size_t len = 0;
      if ((c & 0xE0U) == 0xC0U) {
        len = 2;
      } else if ((c & 0xF0U) == 0xE0U) {
        len = 3;
      } else if ((c & 0xF8U) == 0xF0U) {
        len = 4;
      }
      bool valid = len != 0 && i + len <= s.size();
      for (std::size_t k = 1; valid && k < len; ++k) {
        valid = (static_cast<unsigned>(static_cast<unsigned char>(s[i + k])) & 0xC0U) == 0x80U;
      }
      if (valid) {
        out.append(s.substr(i, len));
        i += len;
      } else {
        out += std::format("\\\\x{:02x}", c);
        ++i;
      }
    }
  }
  return out;
}

std::string CsvField(const std::string& s) {
  if (s.find_first_of(",\"\n\r") == std::string::npos) {
    return s;
  }
  std::string out = "\"";
  for (const char c : s) {
    out += c;
    if (c == '"') {
      out += '"';
    }
  }
  out += '"';
  return out;
}

}  // namespace

std::string FormatValue(const arrow::Array& column, int64_t row, plan::LogicalType type) {
  if (column.IsNull(row)) {
    return "NULL";
  }
  switch (column.type_id()) {
    case arrow::Type::INT16:
      return std::to_string(Value<arrow::Int16Array>(column, row));
    case arrow::Type::INT32:
      if (type == plan::LogicalType::kDate) {
        return FormatDate(Value<arrow::Int32Array>(column, row));
      }
      return std::to_string(Value<arrow::Int32Array>(column, row));
    case arrow::Type::INT64:
      return std::to_string(Value<arrow::Int64Array>(column, row));
    case arrow::Type::UINT16:
      return std::to_string(Value<arrow::UInt16Array>(column, row));
    case arrow::Type::DECIMAL128: {
      const arrow::Decimal128 v(static_cast<const arrow::Decimal128Array&>(column).GetValue(row));
      return v.ToIntegerString();
    }
    case arrow::Type::FLOAT:
      return FormatDouble(static_cast<double>(Value<arrow::FloatArray>(column, row)));
    case arrow::Type::DOUBLE:
      return FormatDouble(Value<arrow::DoubleArray>(column, row));
    case arrow::Type::DATE32:
      return FormatDate(Value<arrow::Date32Array>(column, row));
    case arrow::Type::BINARY:
      return std::string(static_cast<const arrow::BinaryArray&>(column).GetView(row));
    case arrow::Type::STRING:
      return std::string(static_cast<const arrow::StringArray&>(column).GetView(row));
    default: {
      auto scalar = column.GetScalar(row);
      return scalar.ok() ? (*scalar)->ToString() : "?";
    }
  }
}

arrow::Result<std::string> FormatResult(const QueryResult& result, OutputFormat format) {
  ARROW_ASSIGN_OR_RAISE(auto table, result.table->CombineChunks());
  const int ncols = table->num_columns();
  const int64_t nrows = table->num_rows();
  if (static_cast<std::size_t>(ncols) != result.types.size() ||
      static_cast<std::size_t>(ncols) != result.names.size()) {
    return arrow::Status::Invalid("result metadata does not match the table");
  }
  std::vector<std::vector<std::string>> cells(static_cast<std::size_t>(nrows));
  for (int64_t r = 0; r < nrows; ++r) {
    auto& row = cells[static_cast<std::size_t>(r)];
    for (int c = 0; c < ncols; ++c) {
      row.push_back(
          FormatValue(*table->column(c)->chunk(0), r, result.types[static_cast<std::size_t>(c)]));
    }
  }
  std::string out;
  switch (format) {
    case OutputFormat::kCsv: {
      for (int c = 0; c < ncols; ++c) {
        out += (c > 0 ? "," : "") + CsvField(result.names[static_cast<std::size_t>(c)]);
      }
      out += '\n';
      for (int64_t r = 0; r < nrows; ++r) {
        for (int c = 0; c < ncols; ++c) {
          const bool null = table->column(c)->chunk(0)->IsNull(r);
          const auto& cell = cells[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)];
          out += (c > 0 ? "," : "") + (null ? std::string() : CsvField(cell));
        }
        out += '\n';
      }
      return out;
    }
    case OutputFormat::kJson: {
      out += '[';
      for (int64_t r = 0; r < nrows; ++r) {
        out += r > 0 ? ",\n {" : "\n {";
        for (int c = 0; c < ncols; ++c) {
          const auto uc = static_cast<std::size_t>(c);
          const bool null = table->column(c)->chunk(0)->IsNull(r);
          const auto& cell = cells[static_cast<std::size_t>(r)][uc];
          const bool finite_number =
              IsJsonNumber(result.types[uc]) && cell != "nan" && cell != "inf" && cell != "-inf";
          out += std::format("{}\"{}\": ", c > 0 ? ", " : "", JsonEscape(result.names[uc]));
          if (null) {
            out += "null";
          } else if (finite_number) {
            out += cell;
          } else {
            out += "\"" + JsonEscape(cell) + "\"";
          }
        }
        out += '}';
      }
      out += nrows > 0 ? "\n]\n" : "]\n";
      return out;
    }
    case OutputFormat::kTable: {
      std::vector<std::size_t> width(static_cast<std::size_t>(ncols));
      for (int c = 0; c < ncols; ++c) {
        const auto uc = static_cast<std::size_t>(c);
        width[uc] = result.names[uc].size();
        for (const auto& row : cells) {
          width[uc] = std::max(width[uc], row[uc].size());
        }
      }
      auto line = [&](auto get) {
        for (int c = 0; c < ncols; ++c) {
          const auto uc = static_cast<std::size_t>(c);
          const std::string cell = get(uc);
          out += (c > 0 ? " | " : "") + cell + std::string(width[uc] - cell.size(), ' ');
        }
        while (!out.empty() && out.back() == ' ') {
          out.pop_back();
        }
        out += '\n';
      };
      line([&](std::size_t c) { return result.names[c]; });
      line([&](std::size_t c) { return std::string(width[c], '-'); });
      for (const auto& row : cells) {
        line([&](std::size_t c) { return row[c]; });
      }
      out += std::format("({} row{})\n", nrows, nrows == 1 ? "" : "s");
      return out;
    }
  }
  return arrow::Status::Invalid("unknown output format");
}

}  // namespace antb1::engine
