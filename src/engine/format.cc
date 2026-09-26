#include "antb1/engine/format.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <arrow/api.h>
#include <arrow/util/decimal.h>

#include "antb1/plan/literal.h"

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

template <class ArrayType>
auto Value(const arrow::Array& column, int64_t row) {
  return static_cast<const ArrayType&>(column).Value(row);
}

bool IsJsonNumber(plan::LogicalType type) {
  return plan::IsNumeric(type) && type != plan::LogicalType::kHugeInt;
}

// Length of the well-formed UTF-8 sequence that starts with the non-ASCII byte s[i], or 0 if the
// bytes there are ill-formed: RFC 3629 and Table 3-7 of the Unicode standard, which rule out
// overlong forms, surrogates and code points above U+10FFFF.
std::size_t Utf8SequenceLength(std::string_view s, std::size_t i) {
  // A byte past the end reads as 0, which is never a continuation byte.
  const auto byte = [&](std::size_t k) {
    return i + k < s.size() ? static_cast<unsigned>(static_cast<unsigned char>(s[i + k])) : 0U;
  };
  const unsigned lead = byte(0);
  std::size_t len = 0;
  unsigned lo = 0x80U;  // range of the second byte
  unsigned hi = 0xBFU;
  if (lead >= 0xC2U && lead <= 0xDFU) {
    len = 2;
  } else if (lead >= 0xE0U && lead <= 0xEFU) {
    len = 3;
    lo = lead == 0xE0U ? 0xA0U : lo;  // E0 80..9F: overlong
    hi = lead == 0xEDU ? 0x9FU : hi;  // ED A0..BF: surrogates
  } else if (lead >= 0xF0U && lead <= 0xF4U) {
    len = 4;
    lo = lead == 0xF0U ? 0x90U : lo;  // F0 80..8F: overlong
    hi = lead == 0xF4U ? 0x8FU : hi;  // F4 90..BF: above U+10FFFF
  } else {
    return 0;  // a continuation byte, C0, C1 or F5..FF never starts a sequence
  }
  if (byte(1) < lo || byte(1) > hi) {
    return 0;
  }
  for (std::size_t k = 2; k < len; ++k) {
    if ((byte(k) & 0xC0U) != 0x80U) {
      return 0;
    }
  }
  return len;
}

}  // namespace

// An ill-formed sequence is escaped one byte at a time: the bytes after its first either start a
// well-formed sequence or are escaped in turn (a continuation byte never starts one).
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
    } else if (const std::size_t len = Utf8SequenceLength(s, i); len != 0) {
      out.append(s.substr(i, len));
      i += len;
    } else {
      out += std::format("\\\\x{:02x}", c);
      ++i;
    }
  }
  return out;
}

namespace {

// The canonical text of a value ("NULL" for NULL) and whether it is NULL, which csv and json
// print differently from a VARCHAR whose bytes are "NULL".
struct Cell {
  std::string text;
  bool null = false;
};

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
        return plan::FormatDate(Value<arrow::Int32Array>(column, row));
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
      return plan::FormatDate(Value<arrow::Date32Array>(column, row));
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
  const arrow::Table& table = *result.table;
  const int ncols = table.num_columns();
  const int64_t nrows = table.num_rows();
  if (static_cast<std::size_t>(ncols) != result.types.size() ||
      static_cast<std::size_t>(ncols) != result.names.size()) {
    return arrow::Status::Invalid("result metadata does not match the table");
  }
  // Column by column, chunk by chunk: a result has one chunk per batch, and even CombineChunks
  // leaves a binary column above 2 GiB in several chunks.
  std::vector<std::vector<Cell>> cells(static_cast<std::size_t>(nrows),
                                       std::vector<Cell>(static_cast<std::size_t>(ncols)));
  for (int c = 0; c < ncols; ++c) {
    const auto uc = static_cast<std::size_t>(c);
    const arrow::ChunkedArray& column = *table.column(c);
    if (column.length() != nrows) {
      return arrow::Status::Invalid("result column length does not match the table");
    }
    std::size_t r = 0;
    for (const auto& chunk : column.chunks()) {
      for (int64_t i = 0; i < chunk->length(); ++i, ++r) {
        cells[r][uc] = {.text = FormatValue(*chunk, i, result.types[uc]), .null = chunk->IsNull(i)};
      }
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
          const auto& cell = cells[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)];
          out += (c > 0 ? "," : "") + (cell.null ? std::string() : CsvField(cell.text));
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
          const auto& [text, null] = cells[static_cast<std::size_t>(r)][uc];
          const bool finite_number =
              IsJsonNumber(result.types[uc]) && text != "nan" && text != "inf" && text != "-inf";
          out += std::format("{}\"{}\": ", c > 0 ? ", " : "", JsonEscape(result.names[uc]));
          if (null) {
            out += "null";
          } else if (finite_number) {
            out += text;
          } else {
            out += "\"" + JsonEscape(text) + "\"";
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
          width[uc] = std::max(width[uc], row[uc].text.size());
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
        line([&](std::size_t c) { return row[c].text; });
      }
      out += std::format("({} row{})\n", nrows, nrows == 1 ? "" : "s");
      return out;
    }
  }
  return arrow::Status::Invalid("unknown output format");
}

}  // namespace antb1::engine
