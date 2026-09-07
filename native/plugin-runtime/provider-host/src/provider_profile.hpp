#pragma once

#include "omarchy/plugin_runtime/unique_fd.hpp"

#include "omarchy/plugin_runtime/provider_host/provider_host.hpp"

#include <unistd.h>

#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace omarchy::plugin_runtime::provider_host::detail {

using ::omarchy::plugin_runtime::UniqueFd;

} // namespace omarchy::plugin_runtime::provider_host::detail

namespace omarchy::plugin_runtime::provider_host {

struct ProviderCatalog::Profile final {
  definitions::AdapterBinding binding;
  std::string group;
  std::string executable_path;
  definitions::Digest executable_digest;
  std::vector<std::string> arguments;
  std::vector<std::string> inherited_environment;
  std::optional<std::chrono::milliseconds> invocation_timeout;
  detail::UniqueFd executable;
};

} // namespace omarchy::plugin_runtime::provider_host
