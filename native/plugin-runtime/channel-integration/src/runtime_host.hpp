#pragma once

#include "plugin_permission_authority.hpp"
#include "surface_session_port.hpp"
#include "gesture_intent.hpp"

class QObject;

namespace omarchy::plugin_runtime::channel {

// Trusted host projection port, never exposed to plugin QML. Calls run on the
// event owner's thread. Publication may synchronously reenter host code, so
// the runtime rechecks its exact epoch and binding after each publication.
// Resource construction/close must not reenter lifecycle mutation; they may
// only construct/detach UI resources against the supplied session port.
class RuntimeHost {
public:
  virtual ~RuntimeHost() = default;
  virtual QObject &eventOwner() noexcept = 0;
  virtual void catalogAvailable() = 0;
  virtual void invalidatePlugin(std::string_view plugin) = 0;
  virtual std::optional<std::string> currentSettings(std::string_view plugin) = 0;
  virtual std::optional<std::string> currentPresentation() = 0;
  virtual bool persistSettings(std::string_view plugin, std::string_view entry) = 0;
  virtual bool publishIntent(host_session::AdmittedSurfaceIntent intent) = 0;
  virtual std::unique_ptr<RuntimePresentation> createPresentation(
      const permissions::ActivationBinding &binding, std::uint64_t epoch,
      SurfaceSessionPort &session) = 0;
  virtual bool publishSurfaces(const permissions::ActivationBinding &binding,
      const std::vector<std::string> &names, std::string_view canonical_surfaces,
      std::uint64_t epoch) = 0;
  virtual void withdrawSurfaces(const permissions::ActivationBinding &binding) noexcept = 0;
  virtual void completeInstall(std::uint64_t serial, std::string plugin,
      std::string revision, std::string error) = 0;
  virtual void failPermissionControl(std::uint64_t serial, std::string error) = 0;
  virtual void completePermissionRead(std::uint64_t serial, std::string plugin,
      std::uint64_t epoch, std::shared_ptr<PluginPermissionAuthority> authority,
      std::optional<host_session::AuthorityView> view,
      std::shared_ptr<const host_session::ConsentReview> review) = 0;
  virtual void completePermissionMutation(std::uint64_t serial, bool applied,
      std::string error) = 0;
};

} // namespace omarchy::plugin_runtime::channel
