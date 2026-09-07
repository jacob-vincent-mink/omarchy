#include "omarchy/plugin_runtime/manifest_identifier.hpp"
#include "omarchy/plugin_runtime/file_metadata.hpp"
#include "omarchy/plugin_runtime/unique_directory.hpp"
#include "activation_catalog.hpp"
#include "checked_operation.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <ranges>
#include <utility>

namespace omarchy::plugin_runtime::channel {
ActivationCatalogEntry::ActivationCatalogEntry(
    std::string record_name,
    host_session::InspectedActivationRecord inspected, Epoch epoch) noexcept
    : record_name_(std::move(record_name)), inspected_(std::move(inspected)),
      epoch_(epoch) {}

bool ActivationCatalogEntry::same_epoch(
    const ActivationCatalogEntry &other) const noexcept {
  const auto &left = inspected_.record();
  const auto &right = other.inspected_.record();
  return record_name_ == other.record_name_ &&
         same_file_metadata(epoch_, other.epoch_) &&
         left.plugin_id == right.plugin_id &&
         left.revision_directory == right.revision_directory &&
         left.revision_sha256 == right.revision_sha256 &&
         left.state_directory == right.state_directory;
}

ActivationCatalog::ActivationCatalog(
    host_session::UniqueFd activation_root,
    ActivationCatalogEntry::Epoch root_epoch,
    std::vector<ActivationCatalogEntry> entries)
    : activation_root_(std::move(activation_root)),
      root_epoch_(root_epoch),
      entries_(std::move(entries)) {}

bool ActivationCatalog::capture_epoch(
    int descriptor, ActivationCatalogEntry::Epoch &epoch) noexcept {
  struct stat metadata{};
  if (descriptor < 0 || ::fstat(descriptor, &metadata) < 0 ||
      metadata.st_size < 0)
    return false;
  epoch = metadata;
  return true;
}

bool ActivationCatalog::unchanged() const noexcept {
  ActivationCatalogEntry::Epoch current{};
  return capture_epoch(activation_root_.get(), current) &&
         same_file_metadata(current, root_epoch_) &&
         std::ranges::all_of(entries_, [](const auto &entry) {
           return entry.currently_unchanged();
         });
}

bool ActivationCatalog::same_epoch(
    const ActivationCatalog &other) const noexcept {
  if (!same_file_metadata(root_epoch_, other.root_epoch_) ||
      entries_.size() != other.entries_.size())
    return false;
  for (std::size_t index = 0; index < entries_.size(); ++index) {
    if (!entries_[index].same_epoch(other.entries_[index]))
      return false;
  }
  return true;
}

std::unique_ptr<ActivationCatalog>
ActivationCatalog::load(int activation_root_fd, std::uint32_t trusted_uid,
                              ActivationCatalogError &error) noexcept {
  return detail::checked_operation(error, nullptr, [&]() -> std::unique_ptr<ActivationCatalog> {
    struct stat root_before{};
    if (activation_root_fd < 0 ||
        ::fstat(activation_root_fd, &root_before) < 0 ||
        !exact_private_directory(root_before, trusted_uid)) {
      error = ActivationCatalogError::root_untrusted;
      return {};
    }

    auto root = reopen_directory(activation_root_fd);
    struct stat pinned_root{};
    if (!root || ::fstat(root.get(), &pinned_root) < 0 ||
        pinned_root.st_dev != root_before.st_dev ||
        pinned_root.st_ino != root_before.st_ino ||
        !exact_private_directory(pinned_root, trusted_uid) ||
        ::flock(root.get(), LOCK_SH) < 0) {
      error = ActivationCatalogError::root_untrusted;
      return {};
    }
    struct stat locked_root_before{};
    if (::fstat(root.get(), &locked_root_before) < 0 ||
        !exact_private_directory(locked_root_before, trusted_uid)) {
      error = ActivationCatalogError::root_untrusted;
      return {};
    }

    const int scan_fd = ::openat(
        root.get(), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (scan_fd < 0) {
      error = ActivationCatalogError::enumeration_failed;
      return {};
    }
    auto directory = take_directory(host_session::UniqueFd(scan_fd));
    if (!directory) {
      error = ActivationCatalogError::enumeration_failed;
      return {};
    }

    std::vector<ActivationCatalogEntry> entries;
    for (;;) {
      errno = 0;
      const auto *entry = ::readdir(directory.get());
      if (entry == nullptr) {
        if (errno != 0)
          error = ActivationCatalogError::enumeration_failed;
        break;
      }
      const std::string_view name(entry->d_name);
      if (name == "." || name == "..")
        continue;
      if (entries.size() == kMaximumActivationCatalogEntries) {
        error = ActivationCatalogError::bound_exceeded;
        break;
      }
      if (!canonical_manifest_identifier(name)) {
        error = ActivationCatalogError::unexpected_entry;
        break;
      }
      struct stat entry_metadata{};
      if (::fstatat(root.get(), entry->d_name, &entry_metadata,
                    AT_SYMLINK_NOFOLLOW) < 0 ||
          !S_ISREG(entry_metadata.st_mode)) {
        error = ActivationCatalogError::unexpected_entry;
        break;
      }
      auto inspected = host_session::inspect_activation_record(root.get(), name,
                                                               trusted_uid);
      if (!inspected) {
        error = ActivationCatalogError::invalid_record;
        break;
      }
      if (std::ranges::any_of(entries, [&](const auto &candidate) {
            return candidate.plugin_id() ==
                   inspected->record().plugin_id;
          })) {
        error = ActivationCatalogError::duplicate_plugin;
        break;
      }
      if (inspected->record().plugin_id != name) {
        error = ActivationCatalogError::invalid_record;
        break;
      }
      ActivationCatalogEntry::Epoch record_epoch{};
      if (!capture_epoch(inspected->descriptor(), record_epoch)) {
        error = ActivationCatalogError::mutated;
        break;
      }
      entries.push_back(ActivationCatalogEntry(
          std::string(name), std::move(*inspected), record_epoch));
    }

    struct stat root_after{};
    if (error == ActivationCatalogError::none &&
        (::fstat(root.get(), &root_after) < 0 ||
         !same_file_metadata(locked_root_before, root_after) ||
         std::ranges::any_of(entries, [](const auto &candidate) {
           return !candidate.currently_unchanged();
         })))
      error = ActivationCatalogError::mutated;

    if (::closedir(directory.release()) < 0 &&
        error == ActivationCatalogError::none)
      error = ActivationCatalogError::enumeration_failed;
    if (::flock(root.get(), LOCK_UN) < 0 &&
        error == ActivationCatalogError::none)
      error = ActivationCatalogError::enumeration_failed;
    if (error != ActivationCatalogError::none)
      return {};
    std::ranges::sort(entries, [](const auto &left, const auto &right) {
      return left.plugin_id() < right.plugin_id();
    });
    return std::unique_ptr<ActivationCatalog>(
        new ActivationCatalog(std::move(root), locked_root_before,
                                    std::move(entries)));
  });
}

} // namespace omarchy::plugin_runtime::channel
