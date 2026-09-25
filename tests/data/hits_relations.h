#pragma once

#include <vector>

#include "relations.h"

// The metamorphic relations of the ClickBench data tests (data.hits0.metamorphic): our own queries
// over the `hits` table (ClickBench's column names, never its query text), judged with the checks
// and the active/pending rules of tests/metamorphic/relations.h. The table is hits_0.parquet, or
// the files of ANTB1_HITS_FILES (cmake/scripts/DataPaths.cmake), with EventDate read as DATE.
// Relations over the real data catch what the small fixtures cannot: many row groups and pages,
// real value distributions, 64-bit sums beyond the int64 range.

namespace antb1::metamorphic {

std::vector<Relation> HitsRelations();

}  // namespace antb1::metamorphic
