#pragma once

#include <memory>
#include <vector>

#include <arrow/type_fwd.h>

#include "key_table.h"

namespace antb1::exec {

// The generic column store takes any positive number of the engine's fixed-width key types,
// including bit-packed BOOLEAN. Input is a batch of arrays, with DOUBLE already normalized.
bool TakesColumnKeys(const std::vector<arrow::TypeHolder>& types);

// Explicit fixed candidates use this when the one/two-key packed layout does not apply. The
// generic index grows on insertion and rebuilds its buckets in group-ID order for either kind.
std::unique_ptr<KeyTable> MakeColumnKeyTable(const std::vector<arrow::TypeHolder>& types,
                                             arrow::MemoryPool* pool,
                                             const KeyTableOptions& options);

}  // namespace antb1::exec
