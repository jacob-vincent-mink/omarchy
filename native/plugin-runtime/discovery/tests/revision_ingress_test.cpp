#include "../../tests/support/child_process.hpp"
#include "../../tests/support/test_assert.hpp"
#include "../../tests/support/temporary_directory.hpp"
#include "omarchy/plugin_runtime/unique_fd.hpp"
#include "revision_ingress.hpp"
#include "authoring_manifest.hpp"
#include "../../tests/support/capability_fixture.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <barrier>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace discovery = omarchy::plugins::discovery;
namespace definitions = omarchy::plugins::definitions;

namespace {

using omarchy::plugin_runtime::test_support::expect_child_exit;

using omarchy::plugin_runtime::test_support::throws_exception;

struct Entry {
  std::string path;
  char type = '0';
  unsigned mode = 0644;
  std::string bytes;
  std::string link;
  std::uint64_t declared_size = 0;
};

using omarchy::plugin_runtime::UniqueFd;

class TempDirectory final : public omarchy::plugin_runtime::test_support::TemporaryDirectory {
public:
  TempDirectory()
      : descriptor_(::open(path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)) {
    omarchy::plugin_runtime::test_support::require(descriptor_.get() >= 0,
                                                  "open temp root failed");
  }
  int fd() const { return descriptor_.get(); }
private:
  UniqueFd descriptor_;
};

void check(bool condition, std::string_view message) {
  if (!condition)
    throw std::runtime_error(std::string(message));
}

void put_octal(char *output, std::size_t size, std::uint64_t value) {
  std::memset(output, '0', size);
  output[size - 1] = '\0';
  for (std::size_t index = size - 1; index-- > 0 && value != 0;) {
    output[index] = static_cast<char>('0' + (value & 7));
    value >>= 3;
  }
  OMARCHY_CHECK(value == 0);
}

void write_all(int fd, const void *bytes, std::size_t size) {
  const auto *cursor = static_cast<const char *>(bytes);
  while (size != 0) {
    const auto count = ::write(fd, cursor, size);
    if (count < 0 && errno == EINTR)
      continue;
    OMARCHY_CHECK(count > 0);
    cursor += count;
    size -= static_cast<std::size_t>(count);
  }
}

UniqueFd archive(const std::vector<Entry> &entries) {
  char path[] = "/tmp/omarchy-ingress-archive-XXXXXX";
  UniqueFd fd(::mkstemp(path));
  OMARCHY_CHECK(fd.get() >= 0);
  ::unlink(path);
  for (const auto &entry : entries) {
    std::array<char, 512> header{};
    std::string name = entry.path;
    std::string prefix;
    if (name.size() > 100) {
      const auto split = name.rfind('/');
      OMARCHY_CHECK(split != std::string::npos && split <= 155 &&
                name.size() - split - 1 <= 100);
      prefix = name.substr(0, split);
      name.erase(0, split + 1);
    }
    OMARCHY_CHECK(name.size() <= 100 && prefix.size() <= 155);
    std::memcpy(header.data(), name.data(), name.size());
    put_octal(header.data() + 100, 8, entry.mode);
    put_octal(header.data() + 108, 8, 12345);
    put_octal(header.data() + 116, 8, 54321);
    const auto size = entry.declared_size == 0 ? entry.bytes.size()
                                                : entry.declared_size;
    put_octal(header.data() + 124, 12, size);
    put_octal(header.data() + 136, 12, 1);
    std::memset(header.data() + 148, ' ', 8);
    header[156] = entry.type;
    std::memcpy(header.data() + 157, entry.link.data(), entry.link.size());
    std::memcpy(header.data() + 257, "ustar\0", 6);
    std::memcpy(header.data() + 263, "00", 2);
    std::memcpy(header.data() + 345, prefix.data(), prefix.size());
    unsigned checksum = 0;
    for (const unsigned char byte : header)
      checksum += byte;
    std::snprintf(header.data() + 148, 8, "%06o", checksum);
    header[154] = '\0';
    header[155] = ' ';
    write_all(fd.get(), header.data(), header.size());
    if (!entry.bytes.empty())
      write_all(fd.get(), entry.bytes.data(), entry.bytes.size());
    const std::array<char, 512> zero{};
    const auto padding = (512 - (entry.bytes.size() % 512)) % 512;
    if (padding != 0)
      write_all(fd.get(), zero.data(), padding);
  }
  const std::array<char, 1024> end{};
  write_all(fd.get(), end.data(), end.size());
  OMARCHY_CHECK(::lseek(fd.get(), 0, SEEK_SET) == 0);
  return fd;
}

constexpr std::string_view manifest = R"({
  "schemaVersion": 2,
  "id": "org.example.ingress",
  "name": "Ingress",
  "version": "1.0.0",
  "runtime": {"apiVersion": 1, "qml": "ui/Main.qml"},
  "surfaces": {"overlay": {"role": "overlay"}},
  "permissions": {"required": [], "optional": []}
})";

std::vector<Entry> valid_entries() {
  Entry directory;
  directory.path = "ui/";
  directory.type = '5';
  directory.mode = 0755;
  Entry manifest_entry;
  manifest_entry.path = "manifest.json";
  manifest_entry.bytes = manifest;
  Entry qml;
  qml.path = "ui/Main.qml";
  qml.bytes = "import QtQuick\nItem {}\n";
  return {std::move(directory), std::move(manifest_entry), std::move(qml)};
}

std::vector<Entry> author_entries() {
  auto entries = valid_entries();
  entries[1].path = "manifest.author.json";
  entries[1].bytes = R"({
    "authoringVersion":1,"id":"org.example.ingress","name":"Author ingress",
    "version":"1","runtime":{"apiVersion":1,"qml":"ui/Main.qml"},
    "surfaces":{"overlay":{"role":"overlay"}},
    "permissions":{"required":[{"capability":"storage.private",
      "definitionVersions":{"minimum":1,"maximum":2},
      "operations":["read"],"quotaBytes":4096,"itemBytes":1024,
      "reason":"Restore state"}],"optional":[]}
  })";
  return entries;
}

std::string published_text(int root, const char *name) {
  UniqueFd file(::openat(root, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  struct stat metadata{};
  OMARCHY_CHECK(file && ::fstat(file.get(), &metadata) == 0 &&
              S_ISREG(metadata.st_mode) && metadata.st_nlink == 1 &&
              (metadata.st_mode & 07777) == 0444 && metadata.st_size >= 0 &&
              metadata.st_size <= 1024 * 1024);
  std::string bytes(static_cast<std::size_t>(metadata.st_size), '\0');
  OMARCHY_CHECK(::read(file.get(), bytes.data(), bytes.size()) == metadata.st_size);
  return bytes;
}

void authoring_publication() {
  TempDirectory root;
  const auto registry = omarchy::plugin_runtime::test_support::packaged_registry();
  const auto entries = author_entries();
  auto input = archive(entries);
  const auto published = discovery::publish_revision_archive(
      input.get(), root.fd(), ::geteuid(), &registry);
  const auto expected = definitions::resolve_authoring_manifest_v1(entries[1].bytes, registry);
  OMARCHY_CHECK(published.verified().manifest == expected &&
              published_text(published.descriptor(), "manifest.author.json") == entries[1].bytes &&
              published_text(published.descriptor(), "manifest.json") == expected.canonical_json);
  OMARCHY_CHECK(discovery::discover_open_published_revision(published.descriptor(), ::geteuid()).identity ==
              published.verified().identity);
  auto retry_input = archive(entries);
  const auto retry = discovery::publish_revision_archive(
      retry_input.get(), root.fd(), ::geteuid(), &registry);
  OMARCHY_CHECK(retry.verified().identity == published.verified().identity);

  // Source provenance is part of the tree even if it resolves identically.
  auto changed_source = entries;
  changed_source[1].bytes += "\n";
  auto source_input = archive(changed_source);
  const auto changed = discovery::publish_revision_archive(
      source_input.get(), root.fd(), ::geteuid(), &registry);
  OMARCHY_CHECK(changed.verified().manifest == expected &&
              changed.verified().identity.tree_sha256 != published.verified().identity.tree_sha256 &&
              changed.verified().identity.request_sha256 == published.verified().identity.request_sha256);

  definitions::TrustedDefinitionRegistry newer;
  for (const auto &definition : definitions::packaged_definitions())
    OMARCHY_CHECK(newer.install(definition, 2));
  auto update_input = archive(entries);
  const auto update = discovery::publish_revision_archive(
      update_input.get(), root.fd(), ::geteuid(), &newer);
  OMARCHY_CHECK(update.verified().manifest.requests[0].definition_generation == 2 &&
              update.verified().identity.tree_sha256 != published.verified().identity.tree_sha256 &&
              update.verified().identity.request_sha256 != published.verified().identity.request_sha256);
  OMARCHY_CHECK(published.verified().manifest.requests[0].definition_generation == 1 &&
              published_text(published.descriptor(), "manifest.json") == expected.canonical_json);
}

void authoring_rejections() {
  const auto registry = omarchy::plugin_runtime::test_support::packaged_registry();
  const auto reject = [&](std::vector<Entry> entries,
                          const definitions::TrustedDefinitionRegistry *trusted) {
    TempDirectory root;
    auto input = archive(entries);
    OMARCHY_CHECK(throws_exception<std::runtime_error>([&] {
      (void)discovery::publish_revision_archive(input.get(), root.fd(), ::geteuid(), trusted);
    }));
    OMARCHY_CHECK(std::filesystem::is_empty(root.path()));
  };
  reject(author_entries(), nullptr);
  const definitions::TrustedDefinitionRegistry empty;
  reject(author_entries(), &empty);
  auto mixed = author_entries();
  mixed.push_back(valid_entries()[1]);
  reject(std::move(mixed), &registry);
  auto executable = author_entries();
  executable[1].mode = 0755;
  reject(std::move(executable), &registry);
  auto directory = author_entries();
  directory[1].path += '/';
  directory[1].type = '5';
  directory[1].bytes.clear();
  reject(std::move(directory), &registry);
  auto too_large = author_entries();
  too_large[1].bytes.resize(1024 * 1024 + 1, ' ');
  reject(std::move(too_large), &registry);
  auto malformed = author_entries();
  malformed[1].bytes = "{\"authoringVersion\":1,\"authoringVersion\":1}";
  reject(std::move(malformed), &registry);
  auto runtime_in_author_file = author_entries();
  runtime_in_author_file[1].bytes = manifest;
  reject(std::move(runtime_in_author_file), &registry);
  auto full = author_entries();
  for (std::size_t index = 2; index < 4096; ++index) {
    Entry asset;
    asset.path = "asset-" + std::to_string(index);
    full.push_back(std::move(asset));
  }
  reject(std::move(full), &registry);
}

void expect_rejected(std::vector<Entry> entries, std::string_view label) {
  TempDirectory root;
  auto input = archive(entries);
  const bool rejected = throws_exception<std::exception>([&] {
    auto ignored = discovery::publish_revision_archive(
        input.get(), root.fd(), static_cast<std::uint32_t>(::geteuid()));
  });
  check(rejected, label);
  OMARCHY_CHECK(std::filesystem::is_empty(root.path()));
}

void expect_descriptor_rejected(int descriptor, std::string_view label) {
  TempDirectory root;
  const bool rejected = throws_exception<std::exception>([&] {
    auto ignored = discovery::publish_revision_archive(
        descriptor, root.fd(), static_cast<std::uint32_t>(::geteuid()));
  });
  check(rejected, label);
}

void rewrite_header_checksum(int fd, std::array<char, 512> &header) {
  std::memset(header.data() + 148, ' ', 8);
  unsigned checksum = 0;
  for (const unsigned char byte : header)
    checksum += byte;
  std::snprintf(header.data() + 148, 8, "%06o", checksum);
  header[154] = '\0';
  header[155] = ' ';
  OMARCHY_CHECK(::pwrite(fd, header.data(), header.size(), 0) ==
            static_cast<ssize_t>(header.size()));
  OMARCHY_CHECK(::lseek(fd, 0, SEEK_SET) == 0);
}

void positive_and_same_digest_race() {
  TempDirectory root;
  auto first_archive = archive(valid_entries());
  auto first = discovery::publish_revision_archive(
      first_archive.get(), root.fd(), static_cast<std::uint32_t>(::geteuid()));
  OMARCHY_CHECK(first.verified().manifest.id == "org.example.ingress");
  OMARCHY_CHECK(first.verified().identity.tree_sha256.size() == 64);

  struct stat root_metadata{};
  OMARCHY_CHECK(::fstat(first.descriptor(), &root_metadata) == 0 &&
            (root_metadata.st_mode & 07777) == 0555);
  const int file = ::openat(first.descriptor(), "manifest.json",
                            O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  struct stat file_metadata{};
  OMARCHY_CHECK(file >= 0 && ::fstat(file, &file_metadata) == 0 &&
            (file_metadata.st_mode & 07777) == 0444 &&
            file_metadata.st_nlink == 1);
  ::close(file);

  auto second_archive = archive(valid_entries());
  auto second = discovery::publish_revision_archive(
      second_archive.get(), root.fd(), static_cast<std::uint32_t>(::geteuid()));
  OMARCHY_CHECK(second.verified().identity == first.verified().identity);

  TempDirectory concurrent_root;
  auto left_archive = archive(valid_entries());
  auto right_archive = archive(valid_entries());
  std::barrier start(3);
  std::atomic<int> completed{0};
  std::atomic<int> failures{0};
  auto run = [&](int fd) {
    start.arrive_and_wait();
    try {
      auto result = discovery::publish_revision_archive(
          fd, concurrent_root.fd(), static_cast<std::uint32_t>(::geteuid()));
      (void)result;
      ++completed;
    } catch (...) {
      ++failures;
    }
  };
  std::thread left(run, left_archive.get());
  std::thread right(run, right_archive.get());
  start.arrive_and_wait();
  left.join();
  right.join();
  OMARCHY_CHECK(failures == 0 && completed == 2 &&
            std::ranges::distance(std::filesystem::directory_iterator(
                                      concurrent_root.path()),
                                  std::filesystem::directory_iterator{}) == 1);
}

void unsafe_archives() {
  auto replace_qml = [](std::string path, char type = '0', unsigned mode = 0644,
                        std::string link = {}) {
    auto entries = valid_entries();
    entries.back().path = std::move(path);
    entries.back().type = type;
    entries.back().mode = mode;
    entries.back().link = std::move(link);
    return entries;
  };
  expect_rejected(replace_qml("/ui/Main.qml"), "absolute path accepted");
  expect_rejected(replace_qml("ui/../Main.qml"), "parent traversal accepted");
  expect_rejected(replace_qml("ui//Main.qml"), "empty path component accepted");
  expect_rejected(replace_qml("ui/./Main.qml"), "dot path component accepted");
  expect_rejected(replace_qml("ui/Main.qml", '2', 0777, "elsewhere"),
                  "symlink accepted");
  expect_rejected(replace_qml("ui/Main.qml", '1', 0644, "manifest.json"),
                  "hardlink accepted");
  expect_rejected(replace_qml("ui/Main.qml", '6'), "special file accepted");
  expect_rejected(replace_qml("ui/Main.qml", '0', 04755), "set-id mode accepted");
  expect_rejected(replace_qml("ui/Main.qml", '0', 0664),
                  "group-writable mode accepted");

  auto duplicate = valid_entries();
  duplicate.push_back(duplicate.back());
  expect_rejected(std::move(duplicate), "duplicate path accepted");
  auto prefix_collision = valid_entries();
  Entry blocked;
  blocked.path = "blocked";
  blocked.bytes = "file";
  prefix_collision.insert(prefix_collision.begin(), blocked);
  Entry child;
  child.path = "blocked/child";
  child.bytes = "file";
  prefix_collision.push_back(child);
  expect_rejected(std::move(prefix_collision), "file/directory alias accepted");

  std::string deep;
  for (int index = 0; index < 65; ++index)
    deep += (index == 0 ? "a" : "/a");
  expect_rejected(replace_qml(deep), "depth bound was not enforced");
  auto oversized = valid_entries();
  oversized.back().declared_size = 64ULL * 1024ULL * 1024ULL + 1;
  expect_rejected(std::move(oversized), "byte bound was not enforced");

  TempDirectory directory_archive;
  expect_descriptor_rejected(directory_archive.fd(),
                             "directory archive descriptor accepted");
  int pipe_fds[2]{};
  OMARCHY_CHECK(::pipe2(pipe_fds, O_CLOEXEC) == 0);
  expect_descriptor_rejected(pipe_fds[0], "nonseekable archive accepted");
  ::close(pipe_fds[0]);
  ::close(pipe_fds[1]);

  {
    auto malformed = archive(valid_entries());
    std::array<char, 512> header{};
    OMARCHY_CHECK(::pread(malformed.get(), header.data(), header.size(), 0) == 512);
    header[0] ^= 1;
    OMARCHY_CHECK(::pwrite(malformed.get(), header.data(), header.size(), 0) == 512);
    expect_descriptor_rejected(malformed.get(), "bad archive checksum accepted");
  }
  {
    auto malformed = archive(valid_entries());
    std::array<char, 512> header{};
    OMARCHY_CHECK(::pread(malformed.get(), header.data(), header.size(), 0) == 512);
    header[124] = '9';
    rewrite_header_checksum(malformed.get(), header);
    expect_descriptor_rejected(malformed.get(), "bad numeric field accepted");
  }
  {
    auto malformed = archive(valid_entries());
    std::array<char, 512> header{};
    OMARCHY_CHECK(::pread(malformed.get(), header.data(), header.size(), 0) == 512);
    header[263] = '9';
    rewrite_header_checksum(malformed.get(), header);
    expect_descriptor_rejected(malformed.get(), "bad ustar version accepted");
  }
  {
    auto malformed = archive(valid_entries());
    OMARCHY_CHECK(::lseek(malformed.get(), 0, SEEK_END) >= 0 &&
              ::write(malformed.get(), "x", 1) == 1 &&
              ::lseek(malformed.get(), 0, SEEK_SET) == 0);
    expect_descriptor_rejected(malformed.get(),
                               "nonzero trailing archive data accepted");
  }
  {
    TempDirectory unsafe_root;
    OMARCHY_CHECK(::fchmod(unsafe_root.fd(), 0755) == 0);
    auto input = archive(valid_entries());
    bool rejected = false;
    try {
      auto ignored = discovery::publish_revision_archive(
          input.get(), unsafe_root.fd(),
          static_cast<std::uint32_t>(::geteuid()));
    } catch (...) {
      rejected = true;
    }
    OMARCHY_CHECK(rejected);
  }
}

void existing_digest_cannot_be_substituted() {
  TempDirectory root;
  auto input = archive(valid_entries());
  auto first = discovery::publish_revision_archive(
      input.get(), root.fd(), static_cast<std::uint32_t>(::geteuid()));
  OMARCHY_CHECK(::fchmod(first.descriptor(), 0755) == 0);
  OMARCHY_CHECK(::fchmodat(first.descriptor(), "manifest.json", 0644, 0) == 0);
  OMARCHY_CHECK(::fchmod(first.descriptor(), 0555) == 0);
  auto retry = archive(valid_entries());
  OMARCHY_CHECK(throws_exception<std::exception>([&] {
    auto ignored = discovery::publish_revision_archive(
        retry.get(), root.fd(), static_cast<std::uint32_t>(::geteuid()));
  }));

  TempDirectory hardlink_root;
  auto hardlink_input = archive(valid_entries());
  auto hardlinked = discovery::publish_revision_archive(
      hardlink_input.get(), hardlink_root.fd(),
      static_cast<std::uint32_t>(::geteuid()));
  OMARCHY_CHECK(::linkat(hardlinked.descriptor(), "manifest.json", hardlink_root.fd(),
                 "outside-hardlink", 0) == 0);
  auto hardlink_retry = archive(valid_entries());
  OMARCHY_CHECK(throws_exception<std::exception>([&] {
    auto ignored = discovery::publish_revision_archive(
        hardlink_retry.get(), hardlink_root.fd(),
        static_cast<std::uint32_t>(::geteuid()));
  }));
}

void crash_points_never_publish_torn_tree(bool authoring = false) {
  const auto registry = omarchy::plugin_runtime::test_support::packaged_registry();
  const auto entries = authoring ? author_entries() : valid_entries();
  for (const auto point : {discovery::RevisionIngressCrashPoint::extracted,
                           discovery::RevisionIngressCrashPoint::verified,
                           discovery::RevisionIngressCrashPoint::durable,
                           discovery::RevisionIngressCrashPoint::renamed,
                           discovery::RevisionIngressCrashPoint::published}) {
    TempDirectory root;
    auto input = archive(entries);
    expect_child_exit(80 + static_cast<int>(point), [&] {
      discovery::set_revision_ingress_crash_point_for_testing(point);
      try {
        auto ignored = discovery::publish_revision_archive(
            input.get(), root.fd(), static_cast<std::uint32_t>(::geteuid()), &registry);
      } catch (...) {
        ::_exit(120);
      }
      ::_exit(121);
    });
    for (const auto &entry : std::filesystem::directory_iterator(root.path())) {
      const auto name = entry.path().filename().string();
      if (name.starts_with(".incoming-"))
        continue;
      OMARCHY_CHECK(name.size() == 64);
      UniqueFd revision(::openat(root.fd(), name.c_str(),
                           O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
      OMARCHY_CHECK(revision.get() >= 0);
      const auto verified = discovery::discover_open_revision(revision.get());
      OMARCHY_CHECK(verified.identity.tree_sha256 == name);
      if (authoring)
        OMARCHY_CHECK(published_text(revision.get(), "manifest.author.json") == entries[1].bytes &&
                    published_text(revision.get(), "manifest.json") == verified.manifest.canonical_json);
    }
    auto recovery_archive = archive(entries);
    auto recovered = discovery::publish_revision_archive(
        recovery_archive.get(), root.fd(),
        static_cast<std::uint32_t>(::geteuid()), &registry);
    (void)recovered;
    for (const auto &entry : std::filesystem::directory_iterator(root.path()))
      OMARCHY_CHECK(!entry.path().filename().string().starts_with(".incoming-"));
  }
}

} // namespace

int main() {
  return omarchy::plugin_runtime::test_support::test_main([&] {
    positive_and_same_digest_race();
    authoring_publication();
    authoring_rejections();
    unsafe_archives();
    existing_digest_cannot_be_substituted();
    crash_points_never_publish_torn_tree();
    crash_points_never_publish_torn_tree(true);
    std::cout << "revision ingress tests passed\n";
    return 0;
  }, "revision ingress test failed: ");
}
