#include "omarchy/plugin_runtime/providers/private_storage_backend.hpp"

#include "omarchy/plugin_runtime/unique_directory.hpp"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <limits>
#include <string>

namespace omarchy::plugin_runtime::providers {

bool valid_storage_key(std::string_view value) noexcept {
  return !value.empty() && value.size() <= kMaximumStorageKeyBytes &&
         !value.starts_with(".omarchy-tmp-") &&
         value != "." && value != ".." &&
         std::ranges::all_of(value, [](unsigned char byte) {
           return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                  (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' || byte == '-';
         });
}

PrivateStorageBackend::PrivateStorageBackend(
    int directory_fd, std::uint64_t maximum_total_bytes,
    std::uint64_t maximum_item_bytes) noexcept
    : maximum_total_bytes_(maximum_total_bytes),
      maximum_item_bytes_(maximum_item_bytes) {
  if (directory_fd < 0 || maximum_item_bytes == 0 ||
      maximum_item_bytes > maximum_total_bytes)
    return;
  directory_fd_ = fcntl(directory_fd, F_DUPFD_CLOEXEC, 3);
}

PrivateStorageBackend::~PrivateStorageBackend() {
  if (directory_fd_ >= 0)
    close(directory_fd_);
}

bool PrivateStorageBackend::read(std::string_view key,
                                 std::span<std::byte> output,
                                 std::size_t &written, bool &found) noexcept {
  written = 0;
  found = false;
  if (!valid_storage_key(key))
    return false;
  const std::string name(key);
  const int fd = openat(directory_fd_, name.c_str(),
                        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0)
    return errno == ENOENT;
  struct stat info {};
  if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1 ||
      info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) >
                              maximum_item_bytes_ ||
      static_cast<std::uint64_t>(info.st_size) > output.size()) {
    close(fd);
    return false;
  }
  const auto size = static_cast<std::size_t>(info.st_size);
  while (written < size) {
    const auto count = ::read(fd, output.data() + written, size - written);
    if (count <= 0) {
      close(fd);
      return false;
    }
    written += static_cast<std::size_t>(count);
  }
  const bool exact = close(fd) == 0;
  found = exact;
  return exact;
}

bool PrivateStorageBackend::within_total_limit(std::string_view replacing,
                                                std::size_t new_size) const noexcept {
  // openat(".") creates an independent directory stream offset. dup() would
  // share the offset and a second quota scan could incorrectly start at EOF.
  const int scan_fd =
      openat(directory_fd_, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (scan_fd < 0)
    return false;
  auto directory = take_directory(UniqueFd(scan_fd));
  if (!directory)
    return false;
  std::uint64_t total = new_size;
  bool ok = true;
  while (const auto *entry = readdir(directory.get())) {
    const std::string_view name(entry->d_name);
    if (name == "." || name == ".." || name == replacing ||
        name.starts_with(".omarchy-tmp-"))
      continue;
    struct stat info {};
    if (fstatat(directory_fd_, entry->d_name, &info,
                AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(info.st_mode) || info.st_nlink != 1 || info.st_size < 0) {
      ok = false;
      break;
    }
    const auto size = static_cast<std::uint64_t>(info.st_size);
    if (size > maximum_total_bytes_ || total > maximum_total_bytes_ - size) {
      ok = false;
      break;
    }
    total += size;
  }
  directory.reset();
  return ok && total <= maximum_total_bytes_;
}

bool PrivateStorageBackend::write(std::string_view key,
                                  std::span<const std::byte> value) noexcept {
  if (!valid_storage_key(key) || value.size() > maximum_item_bytes_ ||
      !within_total_limit(key, value.size()))
    return false;
  const std::string destination(key);
  std::array<char, 64> temporary{};
  const int length = std::snprintf(temporary.data(), temporary.size(),
                                   ".omarchy-tmp-%ld-%llu",
                                   static_cast<long>(getpid()),
                                   static_cast<unsigned long long>(
                                       ++temporary_sequence_));
  if (length <= 0 || static_cast<std::size_t>(length) >= temporary.size())
    return false;
  const int fd = openat(directory_fd_, temporary.data(),
                        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                        0600);
  if (fd < 0)
    return false;
  std::size_t offset = 0;
  bool ok = true;
  while (offset < value.size()) {
    const auto count = ::write(fd, value.data() + offset, value.size() - offset);
    if (count <= 0) {
      ok = false;
      break;
    }
    offset += static_cast<std::size_t>(count);
  }
  if (ok) ok = fsync(fd) == 0;
  if (close(fd) != 0) ok = false;
  if (ok)
    ok = renameat(directory_fd_, temporary.data(), directory_fd_,
                  destination.c_str()) == 0 &&
         fsync(directory_fd_) == 0;
  if (!ok)
    unlinkat(directory_fd_, temporary.data(), 0);
  return ok;
}

bool PrivateStorageBackend::remove(std::string_view key) noexcept {
  if (!valid_storage_key(key))
    return false;
  const std::string name(key);
  struct stat info {};
  if (fstatat(directory_fd_, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) !=
          0 ||
      !S_ISREG(info.st_mode) || info.st_nlink != 1)
    return false;
  return unlinkat(directory_fd_, name.c_str(), 0) == 0 &&
         fsync(directory_fd_) == 0;
}

} // namespace omarchy::plugin_runtime::providers
