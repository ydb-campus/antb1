#include "fixed_key_table.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <arrow/memory_pool.h>
#include <arrow/type.h>

#include "antb1/common/check.h"

#include "column_key_table.h"
#include "key_table.h"

namespace antb1::exec {
namespace {

// Whether a FixedKeyTable takes a key of type `id` (FixedKeyLayoutOf).
bool TakesKeyType(arrow::Type::type id) {
  switch (id) {
    case arrow::Type::INT16:
    case arrow::Type::UINT16:
    case arrow::Type::INT32:
    case arrow::Type::DATE32:
    case arrow::Type::INT64:
    case arrow::Type::TIMESTAMP:
    case arrow::Type::DOUBLE:
    case arrow::Type::DECIMAL64:
    case arrow::Type::DECIMAL128:
      return true;
    default:
      return false;
  }
}

template <Growth kGrowth>
std::unique_ptr<KeyTable> MakeGrowing(FixedKeyLayout layout, arrow::MemoryPool* pool,
                                      const KeyTableOptions& options, MemoryBudget* budget) {
  switch (layout.key_bytes) {
    case 8:
      return std::make_unique<FixedKeyTable<8, kGrowth>>(std::move(layout), pool, options, budget);
    case 16:
      return std::make_unique<FixedKeyTable<16, kGrowth>>(std::move(layout), pool, options, budget);
    case 24:
      return std::make_unique<FixedKeyTable<24, kGrowth>>(std::move(layout), pool, options, budget);
    case 32:
      return std::make_unique<FixedKeyTable<32, kGrowth>>(std::move(layout), pool, options, budget);
    default:
      ANTB1_CHECK(layout.key_bytes == 40);
      return std::make_unique<FixedKeyTable<40, kGrowth>>(std::move(layout), pool, options, budget);
  }
}

}  // namespace

bool TakesColumnKeys(const std::vector<arrow::TypeHolder>& types) {
  return !types.empty() && std::ranges::all_of(types, [](const arrow::TypeHolder& type) {
    return type.id() == arrow::Type::BOOL || TakesKeyType(type.id());
  });
}

std::optional<FixedKeyLayout> FixedKeyLayoutOf(const std::vector<arrow::TypeHolder>& types) {
  if (types.empty() || types.size() > 2) {
    return std::nullopt;
  }
  FixedKeyLayout layout;
  std::size_t offset = 0;
  for (const arrow::TypeHolder& type : types) {
    if (!TakesKeyType(type.id())) {
      return std::nullopt;
    }
    const auto width = static_cast<std::size_t>(type.type->byte_width());
    layout.columns.push_back({.type = type.GetSharedPtr(), .width = width, .offset = offset});
    offset += width;
  }
  if (types.size() == 1) {
    layout.key_bytes = offset <= 8 ? 8 : 16;
  } else {
    layout.mask_offset = offset;
    layout.key_bytes = (offset + 1 + 7) / 8 * 8;
  }
  return layout;
}

std::unique_ptr<KeyTable> MakeFixedKeyTable(FixedKeyLayout layout, Growth growth,
                                            arrow::MemoryPool* pool, const KeyTableOptions& options,
                                            MemoryBudget* budget) {
  return growth == Growth::kSlotOrder
             ? MakeGrowing<Growth::kSlotOrder>(std::move(layout), pool, options, budget)
             : MakeGrowing<Growth::kIdOrder>(std::move(layout), pool, options, budget);
}

}  // namespace antb1::exec
