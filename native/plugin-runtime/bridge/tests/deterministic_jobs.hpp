#pragma once

#include "PluginManager.h"
#include "../../tests/support/test_assert.hpp"

#include <algorithm>
#include <functional>
#include <thread>
#include <vector>

namespace omarchy::plugin_runtime::bridge::test_support {

class DeterministicJobs final {
public:
  void install(bridge::PluginManager &manager) {
    bridge::PluginManagerTestAccess::setJobSubmitter(
        manager, [this](auto kind, auto job) {
          return submit(kind, std::move(job));
        });
  }

  bool submit(bridge::PluginManagerTestAccess::TestJobKind kind,
              std::function<void()> job) {
    if (throws)
      throw std::runtime_error("injected submit failure");
    if (refuses)
      return false;
    kinds.push_back(kind);
    jobs.push_back(std::move(job));
    peak = std::max(peak, jobs.size());
    return true;
  }

  void runOne() { runAt(0); }

  void runThread(std::size_t index = 0) {
    std::thread worker([&] { runAt(index); });
    worker.join();
  }

  void runAt(std::size_t index) {
    OMARCHY_CHECK(!jobs.empty());
    OMARCHY_CHECK(index < jobs.size());
    auto job = std::move(jobs[index]);
    jobs.erase(jobs.begin() + static_cast<std::ptrdiff_t>(index));
    kinds.erase(kinds.begin() + static_cast<std::ptrdiff_t>(index));
    job();
  }

  void runAndDrain(bridge::PluginManager &manager) {
    runThread();
    bridge::PluginManagerTestAccess::drainRuntime(manager);
  }

  void runInlineAndDrain(bridge::PluginManager &manager, std::size_t index = 0) {
    runAt(index);
    bridge::PluginManagerTestAccess::drainRuntime(manager);
  }

  QString completeOperation(bridge::PluginManager &manager, QString operation) {
    OMARCHY_CHECK(!operation.isEmpty() && jobs.size() == 1);
    runAndDrain(manager);
    return operation;
  }

  std::vector<bridge::PluginManagerTestAccess::TestJobKind> kinds;
  std::vector<std::function<void()>> jobs;
  std::size_t peak = 0;
  bool refuses = false;
  bool throws = false;
};

} // namespace omarchy::plugin_runtime::bridge::test_support
