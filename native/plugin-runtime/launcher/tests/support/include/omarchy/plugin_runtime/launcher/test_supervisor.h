#pragma once

#include "omarchy/plugin_runtime/launcher/launcher.h"

#include <memory>
#include <string>
#include <string_view>

namespace omarchy::plugin_runtime::launcher::test_support {

// These test bases supply only the explicitly successful setup stages.
class ReadyScope : public ResourceScopeController {
public:
  bool probe(Deadline, std::string &) override { return true; }
  bool prepare_cleanup(Deadline, std::string &) override { return true; }
};

class AttachedScope : public ReadyScope {
protected:
  AttachResult attach_validated(const ProcessScopeRequest &, Deadline,
                                std::string &) override {
    return {.attached = true, .cleanup_required = true};
  }
};

[[nodiscard]] Supervisor
make_supervisor(std::string bwrap_path, std::string worker_path,
                std::shared_ptr<ResourceScopeController> resource_scope,
                bool force_reaper_start_failure = false);

[[nodiscard]] bool connect_bus(std::string_view address, Deadline deadline,
                               std::string &error) noexcept;

} // namespace omarchy::plugin_runtime::launcher::test_support
