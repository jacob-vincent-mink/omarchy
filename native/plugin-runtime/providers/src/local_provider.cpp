#include "omarchy/plugin_runtime/providers/local_provider.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QStringDecoder>

#include <algorithm>
#include <array>
#include <stdexcept>

namespace omarchy::plugin_runtime::providers {
namespace definitions = plugins::definitions;
namespace {

QJsonObject object(std::string_view bytes) {
  if (bytes.size() > definitions::kMaximumDynamicPayloadBytes)
    throw std::invalid_argument("local provider input exceeds limit");
  const auto document = QJsonDocument::fromJson(QByteArray(bytes.data(), bytes.size()));
  if (!document.isObject() || document.toJson(QJsonDocument::Compact).toStdString() != bytes)
    throw std::invalid_argument("local provider input is not a canonical JSON object");
  return document.object();
}

QByteArray text(const QJsonValue &value, qsizetype limit, bool newline = false) {
  const auto string = value.toString();
  const auto bytes = string.toUtf8();
  if (!value.isString() || bytes.isEmpty() || bytes.size() > limit ||
      std::ranges::any_of(string, [=](QChar ch) {
        return (ch.unicode() < 0x20 && !(newline && ch.unicode() == '\n')) || ch.unicode() == 0x7f;
      }))
    throw std::invalid_argument("invalid local provider text");
  return bytes;
}

} // namespace

LocalProvider::LocalProvider(const definitions::DynamicRevisionGrant &grant,
                             definitions::EnforcementFamily family,
                             int state_directory, NotificationSend send, void *context,
                             std::uint64_t maximum_total, std::uint64_t maximum_item)
    : grant_(grant), scope_(object(grant.request.scope.view())), send_(send), context_(context) {
  if (family == definitions::EnforcementFamily::private_storage) {
    const auto total = scope_.value("quotaBytes").toInteger(-1);
    const auto item = scope_.value("itemBytes").toInteger(-1);
    if (scope_.size() != 2 || item <= 0 || total < item || static_cast<std::uint64_t>(total) > maximum_total ||
        static_cast<std::uint64_t>(item) > maximum_item)
      throw std::invalid_argument("invalid private storage quota");
    storage_ = std::make_unique<PrivateStorageBackend>(state_directory, total, item);
    if (!storage_->valid()) throw std::invalid_argument("private storage is unavailable");
  } else if (family == definitions::EnforcementFamily::notifications) {
    const auto categories = scope_.value("categories").toArray();
    if (scope_.size() != 1 || categories.isEmpty() || categories.size() > 64)
      throw std::invalid_argument("invalid notification categories");
    for (qsizetype i = 0; i < categories.size(); ++i) {
      (void)text(categories[i], 96);
      for (qsizetype j = 0; j < i; ++j)
        if (categories[j] == categories[i]) throw std::invalid_argument("duplicate category");
    }
  } else {
    throw std::invalid_argument("not a local provider family");
  }
}

bool LocalProvider::dispatch(const definitions::AuthorizedDynamicRequest &request,
                            std::span<std::byte> response, std::size_t &written) noexcept {
  written = 0;
  if (response.size() < 2) return false;
  try {
    if (request.authorization.binding != grant_.binding ||
        request.authorization.definition != grant_.request.definition ||
        request.authorization.grant_epoch != grant_.grant.epoch ||
        grant_.grant.state != plugins::permissions::GrantState::granted ||
        !grant_.grant.operations.contains(definitions::Name(request.operation)) ||
        request.demand_scope != grant_.request.scope.view())
      return false;
    const auto payload = object({reinterpret_cast<const char *>(request.payload.data()),
                                 request.payload.size()});
    QJsonObject result;
    if (storage_) {
      const auto key = text(payload.value("key"), kMaximumStorageKeyBytes);
      if (!valid_storage_key(key.toStdString())) return false;
      if (request.operation == "write") {
        if (payload.size() != 2 || !payload.value("value").isString()) return false;
        const auto value = payload.value("value").toString().toUtf8();
        if (static_cast<std::size_t>(value.size()) > kMaximumStorageValueBytes ||
            !storage_->write(key.toStdString(), std::as_bytes(std::span(value))))
          return false;
      } else {
        if (payload.size() != 1) return false;
        if (request.operation == "remove") {
          if (!storage_->remove(key.toStdString())) return false;
        } else if (request.operation == "read") {
          std::array<std::byte, kMaximumStorageValueBytes> bytes{};
          std::size_t size = 0;
          bool found = false;
          if (!storage_->read(key.toStdString(), bytes, size, found)) return false;
          QStringDecoder decoder(QStringDecoder::Utf8);
          const QString value = decoder(QByteArrayView(reinterpret_cast<const char *>(bytes.data()), size));
          if (decoder.hasError()) return false;
          result = {{"found", found}, {"value", value}};
        } else {
          return false;
        }
      }
    } else {
      if (request.operation != "send" || payload.size() != 3 || !send_) return false;
      const auto category = text(payload.value("category"), 96);
      const auto title = text(payload.value("title"), 96);
      const auto body = text(payload.value("body"), 512, true);
      if (!scope_.value("categories").toArray().contains(payload.value("category")) ||
          !send_(grant_.binding.plugin.view(), category.toStdString(),
                      title.toStdString(), body.toStdString(), context_))
        return false;
    }
    const auto bytes = QJsonDocument(result).toJson(QJsonDocument::Compact);
    if (static_cast<std::size_t>(bytes.size()) > response.size()) return false;
    std::ranges::copy(std::as_bytes(std::span(bytes)), response.begin());
    written = bytes.size();
    return true;
  } catch (...) {
    return false;
  }
}

} // namespace omarchy::plugin_runtime::providers
