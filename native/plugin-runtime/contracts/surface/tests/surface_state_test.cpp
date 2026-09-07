#include "test.hpp"

#include "omarchy/plugin_runtime/surface/surface_state.hpp"

using namespace omarchy::plugin_runtime::surface;

int main() {
  const auto allocation =
      make_allocation({.id = 4, .generation = 1}, 20, 10, 20, 10, 1, 1, 4096);
  OMARCHY_CHECK_WITH(require, allocation.has_value());
  auto state = SurfaceState::create_active(*allocation);
  OMARCHY_CHECK_WITH(require, state && state->phase() == SurfacePhase::active);
  auto inconsistent = *allocation;
  ++inconsistent.stride;
  OMARCHY_CHECK_WITH(require, !SurfaceState::create_active(inconsistent));
  OMARCHY_CHECK_WITH(require, !state->apply(SurfaceTransition::resume));
  OMARCHY_CHECK_WITH(require, state->accepts_frame(allocation->surface));
  OMARCHY_CHECK_WITH(require, !state->accepts_frame({.id = 4, .generation = 2}));
  OMARCHY_CHECK_WITH(require, !state->accepts_input(allocation->surface, false));
  OMARCHY_CHECK_WITH(require, state->apply(SurfaceTransition::suspend));
  OMARCHY_CHECK_WITH(require, !state->accepts_frame(allocation->surface));
  OMARCHY_CHECK_WITH(require, state->apply(SurfaceTransition::resume));
  OMARCHY_CHECK_WITH(require, state->apply(SurfaceTransition::destroy));
  OMARCHY_CHECK_WITH(require, !state->apply(SurfaceTransition::resume));
  OMARCHY_CHECK_WITH(require, !state->apply(SurfaceTransition::destroy));
  OMARCHY_CHECK_WITH(require, !state->apply(SurfaceTransition::suspend));
  OMARCHY_CHECK_WITH(require, !state->accepts_frame(allocation->surface) &&
                            !state->accepts_input(allocation->surface, true));
}
