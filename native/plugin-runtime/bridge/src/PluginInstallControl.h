#pragma once
#include "operation_store.hpp"

#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include <cstdint>
#include <string>

namespace omarchy::plugin_runtime::bridge {

class PluginManager;

// Owns the opaque public handle for one verified archive candidate. The exact
// revision never crosses into QML and can only be consumed by exact review.
class PluginInstallControl final : public QObject {
  Q_OBJECT
  QML_NAMED_ELEMENT(PluginInstallControl)
  QML_UNCREATABLE("PluginInstallControl is owned by PluginManager")

public:
  ~PluginInstallControl() override;

  Q_INVOKABLE QString begin(const QString &archive_path) noexcept;
  Q_INVOKABLE QString poll(const QString &operation_id) noexcept;
  Q_INVOKABLE QString beginReview(const QString &operation_id) noexcept;

private:
  using State = detail::OperationState;
  struct Operation final : detail::OperationRecord {
    std::string plugin;
    std::string revision;
  };

  explicit PluginInstallControl(PluginManager &manager);
  void complete(std::uint64_t serial, std::string plugin,
                std::string revision, std::string error) noexcept;
  void prune() noexcept;

  PluginManager &manager_;
  detail::OperationStore<Operation> operations_{8, "install"};

  friend class PluginManager;
};

} // namespace omarchy::plugin_runtime::bridge
