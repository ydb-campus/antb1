#pragma once

// The round-trip property of the Arrow-free SQL front end, shared by the libFuzzer target
// (sql_parser_fuzzer.cc) and the corpus replay (replay_main.cc). For every byte string:
//   1. sql::Parse never crashes and has no UB (the fuzz builds run under ASan and UBSan);
//   2. if Parse succeeds, the canonical text ToSql(ast) parses again,
//   3. to a statement that is EqualIgnoringSpans to ast,
//   4. and ToSql is idempotent: ToSql(Parse(ToSql(ast))) == ToSql(ast).
// Rejecting an input (a syntax error or Unsupported) is never a violation.

#include <cstddef>
#include <cstdio>
#include <format>
#include <print>
#include <string>
#include <string_view>

#include "antb1/sql/ast.h"
#include "antb1/sql/parser.h"
#include "antb1/sql/unparse.h"

namespace antb1::fuzz {

// The unparser under test: sql::ToSql, except in the replay self-test (antb1-fuzz-replay
// --mutate-unparse), which injects a broken one to prove that violations are detected.
using Unparser = std::string (*)(const sql::SelectStatement&);

// `text` with every byte outside printable ASCII escaped as \xHH, so a report stays one readable
// line.
inline std::string Printable(std::string_view text, std::size_t max_bytes = 400) {
  std::string out;
  for (std::size_t i = 0; i < text.size() && i < max_bytes; ++i) {
    const auto byte = static_cast<unsigned char>(text[i]);
    if (byte < 0x20 || byte >= 0x7f || byte == '\\') {
      out += std::format("\\x{:02x}", byte);
    } else {
      out.push_back(text[i]);
    }
  }
  if (text.size() > max_bytes) {
    out += std::format("... ({} bytes)", text.size());
  }
  return out;
}

// Checks the property for one input. Returns an empty string when it holds, otherwise a description
// of the violation.
inline std::string SqlParserPropertyViolation(std::string_view input,
                                              Unparser unparse = &sql::ToSql) {
  const auto ast = sql::Parse(input);
  if (!ast) {
    return {};
  }
  const std::string text = unparse(*ast);
  const auto again = sql::Parse(text);
  if (!again) {
    return std::format("Parse(ToSql(ast)) failed: {}; ToSql(ast): {}", again.error().message,
                       Printable(text));
  }
  if (!sql::EqualIgnoringSpans(*ast, *again)) {
    return std::format("Parse(ToSql(ast)) is not EqualIgnoringSpans to ast; ToSql(ast): {}",
                       Printable(text));
  }
  const std::string text_again = unparse(*again);
  if (text_again != text) {
    return std::format("ToSql is not idempotent; ToSql(ast): {}; ToSql(Parse(ToSql(ast))): {}",
                       Printable(text), Printable(text_again));
  }
  return {};
}

// The fuzz target's check: a violation is reported and traps, so libFuzzer saves the input as a
// crash artifact.
inline void CheckSqlParserProperty(std::string_view input) {
  if (const std::string violation = SqlParserPropertyViolation(input); !violation.empty()) {
    std::println(stderr, "antb1 SQL parser property violated: {}\n  input: {}", violation,
                 Printable(input));
    __builtin_trap();
  }
}

}  // namespace antb1::fuzz
