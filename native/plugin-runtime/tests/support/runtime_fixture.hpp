#pragma once

#include "temporary_directory.hpp"
#include "revision_verifier_adapter.hpp"
#include "omarchy/plugin_runtime/Version.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <string>

namespace omarchy::plugin_runtime::test_support {

class RuntimeTree : public TemporaryDirectory {
public:
  RuntimeTree() {
    require(::chmod(root_.c_str(), 0755) == 0, "cannot secure fixture root");
    home_ = root_ / "home";
    create(home_, 0700);
    for (const auto &path : {revisions(), activations(), authority(), state()})
      create(path, 0700);
    create(package(), 0755);
  }

  RuntimeTree(const RuntimeTree &) = delete;
  RuntimeTree &operator=(const RuntimeTree &) = delete;
  std::filesystem::path package() const {
    return root_ / "usr/lib/omarchy/plugin-security" /
           std::string(build_version()) / "capabilities.d";
  }
  std::filesystem::path admin() const {
    return root_ / "etc/omarchy/plugin-capabilities.d";
  }
  std::filesystem::path revisions() const {
    return home_ / ".local/share/omarchy-plugin-security/v2/revisions";
  }
  std::filesystem::path activations() const {
    return home_ / ".local/state/omarchy/plugin-security/v2/activations";
  }
  std::filesystem::path authority(std::string_view plugin = {}) const {
    auto path = home_ / ".local/state/omarchy/plugin-security/v2/authority";
    return plugin.empty() ? path : path / std::string(plugin);
  }
  std::filesystem::path state() const {
    return home_ / ".local/state/omarchy/plugin-security/v2/state";
  }

  void create(const std::filesystem::path &path, mode_t leaf_mode) {
    auto current = root_;
    for (const auto &part : path.lexically_relative(root_)) {
      require(part != "..", "fixture path escaped its root");
      current /= part;
      const bool created = std::filesystem::create_directory(current);
      if (created || current == path)
        require(::chmod(current.c_str(), current == path ? leaf_mode : 0755) == 0,
                "cannot set fixture directory mode");
    }
  }

protected:
  std::filesystem::path home_;
};

inline host_session::VerifiedRevision freeze_revision(
    const std::filesystem::path &path, std::string_view plugin) {
  for (const auto &entry : std::filesystem::recursive_directory_iterator(path))
    require(::chmod(entry.path().c_str(), entry.is_directory() ? 0555 : 0444) == 0,
            "cannot freeze fixture content");
  require(::chmod(path.c_str(), 0555) == 0, "cannot freeze fixture revision");
  host_session::UniqueFd descriptor(
      ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  require(descriptor.get() >= 0, "cannot open fixture revision");
  auto verified = host_session::DescriptorRevisionVerifier{::getuid()}.verify_open_revision(
      descriptor.get());
  require(verified && verified->manifest.id == plugin,
          "fixture revision verification failed");
  return std::move(*verified);
}

} // namespace omarchy::plugin_runtime::test_support
