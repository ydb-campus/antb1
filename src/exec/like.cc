#include "antb1/exec/like.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <arrow/api.h>

#include "antb1/common/utf8.h"

namespace antb1::exec {
namespace {

// The length of the character at text[i] (i < text.size()): its UTF-8 sequence, or one byte.
std::size_t CharLength(std::string_view text, std::size_t i) {
  if (static_cast<unsigned char>(text[i]) < 0x80) {
    return 1;
  }
  const std::size_t length = Utf8SequenceLength(text, i);
  return length == 0 ? 1 : length;
}

}  // namespace

LikePattern::LikePattern(std::string_view pattern) : pattern_(pattern) {
  has_underscore_ = pattern.contains('_');
  leading_percent_ = pattern.starts_with('%');
  trailing_percent_ = pattern.ends_with('%');
  std::size_t start = 0;
  while (start <= pattern.size()) {
    const std::size_t end = std::min(pattern.find('%', start), pattern.size());
    if (end > start) {
      segments_.emplace_back(pattern.substr(start, end - start));
    }
    start = end + 1;
  }
}

bool LikePattern::Matches(std::string_view text) const {
  return has_underscore_ ? MatchGeneral(text) : MatchSegments(text);
}

bool LikePattern::MatchSegments(std::string_view text) const {
  if (!pattern_.contains('%')) {
    return text == pattern_;
  }
  std::size_t first = 0;
  std::size_t last = segments_.size();
  std::size_t pos = 0;            // where the next segment may start
  std::size_t end = text.size();  // where the segments before the last one must end
  if (!leading_percent_) {
    if (!text.starts_with(segments_.front())) {
      return false;
    }
    pos = segments_.front().size();
    ++first;
  }
  if (!trailing_percent_) {
    const std::string& suffix = segments_.back();
    if (suffix.size() > text.size() - pos || !text.ends_with(suffix)) {
      return false;
    }
    end = text.size() - suffix.size();
    --last;
  }
  const std::string_view window = text.substr(0, end);
  for (std::size_t i = first; i < last; ++i) {
    const std::size_t found = window.find(segments_[i], pos);
    if (found == std::string_view::npos) {
      return false;
    }
    pos = found + segments_[i].size();
  }
  return true;
}

bool LikePattern::MatchGeneral(std::string_view text) const {
  const std::string_view pattern = pattern_;
  std::size_t p = 0;
  std::size_t t = 0;
  std::size_t star = std::string_view::npos;  // the last `%` seen
  std::size_t star_text = 0;                  // where the text after it is tried from
  while (t < text.size()) {
    if (p < pattern.size() && pattern[p] == '%') {
      star = p++;
      star_text = t;
    } else if (p < pattern.size() && pattern[p] == '_') {
      t += CharLength(text, t);
      ++p;
    } else if (p < pattern.size() && pattern[p] == text[t]) {
      ++p;
      ++t;
    } else if (star != std::string_view::npos) {
      // The `%` takes one more character, and the rest of the pattern is tried after it.
      star_text += CharLength(text, star_text);
      t = star_text;
      p = star + 1;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '%') {
    ++p;
  }
  return p == pattern.size();
}

arrow::Result<std::shared_ptr<arrow::BooleanArray>> LikePattern::Evaluate(
    const arrow::BinaryArray& values, bool negated, arrow::MemoryPool* pool) const {
  arrow::BooleanBuilder builder(pool);
  ARROW_RETURN_NOT_OK(builder.Reserve(values.length()));
  for (int64_t i = 0; i < values.length(); ++i) {
    if (values.IsNull(i)) {
      builder.UnsafeAppendNull();
    } else {
      builder.UnsafeAppend(Matches(values.GetView(i)) != negated);
    }
  }
  std::shared_ptr<arrow::BooleanArray> out;
  ARROW_RETURN_NOT_OK(builder.Finish(&out));
  return out;
}

}  // namespace antb1::exec
