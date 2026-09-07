#pragma once

#include "authority_store.hpp"
#include "capability_definition.hpp"
#include "consent_review.hpp"
#include "operation_store.hpp"

#include <QJsonArray>
#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace omarchy::plugin_runtime::channel {
class PluginPermissionAuthority;
}

namespace omarchy::plugin_runtime::bridge {

class PluginManager;
namespace detail {
class PluginRuntimeController;
}
#ifdef OMARCHY_PLUGIN_MANAGER_TESTING
class PermissionControlTestAccess;
#endif

// The trusted shell receives only bounded JSON presentation and opaque
// selectors. Exact capability, definition, authority and generation identity
// stays in this manager-owned object and is never accepted back from QML.
class PermissionControl final : public QObject {
  Q_OBJECT
  QML_NAMED_ELEMENT(PermissionControl)
  QML_UNCREATABLE("PermissionControl is owned by PluginManager")

public:
  enum class Kind : std::uint8_t { list, review, apply, revoke };

  ~PermissionControl() override;

  Q_INVOKABLE QString beginList(const QString &plugin_id) noexcept;
  Q_INVOKABLE QString beginReview(const QString &plugin_id) noexcept;
  Q_INVOKABLE QString apply(const QString &review_operation_id,
                            const QString &choices_json) noexcept;
  Q_INVOKABLE QString revoke(const QString &source_operation_id,
                             const QString &row_id) noexcept;
  Q_INVOKABLE QString poll(const QString &operation_id) noexcept;

  // The trusted host shell pairs CLI review/apply through distinct methods;
  // provenance is fixed here and never accepted from JSON.
  Q_INVOKABLE QString
  beginInteractiveCliReview(const QString &plugin_id) noexcept;
  Q_INVOKABLE QString applyInteractiveCli(const QString &review_operation_id,
                                          const QString &choices_json) noexcept;

private:
  using State = detail::OperationState;
  enum class Ingress : std::uint8_t { trusted_ui, interactive_cli };

  struct ExactContext final {
    std::string plugin;
    std::uint64_t slot_epoch = 0;
    std::uint64_t authority_sequence = 0;
    std::shared_ptr<channel::PluginPermissionAuthority> authority;
  };

  struct Row final {
    struct DynamicOperation final {
      std::string id;
      plugins::definitions::Name name;
    };

    std::string id;
    std::size_t index = 0;
    bool required = false;
    bool available = false;
    plugins::definitions::CapabilityReference definition;
    std::vector<DynamicOperation> operations;
  };

  struct Operation final : detail::OperationRecord {
    Kind kind = Kind::list;
    Ingress ingress = Ingress::trusted_ui;
    std::optional<ExactContext> context;
    std::shared_ptr<const host_session::ConsentReview> review;
    std::vector<Row> rows;
  };

  explicit PermissionControl(PluginManager &manager);

  [[nodiscard]] QString beginRead(const QString &plugin_id, Kind kind,
                                  Ingress ingress,
                                  std::optional<plugins::permissions::Digest>
                                      expected_revision = std::nullopt) noexcept;
  [[nodiscard]] QString beginInteractiveCliReviewExact(
      std::string_view plugin,
      std::string_view revision) noexcept;
  [[nodiscard]] QString applyForIngress(const QString &review_operation_id,
                                        const QString &choices_json,
                                        Ingress ingress) noexcept;
  template <typename Begin>
  [[nodiscard]] std::string start(Kind kind, Ingress ingress, Begin begin) {
    prune();
    Operation operation;
    operation.kind = kind;
    operation.ingress = ingress;
    return operations_.start(std::move(begin), std::move(operation));
  }
  [[nodiscard]] static std::optional<
      plugins::permissions::FixedSet<plugins::definitions::Name, 16>>
  selectDynamicOperations(const Row &row, const QJsonArray &selected) noexcept;
  void prune() noexcept;
  void fail(std::uint64_t serial, std::string error) noexcept;
  void completeRead(
      std::uint64_t serial, std::string plugin, std::uint64_t slot_epoch,
      std::shared_ptr<channel::PluginPermissionAuthority> authority,
      std::optional<host_session::AuthorityView> view,
      std::shared_ptr<const host_session::ConsentReview> review) noexcept;
  void completeMutation(std::uint64_t serial, bool applied,
                        std::string error) noexcept;
  void invalidatePlugin(std::string_view plugin) noexcept;

  PluginManager &manager_;
  detail::OperationStore<Operation> operations_{32, "permission"};

  friend class PluginManager;
  friend class PluginInstallControl;
  friend class detail::PluginRuntimeController;
#ifdef OMARCHY_PLUGIN_MANAGER_TESTING
  friend class PermissionControlTestAccess;
#endif
};

#ifdef OMARCHY_PLUGIN_MANAGER_TESTING
class PermissionControlTestAccess final {
public:
  static void ageOperation(PermissionControl &control,
                           const QString &operation_id,
                           std::chrono::minutes age);
};
#endif

} // namespace omarchy::plugin_runtime::bridge
