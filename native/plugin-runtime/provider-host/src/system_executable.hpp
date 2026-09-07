#pragma once

#include <sys/stat.h>

namespace omarchy::plugin_runtime::provider_host::detail {

// For providers' fixed /usr/bin targets, not arbitrary plugin-selected paths.
inline bool secure_system_executable(const char *path) {
  const auto safe = [](const char *name, bool directory) {
    struct stat metadata {};
    if (::lstat(name, &metadata) != 0 || (metadata.st_mode & 0022) != 0)
      return false;
#if !defined(OMARCHY_DESKTOP_OPEN_TESTING) && !defined(OMARCHY_SYSTEM_OBSERVE_TESTING)
    if (metadata.st_uid != 0)
      return false;
#endif
    return directory ? S_ISDIR(metadata.st_mode)
                     : S_ISREG(metadata.st_mode) &&
                           (metadata.st_mode & 06000) == 0 &&
                           (metadata.st_mode & 0100) != 0;
  };
  return safe("/", true) && safe("/usr", true) && safe("/usr/bin", true) &&
         safe(path, false);
}

} // namespace omarchy::plugin_runtime::provider_host::detail
