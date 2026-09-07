#pragma once

#include "capability_fixture.hpp"
#include "temporary_directory.hpp"
#include "authority_store.hpp"
#include "omarchy/plugin_runtime/unique_fd.hpp"

#include <fcntl.h>

namespace omarchy::plugin_runtime::test_support {

struct AuthorityFixture {
  TemporaryDirectory directory;
  std::filesystem::path path = directory.path();
  UniqueFd root;
  definitions::TrustedDefinitionRegistry definitions = packaged_registry();
  std::unique_ptr<host_session::AuthorityStore> store;

  explicit AuthorityFixture(std::string_view plugin, bool open_store = true) {
    root.reset(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    OMARCHY_CHECK(root);
    if (open_store) {
      store = host_session::AuthorityStore::open(
          root.get(), ::getuid(), permissions::PluginId(plugin));
      OMARCHY_CHECK(store != nullptr);
    }
  }
};

} // namespace omarchy::plugin_runtime::test_support
