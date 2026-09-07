#pragma once

#include "omarchy/plugin_runtime/surface/shared_layout.hpp"

#include <cstdint>
#include <optional>

namespace omarchy::plugin_runtime::surface {

enum class SurfacePhase { active, suspended, destroyed };
enum class SurfaceTransition {
  suspend,
  resume,
  destroy,
};

class SurfaceState {
public:
  [[nodiscard]] static std::optional<SurfaceState>
  create_active(const TrustedAllocation &allocation);

  [[nodiscard]] bool apply(SurfaceTransition transition);
  [[nodiscard]] bool accepts_frame(SurfaceKey key) const;
  [[nodiscard]] bool accepts_input(SurfaceKey key, bool focused) const;
  [[nodiscard]] SurfacePhase phase() const;
  [[nodiscard]] const TrustedAllocation &allocation() const;

private:
  explicit SurfaceState(TrustedAllocation allocation);

  TrustedAllocation allocation_;
  SurfacePhase phase_ = SurfacePhase::active;
};

} // namespace omarchy::plugin_runtime::surface
