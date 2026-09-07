#pragma once

#include "runtime_fixture.hpp"
#include "authority_store.hpp"
#include "runtime_bootstrap.hpp"
#include "../../channel-integration/tests/runtime_roots_test_access.hpp"

namespace omarchy::plugin_runtime::test_support {

// Only valid fixture construction is shared. Callers retain the returned
// bootstrap error so malformed package/provider scenarios exercise admission.
class RuntimeBootstrapTree : public RuntimeTree {
public:
  [[nodiscard]] std::unique_ptr<host_session::AuthorityStore>
  open_authority_store(std::string_view plugin) const {
    UniqueFd descriptor(::open(authority(plugin).c_str(),
                                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    require(static_cast<bool>(descriptor), "fixture authority descriptor unavailable");
    auto store = host_session::AuthorityStore::open(
        descriptor.get(), ::getuid(), plugins::permissions::PluginId(plugin));
    descriptor.reset();
    require(store != nullptr, "fixture authority store unavailable");
    return store;
  }

  [[nodiscard]] int open_root() const {
    return ::open(root_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  }

  [[nodiscard]] std::unique_ptr<channel::RuntimeRoots> roots() const {
    UniqueFd home(::open(home_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    require(static_cast<bool>(home), "fixture home descriptor unavailable");
    channel::RuntimeRootsError error{};
    auto result = channel::RuntimeRootsTestAccess::open_from_home_fd(
        home.get(), static_cast<std::uint32_t>(::getuid()), error);
    require(result && error == channel::RuntimeRootsError::none,
            "fixture runtime roots rejected");
    return result;
  }

  [[nodiscard]] std::unique_ptr<channel::RuntimeBootstrap>
  open_bootstrap(channel::RuntimeBootstrapError &error) const {
    UniqueFd root(open_root());
    require(static_cast<bool>(root), "fixture filesystem root unavailable");
    return channel::RuntimeBootstrapTestAccess::open_from_filesystem_root(
        roots(), root.get(), static_cast<std::uint32_t>(::getuid()), error);
  }
};

} // namespace omarchy::plugin_runtime::test_support
