#include "../../tests/support/test_assert.hpp"
#include "../../tests/support/temporary_directory.hpp"

#include "discovery.hpp"
#include "omarchy/plugin_runtime/unique_directory.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace discovery = omarchy::plugins::discovery;

namespace {

using omarchy::plugin_runtime::test_support::throws_exception;

using omarchy::plugin_runtime::test_support::require;
using omarchy::plugin_runtime::test_support::TemporaryDirectory;

void copy_tree(const std::filesystem::path &source,
               const std::filesystem::path &destination) {
  std::filesystem::create_directories(destination);
  for (const auto &entry : std::filesystem::directory_iterator(source)) {
    std::filesystem::copy(
        entry.path(), destination / entry.path().filename(),
        std::filesystem::copy_options::recursive |
            std::filesystem::copy_options::overwrite_existing);
  }
}

template <typename Function>
void expect_rejected(Function &&function, std::string_view message) {
  require(throws_exception<std::exception>(function), message);
}

int open_directory(const std::filesystem::path &path) {
  return ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
}

void expect_tree_rejected(const std::filesystem::path &path,
                          std::string_view message) {
  omarchy::plugin_runtime::UniqueFd descriptor(open_directory(path));
  OMARCHY_CHECK(descriptor);
  expect_rejected(
      [&] { (void)discovery::discover_open_revision(descriptor.get()); }, message);
}

} // namespace

int main() {
  return omarchy::plugin_runtime::test_support::test_main([&] {
    expect_rejected(
        [] { (void)discovery::discover_open_revision(-1); },
        "descriptor discovery accepted an invalid descriptor");

    TemporaryDirectory verified_root;
    for (const bool is_directory : {false, true}) {
      const int descriptor = ::open(
          is_directory ? verified_root.path().c_str() : "/dev/null",
          O_RDONLY | O_CLOEXEC);
      OMARCHY_CHECK(descriptor >= 0);
      auto directory = omarchy::plugin_runtime::take_directory(
          omarchy::plugin_runtime::UniqueFd(descriptor));
      OMARCHY_CHECK(static_cast<bool>(directory) == is_directory);
      if (is_directory)
        OMARCHY_CHECK(::dirfd(directory.get()) == descriptor &&
                      ::fcntl(descriptor, F_GETFD) >= 0);
      directory.reset();
      OMARCHY_CHECK(::fcntl(descriptor, F_GETFD) == -1 && errno == EBADF);
    }
    const auto original_path = verified_root.path() / "secure";
    copy_tree(MANIFEST_V2_FIXTURE_ROOT, original_path);
    const int stable_fd = open_directory(original_path);
    OMARCHY_CHECK(stable_fd >= 0);
    const auto descriptor_result = discovery::discover_open_revision(stable_fd);
    OMARCHY_CHECK(descriptor_result.manifest.id == "org.example.status" &&
                descriptor_result.identity.tree_sha256 == TREE_SHA256_GOLDEN);
    OMARCHY_CHECK(discovery::discover_open_revision(stable_fd).identity ==
                descriptor_result.identity);

    const auto displaced_path = verified_root.path() / "displaced";
    std::filesystem::rename(original_path, displaced_path);
    copy_tree(MANIFEST_DUPLICATE_FIXTURE_ROOT, original_path);
    const auto after_replacement = discovery::discover_open_revision(stable_fd);
    OMARCHY_CHECK(after_replacement.identity == descriptor_result.identity &&
                after_replacement.manifest == descriptor_result.manifest);
    ::close(stable_fd);

    TemporaryDirectory special_root;
    const auto special_plugin = special_root.path() / "special";
    std::filesystem::create_directory(special_plugin);
    OMARCHY_CHECK(::mkfifo((special_plugin / "manifest.json").c_str(), 0600) == 0);
    expect_tree_rejected(special_plugin,
        "descriptor discovery admitted a special file");

    TemporaryDirectory symlink_root;
    copy_tree(MANIFEST_V2_FIXTURE_ROOT, symlink_root.path() / "linked");
    std::filesystem::create_symlink("ui/Status.qml",
                                    symlink_root.path() / "linked/escape");
    expect_tree_rejected(symlink_root.path() / "linked",
        "descriptor discovery admitted a symlink");

    TemporaryDirectory git_root;
    copy_tree(MANIFEST_V2_FIXTURE_ROOT, git_root.path() / "checkout");
    std::filesystem::create_directories(git_root.path() / "checkout/.git");
    std::ofstream(git_root.path() / "checkout/.git/config")
        << "untrusted metadata\n";
    expect_tree_rejected(git_root.path() / "checkout",
        "descriptor discovery excluded sandbox-visible .git content");

    TemporaryDirectory oversized_manifest_root;
    copy_tree(MANIFEST_V2_FIXTURE_ROOT,
              oversized_manifest_root.path() / "oversized");
    {
      std::ofstream manifest_file(oversized_manifest_root.path() /
                                  "oversized/manifest.json");
      manifest_file << std::string(1024 * 1024 + 1, ' ');
    }
    expect_tree_rejected(oversized_manifest_root.path() / "oversized",
        "descriptor discovery admitted an oversized manifest");

    TemporaryDirectory changing_root;
    copy_tree(MANIFEST_V2_FIXTURE_ROOT, changing_root.path() / "changing");
    {
      std::ofstream padding(changing_root.path() / "changing/padding");
      padding << std::string(8 * 1024 * 1024, 'x');
    }
    const int changing_fd = open_directory(changing_root.path() / "changing");
    OMARCHY_CHECK(changing_fd >= 0);
    std::atomic_bool mutate{true};
    std::atomic_bool first_mutation{false};
    std::jthread mutator([&] {
      const auto marker = changing_root.path() / "changing/mutation";
      while (mutate.load(std::memory_order_relaxed)) {
        std::ofstream(marker) << "changed";
        std::filesystem::remove(marker);
        first_mutation.store(true, std::memory_order_release);
      }
    });
    while (!first_mutation.load(std::memory_order_acquire))
      std::this_thread::yield();
    expect_rejected(
        [&] { (void)discovery::discover_open_revision(changing_fd); },
        "descriptor discovery admitted a concurrently changing tree");
    mutate.store(false, std::memory_order_relaxed);
    mutator.join();
    ::close(changing_fd);

    std::cout << "descriptor plugin discovery: PASS\n";
    return 0;
  }, "descriptor plugin discovery: FAIL: ");
}
