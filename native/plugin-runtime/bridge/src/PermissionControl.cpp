#include "PermissionControl.h"

#include "PluginManager.h"
#include "omarchy/plugin/wire/permission_snapshot.hpp"
#include "plugin_permission_authority.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <algorithm>
#include <chrono>
#include <ranges>
#include <string_view>

namespace omarchy::plugin_runtime::bridge {
namespace {

// One 1 KiB choice lane covers the fixed JSON keys, a 128-bit opaque row ID,
// and sixteen 128-bit opaque operation IDs. The wire contract supplies the
// exact manifest row bound, so every valid review fits without unbounded input.
constexpr qsizetype kMaximumChoiceRowBytes = 1024;
constexpr qsizetype kMaximumChoicesBytes =
    96 +
    static_cast<qsizetype>(
        omarchy::plugin::wire::permission_snapshot::kMaximumManifestRequests) *
        kMaximumChoiceRowBytes;
constexpr auto kCompletedLifetime = std::chrono::minutes(1);
constexpr auto kReviewLifetime = std::chrono::minutes(15);
constexpr auto kPendingLifetime = std::chrono::minutes(5);

namespace definitions = omarchy::plugins::definitions;
namespace permissions = omarchy::plugins::permissions;

QString text(std::string_view value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

using detail::opaque_id;

QString kind_name(PermissionControl::Kind kind) {
  switch (kind) {
  case PermissionControl::Kind::list:
    return QStringLiteral("list");
  case PermissionControl::Kind::review:
    return QStringLiteral("review");
  case PermissionControl::Kind::apply:
    return QStringLiteral("apply");
  case PermissionControl::Kind::revoke:
    return QStringLiteral("revoke");
  }
  return QStringLiteral("invalid");
}

QString grant_state(permissions::GrantState state) {
  switch (state) {
  case permissions::GrantState::granted:
    return QStringLiteral("granted");
  case permissions::GrantState::denied:
    return QStringLiteral("denied");
  case permissions::GrantState::revoked:
    return QStringLiteral("revoked");
  }
  return QStringLiteral("denied");
}

QString delta_name(host_session::ConsentDeltaKind delta) {
  using Delta = host_session::ConsentDeltaKind;
  switch (delta) {
  case Delta::unchanged:
    return QStringLiteral("unchanged");
  case Delta::scope_changed:
    return QStringLiteral("scope-changed");
  case Delta::added:
    return QStringLiteral("added");
  case Delta::removed:
    return QStringLiteral("removed");
  case Delta::requirement_changed:
    return QStringLiteral("requirement-changed");
  case Delta::definition_changed:
    return QStringLiteral("definition-changed");
  case Delta::operations_changed:
    return QStringLiteral("operations-changed");
  }
  return QStringLiteral("invalid");
}

template <typename Values> QJsonArray dynamic_operations(const Values &values) {
  QJsonArray result;
  for (const auto &operation : values.values())
    result.push_back(text(operation.view()));
  return result;
}

QJsonObject dynamic_row(
    std::string_view row_id, const definitions::DynamicRequest &request,
    const std::optional<definitions::DynamicGrant> &grant,
    const std::optional<definitions::CapabilityDefinition> &trusted_definition,
    bool available, QJsonArray operations) {
  QJsonObject result{
      {QStringLiteral("rowId"), text(row_id)},
      {QStringLiteral("name"), text(request.definition.canonical_name.view())},
      {QStringLiteral("trustTier"), trusted_definition
          ? text(definitions::trust_tier_name(definitions::trust_tier(
                trusted_definition->enforcement_family)))
          : QStringLiteral("unknown")},
      {QStringLiteral("required"), request.required},
      {QStringLiteral("available"), available},
      {QStringLiteral("state"),
       grant ? grant_state(grant->state) : QStringLiteral("undecided")},
      {QStringLiteral("scope"), text(request.scope.view())},
      {QStringLiteral("operations"), operations}};
  if (trusted_definition) {
    result.insert(QStringLiteral("title"),
                  text(trusted_definition->title.view()));
    result.insert(QStringLiteral("category"),
                  text(trusted_definition->display_category_label.view()));
  }
  return result;
}

} // namespace

PermissionControl::PermissionControl(PluginManager &manager)
    : QObject(&manager), manager_(manager) {
}

PermissionControl::~PermissionControl() = default;

QString PermissionControl::beginList(const QString &plugin_id) noexcept {
  return beginRead(plugin_id, Kind::list, Ingress::trusted_ui);
}

QString PermissionControl::beginReview(const QString &plugin_id) noexcept {
  return beginRead(plugin_id, Kind::review, Ingress::trusted_ui);
}

QString PermissionControl::beginInteractiveCliReview(
    const QString &plugin_id) noexcept {
  return beginRead(plugin_id, Kind::review, Ingress::interactive_cli);
}

QString PermissionControl::beginInteractiveCliReviewExact(
    std::string_view plugin, std::string_view revision) noexcept {
  try {
    const permissions::PluginId exact_plugin(plugin);
    const permissions::Digest exact_revision(revision);
    return beginRead(QString::fromUtf8(exact_plugin.view().data(),
                                      static_cast<qsizetype>(exact_plugin.view().size())),
                     Kind::review, Ingress::interactive_cli, exact_revision);
  } catch (...) {
    return {};
  }
}

QString
PermissionControl::applyInteractiveCli(const QString &review_operation_id,
                                       const QString &choices_json) noexcept {
  return applyForIngress(review_operation_id, choices_json,
                         Ingress::interactive_cli);
}

QString PermissionControl::beginRead(const QString &plugin_id, Kind kind,
                                     Ingress ingress,
                                     std::optional<permissions::Digest>
                                         expected_revision) noexcept {
  try {
    const auto encoded = plugin_id.toUtf8();
    const permissions::PluginId exact(std::string_view(
        encoded.constData(), static_cast<std::size_t>(encoded.size())));
    const auto id = start(kind, ingress, [&](auto serial) {
      return manager_.beginPermissionRead(serial, std::string(exact.view()),
                                          kind == Kind::review,
                                          std::move(expected_revision));
    });
    if (id.empty())
      return {};
    return QString::fromStdString(id);
  } catch (...) {
    return {};
  }
}

QString PermissionControl::apply(const QString &review_operation_id,
                                 const QString &choices_json) noexcept {
  return applyForIngress(review_operation_id, choices_json,
                         Ingress::trusted_ui);
}

QString PermissionControl::applyForIngress(const QString &review_operation_id,
                                           const QString &choices_json,
                                           Ingress ingress) noexcept {
  try {
    prune();
    auto *source = operations_.find(review_operation_id);
    if (!source || source->kind != Kind::review ||
        source->state != State::succeeded || source->consumed ||
        !source->context || !source->review || source->ingress != ingress ||
        choices_json.toUtf8().size() > kMaximumChoicesBytes)
      return {};

    QJsonParseError parse_error{};
    const auto document =
        QJsonDocument::fromJson(choices_json.toUtf8(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject())
      return {};
    const auto root = document.object();
    const auto acknowledgement = root.value(QStringLiteral("hostExtensionAcknowledged"));
    if (root.size() != (acknowledgement.isUndefined() ? 1 : 2) ||
        (!acknowledgement.isUndefined() && !acknowledgement.isBool()) ||
        !root.value(QStringLiteral("choices")).isArray())
      return {};
    const auto choices = root.value(QStringLiteral("choices")).toArray();
    if (choices.size() != static_cast<qsizetype>(source->rows.size()))
      return {};

    std::vector<host_session::DynamicConsentDecision> dynamic;
    dynamic.reserve(source->review->dynamic_rows.size());
    std::vector<bool> seen(source->rows.size(), false);
    for (const auto value : choices) {
      if (!value.isObject())
        return {};
      const auto choice = value.toObject();
      if (choice.size() < 2 || choice.size() > 3 ||
          !choice.value(QStringLiteral("rowId")).isString() ||
          !choice.value(QStringLiteral("decision")).isString())
        return {};
      const auto row_id = choice.value(QStringLiteral("rowId")).toString();
      const auto found =
          std::ranges::find(source->rows, row_id.toStdString(), &Row::id);
      if (found == source->rows.end())
        return {};
      const auto position =
          static_cast<std::size_t>(std::distance(source->rows.begin(), found));
      if (seen[position])
        return {};
      seen[position] = true;
      const auto decision_text =
          choice.value(QStringLiteral("decision")).toString();
      const bool grant = decision_text == QStringLiteral("grant");
      if (!grant && decision_text != QStringLiteral("deny"))
        return {};
      if ((grant && !found->available) || (!grant && found->required))
        return {};
      const auto decision = grant ? permissions::UserDecision::grant
                                  : permissions::UserDecision::deny;
        const auto &row = source->review->dynamic_rows[found->index];
        if (!row.requested)
          return {};
        auto operations = row.requested->operations;
        if (grant) {
          if (choice.size() != 3 ||
              !choice.value(QStringLiteral("operations")).isArray())
            return {};
          const auto selected =
              choice.value(QStringLiteral("operations")).toArray();
          const auto narrowed = selectDynamicOperations(*found, selected);
          if (!narrowed)
            return {};
          operations = *narrowed;
        } else if (choice.size() != 2) {
          return {};
        }
        dynamic.push_back({.definition = row.requested->definition,
                           .operations = std::move(operations),
                           .decided_scope = row.requested->scope,
                           .decision = decision});
    }
    if (!std::ranges::all_of(seen, [](bool value) { return value; }))
      return {};

    const auto context = *source->context;
    const auto review = source->review;
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    host_session::ConsentConfirmation confirmation{
        .review_fingerprint = review->fingerprint,
        .decision_fingerprint = host_session::consent_decision_fingerprint(
            *review, dynamic),
        .actor = ingress == Ingress::trusted_ui
                     ? permissions::DecisionActor::trusted_ui
                     : permissions::DecisionActor::interactive_cli,
        .confirmed_wall_seconds =
            seconds > 0 ? static_cast<std::uint64_t>(seconds) : 1,
        .host_extension_acknowledgement = acknowledgement.toBool(false)
            ? std::optional{review->fingerprint} : std::nullopt};
    const auto id = start(Kind::apply, ingress, [&](auto serial) {
      return manager_.beginPermissionApply(serial, context, review, confirmation, dynamic);
    });
    if (id.empty())
      return {};
    source = operations_.find(review_operation_id);
    if (source)
      source->consumed = true;
    return QString::fromStdString(id);
  } catch (...) {
    return {};
  }
}

QString PermissionControl::revoke(const QString &source_operation_id,
                                  const QString &row_id) noexcept {
  try {
    prune();
    auto *source = operations_.find(source_operation_id);
    if (!source ||
        (source->kind != Kind::list && source->kind != Kind::review) ||
        source->state != State::succeeded || source->consumed ||
        !source->context)
      return {};
    const auto found =
        std::ranges::find(source->rows, row_id.toStdString(), &Row::id);
    if (found == source->rows.end())
      return {};
    const auto exact_row = *found;
    const auto context = *source->context;
    const auto id = start(Kind::revoke, source->ingress, [&](auto serial) {
      return manager_.beginPermissionRevoke(serial, context, exact_row);
    });
    if (id.empty())
      return {};
    source = operations_.find(source_operation_id);
    if (source)
      source->consumed = true;
    return QString::fromStdString(id);
  } catch (...) {
    return {};
  }
}

QString PermissionControl::poll(const QString &operation_id) noexcept {
  try {
    prune();
    auto *operation = operations_.find(operation_id);
    return operation
        ? operation->poll(operation_id, {{QStringLiteral("kind"), kind_name(operation->kind)}})
        : QString{};
  } catch (...) {
    return {};
  }
}

std::optional<permissions::FixedSet<definitions::Name, 16>>
PermissionControl::selectDynamicOperations(
    const Row &row, const QJsonArray &selected) noexcept {
  try {
    if (selected.isEmpty() ||
        selected.size() > static_cast<qsizetype>(row.operations.size()))
      return std::nullopt;
    permissions::FixedSet<definitions::Name, 16> operations;
    for (const auto selected_value : selected) {
      if (!selected_value.isString())
        return std::nullopt;
      const auto selected_id = selected_value.toString().toStdString();
      const auto exact = std::ranges::find(row.operations, selected_id,
                                           &Row::DynamicOperation::id);
      if (exact == row.operations.end() || !operations.insert(exact->name))
        return std::nullopt;
    }
    return operations;
  } catch (...) {
    return std::nullopt;
  }
}

#ifdef OMARCHY_PLUGIN_MANAGER_TESTING
void PermissionControlTestAccess::ageOperation(PermissionControl &control,
                                               const QString &operation_id,
                                               std::chrono::minutes age) {
  if (auto *operation = control.operations_.find(operation_id))
    operation->touched = std::chrono::steady_clock::now() - age;
}
#endif

void PermissionControl::prune() noexcept {
  operations_.prune([](const Operation &operation) {
    return operation.state == State::pending ? kPendingLifetime
        : operation.kind == Kind::review && !operation.consumed
            ? kReviewLifetime : kCompletedLifetime;
  });
}

void PermissionControl::fail(std::uint64_t serial, std::string error) noexcept {
  if (auto *operation = operations_.find(serial)) {
    operation->state = State::failed;
    operation->error = std::move(error);
    operation->touched = std::chrono::steady_clock::now();
  }
}

void PermissionControl::completeRead(
    std::uint64_t serial, std::string plugin, std::uint64_t slot_epoch,
    std::shared_ptr<channel::PluginPermissionAuthority> authority,
    std::optional<host_session::AuthorityView> view,
    std::shared_ptr<const host_session::ConsentReview> review) noexcept {
  try {
    auto *operation = operations_.find(serial);
    if (!operation || !authority || !view ||
        (operation->kind == Kind::review && !review)) {
      fail(serial, "read-failed");
      return;
    }
    operation->context =
        ExactContext{.plugin = std::move(plugin),
                     .slot_epoch = slot_epoch,
                     .authority_sequence = view->authority_slots.sequence,
                     .authority = std::move(authority)};
    operation->review = std::move(review);
    const auto append_row = [&](std::size_t index, const auto &request) -> Row & {
      const auto id = opaque_id("row");
      const bool available = operation->context->authority->provider_available(
          request.definition);
      operation->rows.push_back({.id = id, .index = index,
                                 .required = request.required,
                                 .available = available,
                                 .definition = request.definition,
                                 .operations = {}});
      return operation->rows.back();
    };
    QJsonArray rows;
    if (operation->kind == Kind::list) {
      if (view->active) {
        const auto &active = *view->active;
        for (std::size_t index = 0; index < active.dynamic_grants.size();
             ++index) {
          const auto &revision = active.dynamic_grants[index];
          const auto &row = append_row(index, revision.request);
          const auto resolved = operation->context->authority->definitions_->resolve(
              revision.request.definition);
          rows.push_back(dynamic_row(row.id, revision.request, revision.grant,
                                      resolved ? std::optional{*resolved->definition}
                                               : std::nullopt, row.available,
                                      dynamic_operations(revision.grant.operations)));
        }
      }
    } else {
      for (std::size_t index = 0;
           index < operation->review->dynamic_rows.size(); ++index) {
        const auto &review_row = operation->review->dynamic_rows[index];
        if (!review_row.requested)
          continue;
        auto &exact_row = append_row(index, *review_row.requested);
        QJsonArray selectable_operations;
        for (const auto &name : review_row.requested->operations.values()) {
          const auto operation_id = opaque_id("operation");
          exact_row.operations.push_back({.id = operation_id, .name = name});
          QString label = text(name.view());
          if (review_row.trusted_definition) {
            const auto definition_operation = std::ranges::find(
                review_row.trusted_definition->operations.values(), name,
                &definitions::OperationDefinition::name);
            if (definition_operation !=
                review_row.trusted_definition->operations.values().end())
              label = text(definition_operation->label.view());
          }
          selectable_operations.push_back(
              QJsonObject{{QStringLiteral("operationId"), text(operation_id)},
                          {QStringLiteral("name"), text(name.view())},
                          {QStringLiteral("label"), label}});
        }
        auto presentation = dynamic_row(
            exact_row.id, *review_row.requested, review_row.previous_grant,
            review_row.trusted_definition, exact_row.available, selectable_operations);
        presentation.insert(QStringLiteral("delta"),
                            delta_name(review_row.delta));
        presentation.insert(QStringLiteral("reason"),
                            text(review_row.publisher_reason));
        rows.push_back(std::move(presentation));
      }
    }
    QJsonObject result{
        {QStringLiteral("plugin"), text(operation->context->plugin)},
        {QStringLiteral("permissions"), rows}};
    if (operation->review && !operation->review->verified.manifest.aur_dependencies.empty()) {
      QJsonArray packages;
      for (const auto &name : operation->review->verified.manifest.aur_dependencies)
        packages.push_back(text(name));
      result.insert(QStringLiteral("dependencies"),
                    QJsonObject{{QStringLiteral("aur"), packages}});
    }
    operation->result_json =
        QJsonDocument(result).toJson(QJsonDocument::Compact).toStdString();
    operation->state = State::succeeded;
    operation->touched = std::chrono::steady_clock::now();
  } catch (...) {
    fail(serial, "read-failed");
  }
}

void PermissionControl::completeMutation(std::uint64_t serial, bool applied,
                                         std::string error) noexcept {
  auto *operation = operations_.find(serial);
  if (!operation)
    return;
  operation->state = applied ? State::succeeded : State::failed;
  operation->error = applied ? std::string{} : std::move(error);
  operation->result_json = applied ? "{\"applied\":true}" : std::string{};
  operation->touched = std::chrono::steady_clock::now();
}

void PermissionControl::invalidatePlugin(std::string_view plugin) noexcept {
  std::erase_if(operations_.values, [&](const Operation &operation) {
    return operation.context && operation.context->plugin == plugin;
  });
}

} // namespace omarchy::plugin_runtime::bridge
