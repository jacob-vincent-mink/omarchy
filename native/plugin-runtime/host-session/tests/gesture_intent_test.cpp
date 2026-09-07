#include "../../tests/support/gesture_clock.hpp"
#include "../../tests/support/test_assert.hpp"

#include "gesture_intent.hpp"

#include <stdexcept>
#include <string>
#include <memory>
#include <type_traits>

namespace host = omarchy::plugin_runtime::host_session;
namespace permissions = omarchy::plugins::permissions;
namespace runtime = omarchy::plugin_runtime::runtime;
namespace surface = omarchy::plugin_runtime::surface;
namespace wire = omarchy::plugin::wire;

namespace {
static_assert(!std::is_copy_constructible_v<host::AdmittedSurfaceIntent>);
static_assert(!std::is_constructible_v<host::SurfaceIntentPublication, host::AdmittedSurfaceIntent>);
using Clock = omarchy::plugin_runtime::test_support::GestureClock<100>;
using omarchy::plugin_runtime::test_support::require;
permissions::Digest digest(char value) {
  return permissions::Digest(std::string(64, value));
}
} // namespace

int main() {
  auto clock = std::make_shared<Clock>();
  runtime::GestureEligibilityLatch eligibility(clock);
  const permissions::ActivationBinding binding{
      .plugin = permissions::PluginId("fixture.plugin"),
      .revision = digest('1'),
      .policy_fingerprint = digest('2'),
      .generation = 7};
  host::GestureIntentAuthority authority(binding, eligibility);
  const surface::SurfaceKey bar{.id = 1, .generation = 7};
  const surface::SurfaceKey panel{.id = 2, .generation = 7};
  const surface::SurfaceKey maximum{.id = 3, .generation = 7};
  const surface::SurfaceKey invalid{.id = 4, .generation = 7};
  OMARCHY_CHECK(authority.declare_surface(bar, "bar") ==
              host::SurfaceDeclarationResult::declared &&
              authority.declare_surface(panel, "PanelWidget") ==
                  host::SurfaceDeclarationResult::declared &&
              authority.declare_surface(maximum, std::string(64, 'X')) ==
                  host::SurfaceDeclarationResult::declared &&
              authority.declare_surface(invalid, "Panel.Widget") ==
                  host::SurfaceDeclarationResult::invalid &&
              authority.declare_surface(invalid, std::string(65, 'X')) ==
                  host::SurfaceDeclarationResult::invalid);
  OMARCHY_CHECK(!authority.arm(bar, 10) && authority.attach_surface(bar) &&
              !authority.attach_surface(bar) && authority.arm(bar, 11));
  const surface::SurfaceIntentRequest request{
      .source = bar,
      .target = panel,
      .input_sequence = 11,
      .action = surface::SurfaceIntentAction::toggle,
      .requested_output = "DP-1"};
  auto admitted = authority.admit(request);
  OMARCHY_CHECK(admitted.intent && admitted.intent->available() &&
              admitted.intent->source_name() == "bar" &&
              admitted.intent->target_name() == "PanelWidget" &&
              admitted.intent->action() == surface::SurfaceIntentAction::toggle &&
              admitted.intent->requested_output() == "DP-1");
  auto moved = std::move(*admitted.intent);
  OMARCHY_CHECK(moved.available() && !admitted.intent->available());
  OMARCHY_CHECK(moved.binding() == binding && moved.source() == bar && moved.target() == panel &&
              moved.requested_output() == "DP-1" && admitted.intent->source_name().empty() &&
              admitted.intent->requested_output().empty() && !admitted.intent->take_if_fresh());
  OMARCHY_CHECK(authority.admit(request).failure ==
              host::SurfaceIntentAdmissionFailure::gesture_missing);

  OMARCHY_CHECK(authority.arm(bar, 12));
  auto invalid_output = request;
  invalid_output.input_sequence = 12;
  invalid_output.requested_output = std::string(129, 'x');
  OMARCHY_CHECK(authority.admit(invalid_output).failure ==
              host::SurfaceIntentAdmissionFailure::malformed);
  auto unknown = request;
  unknown.input_sequence = 12;
  unknown.target.id = 99;
  OMARCHY_CHECK(authority.admit(unknown).failure ==
              host::SurfaceIntentAdmissionFailure::unknown_target &&
              authority.admit(request).failure ==
                  host::SurfaceIntentAdmissionFailure::gesture_missing);

  OMARCHY_CHECK(authority.attach_surface(panel) && authority.arm(bar, 13) &&
              authority.detach_surface(panel));
  auto surviving_source = request;
  surviving_source.input_sequence = 13;
  auto detached_target = authority.admit(surviving_source);
  OMARCHY_CHECK(detached_target.intent &&
              detached_target.intent->target_name() == "PanelWidget");

  surviving_source.input_sequence = 14;
  OMARCHY_CHECK(authority.arm(bar, 14));
  authority.clear_surface_eligibility(maximum);
  OMARCHY_CHECK(authority.admit(surviving_source).intent.has_value());

  surviving_source.input_sequence = 15;
  OMARCHY_CHECK(authority.arm(bar, 15));
  authority.clear_surface_eligibility(bar);
  OMARCHY_CHECK(authority.admit(surviving_source).failure ==
              host::SurfaceIntentAdmissionFailure::gesture_missing);

  surviving_source.input_sequence = 16;
  OMARCHY_CHECK(authority.arm(bar, 16) && authority.detach_surface(bar));
  OMARCHY_CHECK(authority.admit(surviving_source).failure ==
              host::SurfaceIntentAdmissionFailure::gesture_missing);
  OMARCHY_CHECK(!authority.arm(bar, 17));
  OMARCHY_CHECK(authority.attach_surface(bar) && authority.arm(bar, 18));
  auto stale_target = request;
  stale_target.input_sequence = 18;
  stale_target.target.generation++;
  auto canonical_after_spoof = request;
  canonical_after_spoof.input_sequence = 18;
  OMARCHY_CHECK(authority.admit(stale_target).failure ==
                  host::SurfaceIntentAdmissionFailure::stale_activation &&
              authority.admit(canonical_after_spoof).failure ==
                  host::SurfaceIntentAdmissionFailure::gesture_missing);
  auto dismiss = authority.admit(
      {.source = panel,
       .target = panel,
       .input_sequence = 0,
       .action = surface::SurfaceIntentAction::dismiss,
       .requested_output = {}});
  OMARCHY_CHECK(dismiss.intent && dismiss.intent->source_name() == "PanelWidget" &&
              dismiss.intent->target_name() == "PanelWidget" &&
              dismiss.intent->input_sequence() == 0 &&
              dismiss.intent->take_if_fresh().has_value());
  OMARCHY_CHECK(authority
                  .admit({.source = bar,
                          .target = panel,
                          .input_sequence = 0,
                          .action = surface::SurfaceIntentAction::dismiss,
                          .requested_output = {}})
                  .failure == host::SurfaceIntentAdmissionFailure::malformed &&
              authority
                      .admit({.source = panel,
                              .target = panel,
                              .input_sequence = 18,
                              .action =
                                  surface::SurfaceIntentAction::dismiss,
                              .requested_output = {}})
                      .failure ==
                  host::SurfaceIntentAdmissionFailure::malformed);
  authority.revoke();
  OMARCHY_CHECK(!authority.attach_surface(panel) && !authority.arm(bar, 19) &&
              authority.admit(request).failure ==
                  host::SurfaceIntentAdmissionFailure::revoked);

  runtime::GestureEligibilityLatch destruction_eligibility(clock);
  std::unique_ptr<host::AdmittedSurfaceIntent> after_destruction;
  {
    host::GestureIntentAuthority temporary(binding,
                                            destruction_eligibility);
    OMARCHY_CHECK(temporary.declare_surface(bar, "BarWidget") ==
                host::SurfaceDeclarationResult::declared &&
                temporary.declare_surface(panel, "PanelWidget") ==
                    host::SurfaceDeclarationResult::declared &&
                temporary.attach_surface(bar) &&
                temporary.arm(bar, 21));
    auto result = temporary.admit(
        {.source = bar,
         .target = panel,
         .input_sequence = 21,
         .action = surface::SurfaceIntentAction::open,
         .requested_output = {}});
    OMARCHY_CHECK(result.intent.has_value());
    after_destruction = std::make_unique<host::AdmittedSurfaceIntent>(
        std::move(*result.intent));
  }
  OMARCHY_CHECK(!after_destruction->take_if_fresh());

  runtime::GestureEligibilityLatch capacity_eligibility(clock);
  host::GestureIntentAuthority capacity(binding, capacity_eligibility);
  for (std::size_t index = 0; index < wire::kMaximumPluginSurfaces; ++index) {
    OMARCHY_CHECK(capacity.declare_surface(
                {.id = 100 + index, .generation = binding.generation},
                "surface" + std::to_string(index)) ==
                host::SurfaceDeclarationResult::declared);
  }
  OMARCHY_CHECK(capacity.declare_surface(
              {.id = 999, .generation = binding.generation}, "overflow") ==
              host::SurfaceDeclarationResult::capacity_exceeded);
}
