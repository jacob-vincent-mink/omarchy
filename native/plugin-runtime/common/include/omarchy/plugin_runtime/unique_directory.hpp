#pragma once

#include "unique_fd.hpp"
#include <dirent.h>
#include <fcntl.h>
#include <memory>

namespace omarchy::plugin_runtime {

struct DirectoryCloser {
  void operator()(DIR *directory) const noexcept { ::closedir(directory); }
};
using UniqueDirectory = std::unique_ptr<DIR, DirectoryCloser>;

// Reopen a held directory with an independent offset, never AT_FDCWD.
inline UniqueFd reopen_directory(int descriptor) noexcept {
  if (descriptor < 0)
    return {};
  return UniqueFd(::openat(
      descriptor, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
}

// Transfer descriptor ownership only when fdopendir succeeds.
inline UniqueDirectory take_directory(UniqueFd descriptor) noexcept {
  UniqueDirectory directory(::fdopendir(descriptor.get()));
  if (directory)
    (void)descriptor.release();
  return directory;
}

} // namespace omarchy::plugin_runtime
