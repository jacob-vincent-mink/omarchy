#include "../../tests/support/test_assert.hpp"
#include "../../tests/support/temporary_directory.hpp"

#include "omarchy/plugin_runtime/providers/private_storage_backend.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace providers = omarchy::plugin_runtime::providers;

namespace {
using omarchy::plugin_runtime::test_support::require;
} // namespace

int main() {
  omarchy::plugin_runtime::test_support::TemporaryDirectory fixture;
  const auto &root = fixture.path();
  const int directory = open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  OMARCHY_CHECK(directory >= 0);
  providers::PrivateStorageBackend storage(directory, 8, 6);
  close(directory);
  OMARCHY_CHECK(storage.valid());
  const std::array value{std::byte{1}, std::byte{2}, std::byte{3}};
  OMARCHY_CHECK(storage.write("widget-state", value));
  std::array<std::byte, 8> output{};
  std::size_t written = 0;
  bool found = false;
  OMARCHY_CHECK(storage.read("widget-state", output, written, found) &&
              found && written == value.size() && output[2] == value[2]);
  const std::array second{std::byte{4}, std::byte{5}, std::byte{6},
                          std::byte{7}, std::byte{8}, std::byte{9}};
  OMARCHY_CHECK(!storage.write("other", second));
  const int fixture_directory =
      open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  OMARCHY_CHECK(fixture_directory >= 0 &&
              symlinkat("/etc/passwd", fixture_directory, "escape") == 0);
  close(fixture_directory);
  OMARCHY_CHECK(!storage.read("escape", output, written, found));
  OMARCHY_CHECK(!storage.write("../escape", value));
  OMARCHY_CHECK(!storage.write(".omarchy-tmp-bypass", value));
  OMARCHY_CHECK(mkfifo((root / "fifo").c_str(), 0600) == 0 &&
              !storage.read("fifo", output, written, found));
  OMARCHY_CHECK(storage.remove("widget-state"));
  OMARCHY_CHECK(storage.read("widget-state", output, written, found) &&
              !found);
}
