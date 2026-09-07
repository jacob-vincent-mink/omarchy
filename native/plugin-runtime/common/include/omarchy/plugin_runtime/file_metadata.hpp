#pragma once

#include <sys/stat.h>
#include <cstdint>

namespace omarchy::plugin_runtime {

inline bool secure_directory(const struct stat &metadata, std::uint32_t uid) noexcept {
  return S_ISDIR(metadata.st_mode) && metadata.st_uid == uid &&
         (metadata.st_mode & 0022) == 0;
}

inline bool exact_private_directory(const struct stat &metadata, std::uint32_t uid) noexcept {
  return S_ISDIR(metadata.st_mode) && metadata.st_uid == uid &&
         (metadata.st_mode & 07777) == 0700;
}

// Compare stable identity and mutation metadata, not access time, allocation
// hints, or struct padding. Trust policy and successful fstat remain caller checks.
[[nodiscard]] inline bool same_file_metadata(const struct stat &before,
                                             const struct stat &after) noexcept {
  return before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
         before.st_mode == after.st_mode && before.st_uid == after.st_uid &&
         before.st_gid == after.st_gid && before.st_nlink == after.st_nlink &&
         before.st_size == after.st_size &&
         before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
         before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
         before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
         before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
}

} // namespace omarchy::plugin_runtime
