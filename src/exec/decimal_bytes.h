// The physical type a DECIMAL or a HUGEINT value has inside the engine.
//
// Today it is decimal128 whatever the declared width: plan::ToArrow maps every DECIMAL(p,s) and
// HUGEINT to it. ADR 0021's N3 narrows p <= 18 to decimal64, and these two functions exist so that
// the narrowing happens here rather than at the twenty-odd sites that would otherwise each decide
// it for themselves.
//
// Until then they are a guard, and the reason they are worth having before the change rather than
// after is that neither hazard they cover announces itself:
//
//   * a `static_cast<const arrow::Decimal128Array&>` of a decimal64 array is not a failure, it is
//     a reinterpretation -- 8-byte values read as 16-byte ones, silently;
//   * `arrow::Decimal128Builder` takes its type as a parameter and checks it with
//     `checked_pointer_cast`, which under NDEBUG is a `static_pointer_cast`, so a release build
//     would write 16 bytes into an 8-byte slot rather than refuse.
//
// ANTB1_CHECK rather than a Status: a physical type that does not match the schema the planner
// built is a programming error, not an input the engine can meet. It costs one test per array or
// per builder, never one per value.
//
// Where it is NOT used, and why, because that is the more useful half of this comment:
// src/exec/compute.cc already gates its reads on the physical type in ReadableInPlace, and
// src/exec/sort.cc's comparators run per comparison while Bind checks the column's type once per
// sort key. Adding a second test in either measured a real cost for no extra coverage.

#ifndef ANTB1_EXEC_DECIMAL_BYTES_H_
#define ANTB1_EXEC_DECIMAL_BYTES_H_

#include <memory>
#include <utility>

#include <arrow/api.h>

#include "antb1/common/check.h"
#include "antb1/plan/types.h"

namespace antb1::exec {

// `array` as the decimal array the engine's decimal code reads.
inline const arrow::Decimal128Array& AsDecimals(const arrow::Array& array) {
  ANTB1_CHECK(array.type_id() == arrow::Type::DECIMAL128);
  return static_cast<const arrow::Decimal128Array&>(array);
}

// `array` as the fixed-width decimal array the engine reads, at either physical width: both
// decimal64 and decimal128 arrays are FixedSizeBinaryArrays, and which one it is decides the stride
// the caller reads with. The caller dispatches on array.type_id() once, never per value.
inline const arrow::FixedSizeBinaryArray& AsDecimalValues(const arrow::Array& array) {
  const bool decimal =
      array.type_id() == arrow::Type::DECIMAL128 || array.type_id() == arrow::Type::DECIMAL64;
  ANTB1_CHECK(decimal);
  return static_cast<const arrow::FixedSizeBinaryArray&>(array);
}

// The Arrow type to build `type`'s values with:
// `arrow::Decimal128Builder builder(DecimalsType(type), pool)`.
inline std::shared_ptr<arrow::DataType> DecimalsType(plan::LogicalType type) {
  std::shared_ptr<arrow::DataType> out = plan::ToArrow(type);
  ANTB1_CHECK(out->id() == arrow::Type::DECIMAL128);
  return out;
}

}  // namespace antb1::exec

#endif  // ANTB1_EXEC_DECIMAL_BYTES_H_
