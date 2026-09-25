#include "antb1/common/version.h"

#include <string_view>

namespace antb1 {

std::string_view Version() { return ANTB1_VERSION_STRING; }

}  // namespace antb1
