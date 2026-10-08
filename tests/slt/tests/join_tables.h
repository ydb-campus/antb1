#pragma once

#include <vector>

#include "query_gen.h"

namespace antb1::slt {

// Tables with refs for the tests of generated joins (query_gen_test.cc, query_gen_oracle_test.cc),
// their key statistics written by hand: facts (3000 rows, by name or path) references dims
// through two keys (dims in two roles) and a DECIMAL one, codes (empty) and slots through a
// two-column key; dims (40 rows, a glob path) references itself. The statistics keep facts -> dims
// <- facts (largest multiplicities 75 and 70) and facts -> slots <- facts (21) above 10,000 rows,
// and facts -> dims -> dims, dims <- facts -> slots and dims <- dims <- facts below them, so a
// table stands twice in a join only as dims, or through codes (0 rows). slots has columns the
// generator skips and a path that names another table, codes a path of several dots that names
// it (ADR 0022 rule 1: the file name up to its first dot, leading dots skipped); `label` is a
// column of facts and of dims. The last refs of facts cannot be joined: a DOUBLE key, a table and
// a column that do not exist, DECIMAL(38,0) to (38,10) (48 common digits, D13) and INTEGER to
// VARCHAR.
std::vector<GenTable> JoinTables();

}  // namespace antb1::slt
