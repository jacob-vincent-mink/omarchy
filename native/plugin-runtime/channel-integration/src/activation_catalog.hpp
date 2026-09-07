#pragma once

#include "activation_snapshot.hpp"

#include <sys/stat.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace omarchy::plugin_runtime::channel {

inline constexpr std::size_t kMaximumActivationCatalogEntries = 1024;

enum class ActivationCatalogError : std::uint8_t {
  none,
  root_untrusted,
  enumeration_failed,
  bound_exceeded,
  unexpected_entry,
  invalid_record,
  duplicate_plugin,
  mutated,
  resource_exhausted,
  internal_failure,
};

// Candidate labels are inventory only. Opaque epoch predicates let the manager
// preserve a last-good scan and identify additions, removals or changes without
// exposing parsed activation authority or record descriptors.
class ActivationCatalogEntry final {
public:
  [[nodiscard]] std::string_view plugin_id() const noexcept {
    return record_name_;
  }
  [[nodiscard]] bool
  same_epoch(const ActivationCatalogEntry &other) const noexcept;

private:
  using Epoch = struct stat;

  ActivationCatalogEntry(
      std::string record_name,
      host_session::InspectedActivationRecord inspected, Epoch epoch) noexcept;
  [[nodiscard]] bool currently_unchanged() const noexcept {
    return inspected_.unchanged();
  }

  std::string record_name_;
  host_session::InspectedActivationRecord inspected_;
  Epoch epoch_;

  friend class ActivationCatalog;
};

class ActivationCatalog final {
public:
  [[nodiscard]] static std::unique_ptr<ActivationCatalog>
  load(int activation_root_fd, std::uint32_t trusted_uid,
       ActivationCatalogError &error) noexcept;

  [[nodiscard]] std::span<const ActivationCatalogEntry>
  entries() const noexcept {
    return entries_;
  }
  [[nodiscard]] bool unchanged() const noexcept;
  [[nodiscard]] bool
  same_epoch(const ActivationCatalog &other) const noexcept;

private:
  ActivationCatalog(host_session::UniqueFd activation_root,
                          ActivationCatalogEntry::Epoch root_epoch,
                          std::vector<ActivationCatalogEntry> entries);
  [[nodiscard]] static bool
  capture_epoch(int descriptor,
                ActivationCatalogEntry::Epoch &epoch) noexcept;

  host_session::UniqueFd activation_root_;
  ActivationCatalogEntry::Epoch root_epoch_;
  std::vector<ActivationCatalogEntry> entries_;
};

} // namespace omarchy::plugin_runtime::channel
