#include "runtime_session_owner.hpp"
#include "../../tests/support/runtime_bootstrap_fixture.hpp"
#include "../../tests/support/test_assert.hpp"

#include <QCoreApplication>
#include <deque>
#include <stdexcept>
#include <thread>

namespace channel = omarchy::plugin_runtime::channel;
namespace host_session = omarchy::plugin_runtime::host_session;
namespace permissions = omarchy::plugins::permissions;
using namespace omarchy::plugin_runtime::test_support;

namespace omarchy::plugin_runtime::channel {
class RuntimeSessionOwnerTestAccess final {
public:
  static std::unique_ptr<RuntimeSessionOwner> manual(
      RuntimeHost &host, std::unique_ptr<RuntimeBootstrap> bootstrap,
      std::deque<std::function<void()>> &jobs) {
    auto owner = std::unique_ptr<RuntimeSessionOwner>(new RuntimeSessionOwner(
        host, std::move(bootstrap), RuntimeSessionOwner::ManualTestTag{}));
    owner->job_submitter_ = [&jobs](RuntimeSessionOwner::JobKind, std::function<void()> job) {
      jobs.push_back(std::move(job));
      return true;
    };
    return owner;
  }
  static void scan(RuntimeSessionOwner &owner) { owner.requestScan(); }
  static void drain(RuntimeSessionOwner &owner) { owner.drainCompletions(); }
  static std::weak_ptr<const void> gate(RuntimeSessionOwner &owner) { return owner.gate_; }
};
} // namespace omarchy::plugin_runtime::channel

namespace {
// Deliberately not a PluginManager or a QML object: runtime orchestration must
// work with a host projection port alone.
class Host final : public channel::RuntimeHost {
public:
  QObject &eventOwner() noexcept override { return events; }
  void catalogAvailable() override { ++catalogs; }
  void invalidatePlugin(std::string_view) override { ++unexpected; }
  std::optional<std::string> currentSettings(std::string_view) override { return {}; }
  std::optional<std::string> currentPresentation() override { return {}; }
  bool persistSettings(std::string_view, std::string_view) override { ++unexpected; return false; }
  bool publishIntent(host_session::AdmittedSurfaceIntent) override { ++unexpected; return false; }
  std::unique_ptr<channel::RuntimePresentation> createPresentation(
      const permissions::ActivationBinding &, std::uint64_t,
      channel::SurfaceSessionPort &) override { ++unexpected; return {}; }
  bool publishSurfaces(const permissions::ActivationBinding &,
      const std::vector<std::string> &, std::string_view, std::uint64_t) override {
    ++unexpected;
    return false;
  }
  void withdrawSurfaces(const permissions::ActivationBinding &) noexcept override { ++unexpected; }
  void completeInstall(std::uint64_t, std::string, std::string, std::string) override { ++unexpected; }
  void failPermissionControl(std::uint64_t, std::string) override { ++unexpected; }
  void completePermissionRead(std::uint64_t, std::string, std::uint64_t,
      std::shared_ptr<channel::PluginPermissionAuthority>,
      std::optional<host_session::AuthorityView>,
      std::shared_ptr<const host_session::ConsentReview>) override { ++unexpected; }
  void completePermissionMutation(std::uint64_t, bool, std::string) override { ++unexpected; }
  QObject events;
  int catalogs = 0;
  int unexpected = 0;
};

std::unique_ptr<channel::RuntimeBootstrap> bootstrap(RuntimeBootstrapTree &tree) {
  channel::RuntimeBootstrapError error{};
  auto result = tree.open_bootstrap(error);
  OMARCHY_CHECK(result && error == channel::RuntimeBootstrapError::none);
  return result;
}

void run_one(std::deque<std::function<void()>> &jobs) {
  OMARCHY_CHECK(jobs.size() == 1);
  auto job = std::move(jobs.front());
  jobs.pop_front();
  job();
}

void runtime_owns_scan_delivery_and_cancellation() {
  RuntimeBootstrapTree tree;
  Host host;
  std::deque<std::function<void()>> jobs;
  auto owner = channel::RuntimeSessionOwnerTestAccess::manual(host, bootstrap(tree), jobs);
  channel::RuntimeSessionOwnerTestAccess::scan(*owner);
  channel::RuntimeSessionOwnerTestAccess::scan(*owner);
  OMARCHY_CHECK(jobs.size() == 1 && host.catalogs == 0);
  run_one(jobs);
  // Completion remains bounded and in flight until the owning loop consumes it.
  channel::RuntimeSessionOwnerTestAccess::scan(*owner);
  OMARCHY_CHECK(jobs.empty() && host.catalogs == 0);
  channel::RuntimeSessionOwnerTestAccess::drain(*owner);
  OMARCHY_CHECK(host.catalogs == 1 && host.unexpected == 0);
  channel::RuntimeSessionOwnerTestAccess::scan(*owner);
  auto gate = channel::RuntimeSessionOwnerTestAccess::gate(*owner);
  owner.reset();
  OMARCHY_CHECK(!gate.expired());
  run_one(jobs);
  OMARCHY_CHECK(gate.expired() && host.catalogs == 1 && host.unexpected == 0);
}

void destruction_cancels_initial_event_turn() {
  RuntimeBootstrapTree tree;
  Host host;
  {
    channel::RuntimeSessionOwner owner(host, bootstrap(tree));
  }
  QCoreApplication::processEvents();
  OMARCHY_CHECK(host.catalogs == 0 && host.unexpected == 0);
}

void wrong_thread_construction_is_rejected() {
  RuntimeBootstrapTree tree;
  Host host;
  auto prepared = bootstrap(tree);
  bool rejected = false;
  std::thread worker([&] {
    try {
      channel::RuntimeSessionOwner owner(host, std::move(prepared));
    } catch (const std::invalid_argument &) {
      rejected = true;
    }
  });
  worker.join();
  OMARCHY_CHECK(rejected && host.catalogs == 0 && host.unexpected == 0);
}
} // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  return test_main([] {
    runtime_owns_scan_delivery_and_cancellation();
    destruction_cancels_initial_event_turn();
    wrong_thread_construction_is_rejected();
    return 0;
  });
}
