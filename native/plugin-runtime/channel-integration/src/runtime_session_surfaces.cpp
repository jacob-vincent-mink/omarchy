#include "runtime_session_owner.hpp"

#include <QThread>

#include <ranges>

namespace omarchy::plugin_runtime::channel {

RuntimeSessionOwner::HookState::HookState(std::string plugin,
                                              std::uint64_t epoch)
    : plugin(std::move(plugin)), epoch(epoch) {}

RuntimeSessionOwner::Hook::Hook(std::shared_ptr<HookState> state,
                                    RuntimeHost &host)
    : state(std::move(state)), host(&host) {}

bool RuntimeSessionOwner::Hook::update_settings(
    const plugins::permissions::ActivationBinding &binding,
    std::string_view canonical_entry) {
  return host && binding.plugin.view() == state->plugin &&
         host->persistSettings(state->plugin, canonical_entry);
}

void RuntimeSessionOwner::Hook::state_changed(
    host_session::SessionState session_state,
    host_session::SessionError error) {
  const auto packed = static_cast<std::uint16_t>(session_state) |
                      (static_cast<std::uint16_t>(error) << 8U);
  state->lifecycle.store(static_cast<std::uint16_t>(packed + 1U),
                         std::memory_order_release);
}

void RuntimeSessionOwner::Hook::render_rejected(host_session::RouteResult) {
}

bool RuntimeSessionOwner::Hook::accept(
    host_session::AdmittedSurfaceIntent intent) {
  try {
    if (!intent.available() || intent.binding().plugin.view() != state->plugin)
      return false;
    std::scoped_lock lock(state->intent_mutex);
    if (state->intents.size() >= HookState::maximum_pending_surface_intents)
      return false;
    state->intents.push_back(std::move(intent));
    return true;
  } catch (...) {
    return false;
  }
}

void RuntimeSessionOwner::withdraw(Slot &slot) noexcept {
  if (!slot.presentation)
    return;
  slot.presentation->close_all();
  host_.withdrawSurfaces(slot.presentation->binding());
  slot.presentation.reset();
}

void RuntimeSessionOwner::stateChanged(
    std::string_view plugin, std::uint64_t epoch,
    host_session::SessionState state,
    host_session::SessionError error) noexcept {
  auto *slot = exact(plugin, epoch);
  if (slot == nullptr)
    return;
#ifdef OMARCHY_PLUGIN_SESSION_TESTING
  slot->last_state = static_cast<std::uint8_t>(state);
  slot->last_error = static_cast<std::uint8_t>(error);
#endif
  if (state == host_session::SessionState::running &&
      error == host_session::SessionError::none) {
    try {
      const plugins::permissions::PluginId expected(slot->plugin);
      const auto binding =
          slot->root ? slot->root->session_binding() : std::nullopt;
      if (!binding || binding->plugin != expected) {
        fail(*slot);
        return;
      }
      if (slot->lifecycle.expected_binding() && *binding != *slot->lifecycle.expected_binding()) {
        disable(*slot);
        return;
      }
      auto declarations = slot->root->declared_surfaces();
      const std::string exact_plugin(slot->plugin);
      if (!declarations || !publishRunning(exact_plugin, epoch, *binding,
                                           std::move(*declarations))) {
        if (auto *current = exact(exact_plugin, epoch)) {
          fail(*current);
        }
        return;
      }
      if (auto *current = exact(exact_plugin, epoch)) {
        current->lifecycle.apply(Event::running_accepted);
      }
    } catch (...) {
      if (auto *current = exact(plugin, epoch)) {
        fail(*current);
      }
    }
    return;
  }
  if (state == host_session::SessionState::failed ||
      state == host_session::SessionState::stopped ||
      state == host_session::SessionState::revoked) {
    fail(*slot);
  }
}

void RuntimeSessionOwner::drainSurfaceIntents(Slot &slot) noexcept {
  const auto state = slot.callback_state;
  if (!state)
    return;
  std::deque<host_session::AdmittedSurfaceIntent> pending;
  {
    std::scoped_lock lock(state->intent_mutex);
    pending.swap(state->intents);
  }
  for (auto &intent : pending) {
    try {
      if (slot.callback_state != state || slot.lifecycle.epoch() != state->epoch ||
          slot.plugin != state->plugin || slot.lifecycle.phase() != Phase::running)
        continue;
      std::optional<plugins::permissions::ActivationBinding> binding;
      if (slot.root && slot.presentation)
        binding = slot.root->session_binding();
#ifdef OMARCHY_PLUGIN_SESSION_TESTING
      else if (slot.test_surface_endpoint)
        binding = slot.test_running_binding;
#endif
      if (!binding || *binding != intent.binding())
        continue;
      static_cast<void>(host_.publishIntent(std::move(intent)));
    } catch (...) {
    }
  }
}

bool RuntimeSessionOwner::publishRunning(
    std::string_view plugin, std::uint64_t epoch,
    const plugins::permissions::ActivationBinding &binding,
    PluginSession::DeclaredSurfaceSet declarations) noexcept {
  if (QThread::currentThread() != host_.eventOwner().thread() || epoch == 0)
    return false;
  auto *slot = exact(plugin, epoch);
  if (slot == nullptr || slot->lifecycle.phase() != Phase::starting ||
      slot->plugin != binding.plugin.view() || !slot->root || !slot->hook ||
      slot->presentation ||
      (slot->lifecycle.expected_binding() && *slot->lifecycle.expected_binding() != binding))
    return false;
  const auto rollback = [&]() noexcept {
    auto *current = exact(plugin, epoch);
    if (current) {
      current->lifecycle.apply(Event::publication_failed);
      if (current->presentation)
        current->presentation->close_all();
      current->presentation.reset();
    }
    host_.withdrawSurfaces(binding);
  };
  try {
    const auto live_binding = slot->root->session_binding();
    if (!live_binding || *live_binding != binding ||
        declarations.binding != binding ||
        declarations.plugin_id != binding.plugin.view())
      return false;
    if (declarations.names.empty()) {
      return slot->lifecycle.publish(binding);
    }
    slot->presentation = host_.createPresentation(
        binding, epoch, slot->root->surface_session());
    if (!slot->presentation) {
      rollback();
      return false;
    }
    if (!slot->lifecycle.publish(binding)) {
      rollback();
      return false;
    }
    if (!host_.publishSurfaces(binding, declarations.names,
                               declarations.canonical_surfaces, epoch)) {
      rollback();
      return false;
    }
    const auto *published = exact(plugin, epoch);
    if (published && published->lifecycle.phase() == Phase::running &&
        published->presentation && published->root) {
      const auto exact_binding = published->root->session_binding();
      if (exact_binding && *exact_binding == binding)
        return true;
    }
    rollback();
    return false;
  } catch (...) {
    rollback();
    return false;
  }
}

RuntimePresentation *RuntimeSessionOwner::presentation(
    std::string_view plugin, std::uint64_t epoch,
    const permissions::ActivationBinding &binding) noexcept {
  if (QThread::currentThread() != host_.eventOwner().thread())
    return nullptr;
  auto *slot = exact(plugin, epoch);
  if (!slot || slot->lifecycle.phase() != Phase::running || !slot->root ||
      !slot->presentation)
    return nullptr;
  const auto current = slot->root->session_binding();
  return current && *current == binding ? slot->presentation.get() : nullptr;
}

} // namespace omarchy::plugin_runtime::channel
