#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <arrow/array/array_binary.h>
#include <arrow/array/array_primitive.h>
#include <arrow/memory_pool.h>
#include <arrow/result.h>

namespace antb1::exec {

// A LIKE pattern with DuckDB's semantics: `%` matches any sequence of characters (none included),
// `_` exactly one character, every other byte itself (there is no escape character: `\` is a
// literal, and the comparison is case-sensitive). A character is a well-formed UTF-8 sequence, or
// one byte where the bytes are not UTF-8.
class LikePattern {
 public:
  explicit LikePattern(std::string_view pattern);

  [[nodiscard]] bool Matches(std::string_view text) const;

  // Matches of every value of `values` (NOT LIKE when `negated`): NULL where the value is NULL.
  [[nodiscard]] arrow::Result<std::shared_ptr<arrow::BooleanArray>> Evaluate(
      const arrow::BinaryArray& values, bool negated, arrow::MemoryPool* pool) const;

 private:
  // A pattern without `_` is its literal segments between the `%`s: the text starts with the
  // first, ends with the last (when the pattern does not start or end with `%`), and has the others
  // in order in between. Leftmost matches decide it, so no backtracking is needed.
  [[nodiscard]] bool MatchSegments(std::string_view text) const;
  // Any pattern, `_` included: backtracking to the last `%`.
  [[nodiscard]] bool MatchGeneral(std::string_view text) const;

  std::string pattern_;
  bool has_underscore_ = false;
  bool leading_percent_ = false;
  bool trailing_percent_ = false;
  std::vector<std::string> segments_;  // the literal parts between `%`s (no empty ones)
};

}  // namespace antb1::exec
