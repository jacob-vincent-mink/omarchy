#pragma once

#include "PluginManager.h"
#include "SurfaceEndpointOwner.h"
#include "runtime_session_owner.hpp"
#include "surface_host.hpp"

namespace omarchy::plugin_runtime::bridge::detail {

// Qt projection adapter. Product lifecycle, authority and asynchronous jobs
// belong to RuntimeSessionOwner; this adapter owns only host presentation glue.
class Q_DECL_HIDDEN PluginRuntimeController final : private channel::RuntimeHost {
public:
  static std::unique_ptr<PluginRuntimeController> open(PluginManager &manager) noexcept;
  PluginRuntimeController(PluginManager &manager,
      std::unique_ptr<channel::RuntimeBootstrap> bootstrap);
  ~PluginRuntimeController() noexcept;

  bool beginPermissionRead(std::uint64_t serial, std::string plugin, bool review,
      std::optional<plugins::permissions::Digest> expected_revision) noexcept {
    return owner_->beginPermissionRead(serial, std::move(plugin), review,
                                      std::move(expected_revision));
  }
  bool beginInstall(std::uint64_t serial, int archive_fd) noexcept {
    return owner_->beginInstall(serial, archive_fd);
  }
  bool beginControlledPermissionApply(std::uint64_t serial, std::string_view plugin,
      std::uint64_t epoch, const std::shared_ptr<channel::PluginPermissionAuthority> &authority,
      std::shared_ptr<const host_session::ConsentReview> review,
      const host_session::ConsentConfirmation &confirmation,
      std::span<const host_session::DynamicConsentDecision> decisions) noexcept {
    return owner_->beginControlledPermissionApply(serial, plugin, epoch, authority,
                                                  std::move(review), confirmation, decisions);
  }
  bool beginControlledPermissionRevoke(std::uint64_t serial, std::string_view plugin,
      std::uint64_t epoch, const std::shared_ptr<channel::PluginPermissionAuthority> &authority,
      const plugins::definitions::CapabilityReference &definition,
      std::uint64_t expected_sequence) noexcept {
    return owner_->beginControlledPermissionRevoke(serial, plugin, epoch, authority,
                                                   definition, expected_sequence);
  }
  bool attach(const QString &surface_key, QObject *surface) noexcept;

private:
  class MonotonicClock final : public surface_host::MonotonicClock {
  public:
    std::uint64_t now_nanoseconds() const override;
  };
  QObject &eventOwner() noexcept override;
  void catalogAvailable() override;
  void invalidatePlugin(std::string_view plugin) override;
  std::optional<std::string> currentSettings(std::string_view plugin) override;
  std::optional<std::string> currentPresentation() override;
  bool persistSettings(std::string_view plugin, std::string_view entry) override;
  bool publishIntent(host_session::AdmittedSurfaceIntent intent) override;
  std::unique_ptr<channel::RuntimePresentation> createPresentation(
      const plugins::permissions::ActivationBinding &binding, std::uint64_t epoch,
      channel::SurfaceSessionPort &session) override;
  bool publishSurfaces(const plugins::permissions::ActivationBinding &binding,
      const std::vector<std::string> &names, std::string_view canonical_surfaces,
      std::uint64_t epoch) override;
  void withdrawSurfaces(const plugins::permissions::ActivationBinding &binding) noexcept override;
  void completeInstall(std::uint64_t serial, std::string plugin,
      std::string revision, std::string error) override;
  void failPermissionControl(std::uint64_t serial, std::string error) override;
  void completePermissionRead(std::uint64_t serial, std::string plugin,
      std::uint64_t epoch, std::shared_ptr<channel::PluginPermissionAuthority> authority,
      std::optional<host_session::AuthorityView> view,
      std::shared_ptr<const host_session::ConsentReview> review) override;
  void completePermissionMutation(std::uint64_t serial, bool applied,
      std::string error) override;

  PluginManager &manager_;
  MonotonicClock clock_;
  std::unique_ptr<channel::RuntimeSessionOwner> owner_;

#ifdef OMARCHY_PLUGIN_MANAGER_TESTING
  struct ManualTestTag final {};
  PluginRuntimeController(PluginManager &manager,
      std::unique_ptr<channel::RuntimeBootstrap> bootstrap, ManualTestTag);
  friend class ::omarchy::plugin_runtime::bridge::PluginManagerTestAccess;
#endif
  friend class ::omarchy::plugin_runtime::bridge::PluginManager;
};

} // namespace omarchy::plugin_runtime::bridge::detail
