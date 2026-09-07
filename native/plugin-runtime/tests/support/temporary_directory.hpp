#pragma once

#include "test_assert.hpp"
#include "omarchy/plugin_runtime/unique_fd.hpp"

#include <fcntl.h>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace omarchy::plugin_runtime::test_support {

inline std::string read_file(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

// Shared fixture plumbing: each test still chooses its files, permissions, and
// malformed inputs. Never follow a fixture symlink while repairing modes.
class TemporaryDirectory {
public:
  TemporaryDirectory() {
    std::string pattern = "/tmp/omarchy-test.XXXXXX";
    const auto *created = ::mkdtemp(pattern.data());
    require(created != nullptr, "cannot create temporary test directory");
    root_ = created;
  }
  TemporaryDirectory(const TemporaryDirectory &) = delete;
  TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;
  ~TemporaryDirectory() {
    std::error_code error;
    const auto writable = [&](const std::filesystem::path &path) {
      if (std::filesystem::is_directory(std::filesystem::symlink_status(path, error)))
        std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::add, error);
    };
    if (std::filesystem::is_directory(std::filesystem::symlink_status(root_, error))) {
      writable(root_);
      for (std::filesystem::recursive_directory_iterator it(root_, error), end;
           !error && it != end; it.increment(error))
        writable(it->path());
    }
    std::filesystem::remove_all(root_, error);
  }
  const std::filesystem::path &path() const { return root_; }

  static void write_file(const std::filesystem::path &path, std::string_view bytes,
                         int flags, mode_t mode = 0600) {
    UniqueFd file(::open(path.c_str(), flags, mode));
    OMARCHY_CHECK(file);
    while (!bytes.empty()) {
      const auto count = ::write(file.get(), bytes.data(), bytes.size());
      if (count < 0 && errno == EINTR)
        continue;
      OMARCHY_CHECK(count > 0);
      bytes.remove_prefix(static_cast<std::size_t>(count));
    }
    OMARCHY_CHECK(::close(file.release()) == 0);
  }

protected:
  std::filesystem::path root_;
};

} // namespace omarchy::plugin_runtime::test_support
