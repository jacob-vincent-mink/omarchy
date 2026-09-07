#include "PluginInstallControl.h"

#include "PluginManager.h"
#include "activation_snapshot.hpp"

#include <QJsonObject>

#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

namespace omarchy::plugin_runtime::bridge {
namespace {

constexpr auto kPendingLifetime = std::chrono::minutes(5);
constexpr auto kCompletedLifetime = std::chrono::minutes(1);
constexpr std::uint64_t kMaximumArchiveBytes = 72U * 1024U * 1024U;

} // namespace

PluginInstallControl::PluginInstallControl(PluginManager &manager)
    : QObject(&manager), manager_(manager) {}

PluginInstallControl::~PluginInstallControl() = default;

QString PluginInstallControl::begin(const QString &archive_path) noexcept {
  try {
    prune();
    const auto encoded = archive_path.toUtf8();
    if (encoded.isEmpty() || encoded.size() >= PATH_MAX ||
        encoded.contains('\0') || operations_.full())
      return {};
    const int fd = ::open(encoded.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW |
                                                  O_NONBLOCK);
    if (fd < 0)
      return {};
    host_session::UniqueFd descriptor(fd);
    struct stat status {};
    if (::fstat(descriptor.get(), &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_size <= 0 ||
        static_cast<std::uint64_t>(status.st_size) > kMaximumArchiveBytes)
      return {};
    const auto id = operations_.start([&](auto serial) {
      return manager_.beginInstall(serial, descriptor.release());
    });
    if (id.empty())
      return {};
    return QString::fromStdString(id);
  } catch (...) {
    return {};
  }
}

QString PluginInstallControl::poll(const QString &operation_id) noexcept {
  try {
    prune();
    auto *operation = operations_.find(operation_id);
    if (!operation) return {};
    const QJsonObject result{
        {QStringLiteral("plugin"), QString::fromStdString(operation->plugin)}};
    return operation->poll(operation_id, {}, &result);
  } catch (...) {
    return {};
  }
}

QString PluginInstallControl::beginReview(
    const QString &operation_id) noexcept {
  try {
    prune();
    auto *operation = operations_.find(operation_id);
    if (!operation || operation->state != State::succeeded ||
        operation->consumed)
      return {};
    const auto review = manager_.permissions_.beginInteractiveCliReviewExact(
        operation->plugin, operation->revision);
    if (!review.isEmpty())
      operation->consumed = true;
    return review;
  } catch (...) {
    return {};
  }
}

void PluginInstallControl::complete(std::uint64_t serial, std::string plugin,
                                    std::string revision,
                                    std::string error) noexcept {
  auto *found = operations_.find(serial);
  if (!found)
    return;
  found->touched = std::chrono::steady_clock::now();
  if (!error.empty()) {
    found->state = State::failed;
    found->error = std::move(error);
    return;
  }
  found->state = State::succeeded;
  found->plugin = std::move(plugin);
  found->revision = std::move(revision);
}

void PluginInstallControl::prune() noexcept {
  operations_.prune([](const Operation &operation) {
    return operation.state == State::pending ? kPendingLifetime : kCompletedLifetime;
  });
}

} // namespace omarchy::plugin_runtime::bridge
