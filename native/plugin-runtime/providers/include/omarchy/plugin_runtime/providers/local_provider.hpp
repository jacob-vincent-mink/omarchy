#pragma once

#include "dynamic_activation.hpp"
#include "omarchy/plugin_runtime/providers/private_storage_backend.hpp"

#include <QJsonObject>
#include <memory>

namespace omarchy::plugin_runtime::providers {

using NotificationSend = bool (*)(std::string_view plugin, std::string_view category,
                                  std::string_view title, std::string_view body,
                                  void *context) noexcept;

// The same reviewed route contract as a process provider. The only local
// effects are descriptor-rooted private storage and host-labelled notifications.
class LocalProvider final {
public:
  LocalProvider(const plugins::definitions::DynamicRevisionGrant &grant,
                plugins::definitions::EnforcementFamily family,
                int state_directory, NotificationSend send, void *context,
                std::uint64_t maximum_total = 64 * 1024 * 1024,
                std::uint64_t maximum_item = 1024 * 1024);
  bool dispatch(const plugins::definitions::AuthorizedDynamicRequest &request,
                std::span<std::byte> response, std::size_t &written) noexcept;

private:
  plugins::definitions::DynamicRevisionGrant grant_;
  QJsonObject scope_;
  std::unique_ptr<PrivateStorageBackend> storage_;
  NotificationSend send_ = nullptr;
  void *context_ = nullptr;
};

} // namespace omarchy::plugin_runtime::providers
