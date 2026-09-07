#pragma once

#include "omarchy/plugin_runtime/unique_fd.hpp"

#include <fcntl.h>
#include <sys/stat.h>

#include <string>
#include <string_view>
#include <utility>

namespace omarchy::plugin_runtime::provider_host::detail {

inline bool canonical_absolute_path(std::string_view path) {
  if (path.size() < 2 || path.size() > 4096 || path.front() != '/' ||
      path.back() == '/' || path.find('\0') != std::string_view::npos)
    return false;
  std::size_t begin = 1;
  while (begin < path.size()) {
    const auto end = path.find('/', begin);
    const auto component = path.substr(
        begin, end == std::string_view::npos ? path.size() - begin : end - begin);
    if (component.empty() || component == "." || component == "..")
      return false;
    begin = end == std::string_view::npos ? path.size() : end + 1;
  }
  return true;
}

// Callers retain distinct root selection, ownership and leaf-file policies.
template <typename DirectoryPolicy, typename FilePolicy>
UniqueFd open_secure_path(int root_fd, const char *root_name,
                          std::string_view path, DirectoryPolicy directory,
                          FilePolicy file) {
  if (!canonical_absolute_path(path))
    return {};
  UniqueFd current(::openat(root_fd, root_name,
                           O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  struct stat metadata {};
  if (!current || ::fstat(current.get(), &metadata) < 0 || !directory(metadata))
    return {};
  std::size_t begin = 1;
  while (begin < path.size()) {
    const auto end = path.find('/', begin);
    const std::string name(path.substr(
        begin, end == std::string_view::npos ? path.size() - begin : end - begin));
    const bool leaf = end == std::string_view::npos;
    UniqueFd next(::openat(
        current.get(), name.c_str(),
        (leaf ? O_RDONLY | O_NONBLOCK : O_RDONLY | O_DIRECTORY) | O_CLOEXEC |
            O_NOFOLLOW));
    if (!next || ::fstat(next.get(), &metadata) < 0)
      return {};
    if (leaf)
      return file(metadata) ? std::move(next) : UniqueFd{};
    if (!directory(metadata))
      return {};
    current = std::move(next);
    begin = end + 1;
  }
  return {};
}

} // namespace omarchy::plugin_runtime::provider_host::detail
