#include "PluginRuntimeController.h"
#include "remote_surface.hpp"

#include <QThread>
#include <chrono>
#include <stdexcept>

namespace omarchy::plugin_runtime::bridge::detail {

std::uint64_t PluginRuntimeController::MonotonicClock::now_nanoseconds() const {
  static_assert(std::chrono::steady_clock::is_steady);
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  return elapsed < 0 ? 0 : static_cast<std::uint64_t>(elapsed);
}

std::unique_ptr<PluginRuntimeController>
PluginRuntimeController::open(PluginManager &manager) noexcept {
  try {
    return std::unique_ptr<PluginRuntimeController>(new PluginRuntimeController(manager, {}));
  } catch (...) {
    return {};
  }
}

PluginRuntimeController::PluginRuntimeController(
    PluginManager &manager, std::unique_ptr<channel::RuntimeBootstrap> bootstrap)
    : manager_(manager) {
  if (bootstrap)
    owner_ = std::make_unique<channel::RuntimeSessionOwner>(
        static_cast<channel::RuntimeHost &>(*this), std::move(bootstrap));
  else
    owner_ = channel::RuntimeSessionOwner::open(*this);
  if (!owner_)
    throw std::runtime_error("runtime unavailable");
}

#ifdef OMARCHY_PLUGIN_MANAGER_TESTING
PluginRuntimeController::PluginRuntimeController(
    PluginManager &manager, std::unique_ptr<channel::RuntimeBootstrap> bootstrap, ManualTestTag)
    : manager_(manager),
      owner_(new channel::RuntimeSessionOwner(*this, std::move(bootstrap),
                 channel::RuntimeSessionOwner::ManualTestTag{})) {}
#endif

PluginRuntimeController::~PluginRuntimeController() noexcept = default;

QObject &PluginRuntimeController::eventOwner() noexcept { return manager_; }
void PluginRuntimeController::catalogAvailable() {
  if (!manager_.available_) {
    manager_.available_ = true;
    emit manager_.availableChanged();
  }
}
void PluginRuntimeController::invalidatePlugin(std::string_view plugin) {
  manager_.permissions_.invalidatePlugin(plugin);
}
std::optional<std::string> PluginRuntimeController::currentSettings(std::string_view plugin) {
  return manager_.currentSettings(plugin);
}
std::optional<std::string> PluginRuntimeController::currentPresentation() {
  return manager_.currentPresentation();
}
bool PluginRuntimeController::persistSettings(std::string_view plugin, std::string_view entry) {
  return manager_.persistSettings(plugin, entry);
}
bool PluginRuntimeController::publishIntent(host_session::AdmittedSurfaceIntent intent) {
  return manager_.publishIntent(std::move(intent));
}
std::unique_ptr<channel::RuntimePresentation> PluginRuntimeController::createPresentation(
    const plugins::permissions::ActivationBinding &binding, std::uint64_t epoch,
    channel::SurfaceSessionPort &session) {
  return std::unique_ptr<SurfaceEndpointOwner>(
      new SurfaceEndpointOwner(clock_, binding, epoch, session));
}
void PluginRuntimeController::withdrawSurfaces(
    const plugins::permissions::ActivationBinding &binding) noexcept {
  try {
    static_cast<void>(manager_.surfaces_.withdrawSurfaces(binding));
  } catch (...) {
  }
}
void PluginRuntimeController::completeInstall(std::uint64_t serial, std::string plugin,
    std::string revision, std::string error) {
  manager_.completeInstall(serial, std::move(plugin), std::move(revision), std::move(error));
}
void PluginRuntimeController::failPermissionControl(std::uint64_t serial, std::string error) {
  manager_.failPermissionControl(serial, std::move(error));
}
void PluginRuntimeController::completePermissionRead(std::uint64_t serial, std::string plugin,
    std::uint64_t epoch, std::shared_ptr<channel::PluginPermissionAuthority> authority,
    std::optional<host_session::AuthorityView> view,
    std::shared_ptr<const host_session::ConsentReview> review) {
  manager_.completePermissionRead(serial, std::move(plugin), epoch, std::move(authority),
                                  std::move(view), std::move(review));
}
void PluginRuntimeController::completePermissionMutation(
    std::uint64_t serial, bool applied, std::string error) {
  manager_.completePermissionMutation(serial, applied, std::move(error));
}

bool PluginRuntimeController::publishSurfaces(
    const plugins::permissions::ActivationBinding &binding,
    const std::vector<std::string> &names, std::string_view canonical_surfaces,
    std::uint64_t epoch) {
  plugins::manifest::ManifestV2 policy_source;
  policy_source.id = binding.plugin.view();
  policy_source.canonical_surfaces = std::string(canonical_surfaces);
  std::vector<SurfaceProjectionModel::SurfaceDeclaration> declarations;
  declarations.reserve(names.size());
  for (const auto &name : names) {
    const auto policy =
        surface_host::parse_named_surface_policy(policy_source, name);
    std::optional<SurfaceProjectionModel::Role> role;
    switch (policy.role) {
    case surface_host::SurfaceRole::bar_embedded:
      role = SurfaceProjectionModel::Role::Bar;
      break;
    case surface_host::SurfaceRole::desktop_overlay:
      role = SurfaceProjectionModel::Role::Overlay;
      break;
    case surface_host::SurfaceRole::panel:
      role = SurfaceProjectionModel::Role::Panel;
      break;
    }
    std::optional<SurfaceProjectionModel::BarSection> section;
    switch (policy.default_bar_section) {
    case surface_host::BarSection::unspecified:
      section = SurfaceProjectionModel::BarSection::Unspecified;
      break;
    case surface_host::BarSection::left:
      section = SurfaceProjectionModel::BarSection::Left;
      break;
    case surface_host::BarSection::center:
      section = SurfaceProjectionModel::BarSection::Center;
      break;
    case surface_host::BarSection::right:
      section = SurfaceProjectionModel::BarSection::Right;
      break;
    }
    if (!role || !section)
      return false;
    declarations.push_back(
        {.surface_name = policy.surface_name,
         .role = *role,
         .initially_visible = policy.initially_visible,
         .maximum_width = policy.maximum_width,
         .maximum_height = policy.maximum_height,
         .dynamic_input_regions = policy.dynamic_input_regions,
         .default_bar_section = *section});
  }
  return manager_.surfaces_.publishSurfaces(binding, std::move(declarations), epoch);
}


bool PluginRuntimeController::attach(const QString &surface_key, QObject *surface) noexcept {
  constexpr qsizetype kMaximumPublishedSurfaceKeyCharacters = 512;
  if (QThread::currentThread() != manager_.thread() || !surface ||
      surface_key.isEmpty() || surface_key.size() > kMaximumPublishedSurfaceKeyCharacters)
    return false;
  auto *remote = qobject_cast<RemotePluginSurface *>(surface);
  if (!remote)
    return false;
  try {
    auto published = manager_.surfaces_.resolve(surface_key);
    if (!published)
      return false;
    auto *presentation = owner_->presentation(published->binding_.plugin.view(),
        published->publication_revision_, published->binding_);
    auto *endpoints = dynamic_cast<SurfaceEndpointOwner *>(presentation);
    return endpoints &&
        endpoints->attach(*published, surface_key, *remote) == SurfaceEndpointAttachResult::attached;
  } catch (...) {
    return false;
  }
}

} // namespace omarchy::plugin_runtime::bridge::detail
