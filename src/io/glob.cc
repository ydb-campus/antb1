#include "antb1/io/glob.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <arrow/result.h>
#include <arrow/status.h>

#include <fnmatch.h>

namespace antb1::io {

// Wildcards are supported in the last path component only (e.g. data/hits_*.parquet); this keeps
// the expansion simple, deterministic and thread-safe (glob(3) is not MT-safe).
arrow::Result<std::vector<std::string>> ExpandGlob(const std::string& pattern) {
  namespace fs = std::filesystem;
  if (pattern.find_first_of("*?[") == std::string::npos) {
    return std::vector<std::string>{pattern};
  }
  const fs::path path(pattern);
  const std::string dir = path.parent_path().string();
  if (dir.find_first_of("*?[") != std::string::npos) {
    return arrow::Status::Invalid("wildcards are only supported in the file name: '", pattern, "'");
  }
  const std::string name_pattern = path.filename().string();
  std::vector<std::string> files;
  std::error_code ec;
  for (const auto& entry :
       fs::directory_iterator(dir.empty() ? fs::path(".") : fs::path(dir), ec)) {
    const std::string name = entry.path().filename().string();
    if (entry.is_regular_file(ec) &&
        ::fnmatch(name_pattern.c_str(), name.c_str(), FNM_PERIOD) == 0) {
      files.push_back(dir.empty() ? name : (fs::path(dir) / name).string());
    }
  }
  if (ec) {
    return arrow::Status::IOError("cannot list '", dir, "': ", ec.message());
  }
  if (files.empty()) {
    return arrow::Status::IOError("no files match '", pattern, "'");
  }
  std::ranges::sort(files);
  return files;
}

}  // namespace antb1::io
