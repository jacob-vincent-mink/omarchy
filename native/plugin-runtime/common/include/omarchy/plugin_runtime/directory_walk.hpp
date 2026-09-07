#pragma once

#include "unique_fd.hpp"
#include "file_metadata.hpp"
#include <fcntl.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace omarchy::plugin_runtime {

enum class DirectoryOpenResult { opened, absent, rejected };

// All directories require trusted ownership and no group/world write bits.
// An optional exact mode applies only to a named leaf, never an empty walk.
// Failure leaves the caller's output untouched.
inline DirectoryOpenResult walk_directory(
    int root, std::span<const std::string_view> components, std::uint32_t uid,
    UniqueFd &output, std::optional<mode_t> leaf_mode = std::nullopt) {
  UniqueFd current(::openat(root, ".",
                           O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  struct stat metadata{};
  if (!current || ::fstat(current.get(), &metadata) < 0 ||
      !secure_directory(metadata, uid))
    return DirectoryOpenResult::rejected;
  for (std::size_t index = 0; index < components.size(); ++index) {
    const auto component = components[index];
    if (component.empty() || component == "." || component == ".." ||
        component.find('/') != std::string_view::npos)
      return DirectoryOpenResult::rejected;
    const std::string name(component);
    UniqueFd next(::openat(current.get(), name.c_str(),
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!next)
      return errno == ENOENT ? DirectoryOpenResult::absent
                            : DirectoryOpenResult::rejected;
    if (::fstat(next.get(), &metadata) < 0 ||
        !secure_directory(metadata, uid) ||
        (index + 1 == components.size() && leaf_mode &&
         (metadata.st_mode & 07777) != *leaf_mode))
      return DirectoryOpenResult::rejected;
    current = std::move(next);
  }
  output = std::move(current);
  return DirectoryOpenResult::opened;
}

} // namespace omarchy::plugin_runtime
