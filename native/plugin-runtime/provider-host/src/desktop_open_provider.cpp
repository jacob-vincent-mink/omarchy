#include "provider_protocol.hpp"
#include "system_executable.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QUrl>

#include <pwd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace protocol = omarchy::plugin_runtime::provider_protocol;

constexpr qsizetype kMaximumUrlBytes = 2048;
constexpr qsizetype kMaximumOrigins = 32;
struct Request final {
  std::uint64_t correlation = 0;
  QString demand_scope;
  QString url;
  QString presentation;
};

std::optional<Request> decode(std::span<const std::byte> frame) {
  const auto decoded = protocol::decode(frame);
  if (!decoded || decoded->adapter != "desktop-open-uri" || decoded->operation != "open" ||
      decoded->contract != OMARCHY_DESKTOP_OPEN_CONTRACT_DIGEST)
    return std::nullopt;
  const auto correlation = decoded->correlation;
  const auto scope = decoded->scope;
  const auto &payload = decoded->payload;
  if (!protocol::exact_keys(payload, {u"url", u"presentation"}) ||
      !payload.value("url").isString() ||
      !payload.value("presentation").isString())
    return std::nullopt;
  return Request{.correlation = correlation,
                 .demand_scope = QString::fromUtf8(scope),
                 .url = payload.value("url").toString(),
                 .presentation = payload.value("presentation").toString()};
}

std::optional<QString> origin(const QUrl &url, bool origin_only) {
  if (!url.isValid() || url.isRelative() ||
      url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0 ||
      url.host().isEmpty() || !url.userName().isEmpty() ||
      !url.password().isEmpty() || (url.port(-1) != -1 && url.port() != 443) ||
      (origin_only && (!url.path().isEmpty() || url.hasQuery() || url.hasFragment())))
    return std::nullopt;
  const auto ace = QUrl::toAce(url.host()).toLower();
  if (ace.isEmpty())
    return std::nullopt;
  return QStringLiteral("https://") + QString::fromLatin1(ace);
}

std::optional<QByteArray> authorized_url(const Request &request) {
  const auto raw = request.url.toUtf8();
  if (raw.isEmpty() || raw.size() > kMaximumUrlBytes ||
      raw.contains('\0') || raw.contains('\r') || raw.contains('\n'))
    return std::nullopt;
  QJsonParseError error{};
  const auto document = QJsonDocument::fromJson(request.demand_scope.toUtf8(),
                                                 &error);
  if (error.error != QJsonParseError::NoError || !document.isObject())
    return std::nullopt;
  const auto scope = document.object();
  if (!protocol::exact_keys(scope, {u"origins", u"userGesture"}) ||
      scope.value("userGesture") != true || !scope.value("origins").isArray())
    return std::nullopt;
  const auto requested = QUrl::fromEncoded(raw, QUrl::StrictMode);
  const auto requested_origin = origin(requested, false);
  if (!requested_origin)
    return std::nullopt;
  const auto origins = scope.value("origins").toArray();
  if (origins.isEmpty() || origins.size() > kMaximumOrigins)
    return std::nullopt;
  bool allowed = false;
  for (const auto &value : origins) {
    if (!value.isString())
      return std::nullopt;
    const auto candidate = origin(QUrl(value.toString(), QUrl::StrictMode), true);
    if (!candidate)
      return std::nullopt;
    allowed = allowed || *candidate == *requested_origin;
  }
  if (!allowed)
    return std::nullopt;
  return requested.toEncoded(QUrl::FullyEncoded);
}

using omarchy::plugin_runtime::provider_host::detail::secure_system_executable;

#ifndef OMARCHY_DESKTOP_OPEN_TESTING
QString account_home() {
  const auto *account = ::getpwuid(::getuid());
  if (!account || !account->pw_dir)
    return QStringLiteral("/nonexistent");
  const QString result = QString::fromLocal8Bit(account->pw_dir);
  return result.startsWith('/') && !result.contains(QChar::Null)
             ? result
             : QStringLiteral("/nonexistent");
}
#endif

bool launch(const QByteArray &url, const QString &presentation,
            std::uint64_t correlation) {
  constexpr auto systemd_run = "/usr/bin/systemd-run";
  constexpr auto xdg_open = "/usr/bin/xdg-open";
  constexpr auto chromium = "/usr/bin/chromium";
  if (!secure_system_executable(systemd_run) ||
      (presentation == QStringLiteral("browser-tab") &&
       !secure_system_executable(xdg_open)) ||
      (presentation == QStringLiteral("web-app-window") &&
       !secure_system_executable(chromium)))
    return false;
#ifdef OMARCHY_DESKTOP_OPEN_TESTING
  (void)url;
  (void)correlation;
  return true;
#else
  const auto uid = ::getuid();
  const QString runtime_dir = QStringLiteral("/run/user/") + QString::number(uid);
  QProcess process;
  QProcessEnvironment environment;
  environment.insert(QStringLiteral("PATH"), QStringLiteral("/usr/bin"));
  environment.insert(QStringLiteral("LANG"), QStringLiteral("C.UTF-8"));
  environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C.UTF-8"));
  environment.insert(QStringLiteral("HOME"), account_home());
  environment.insert(QStringLiteral("XDG_RUNTIME_DIR"), runtime_dir);
  environment.insert(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"),
                     QStringLiteral("unix:path=") + runtime_dir +
                         QStringLiteral("/bus"));
  process.setProcessEnvironment(environment);
  process.setProgram(QString::fromLatin1(systemd_run));
  QStringList arguments{
      QStringLiteral("--user"), QStringLiteral("--quiet"),
      QStringLiteral("--collect"),
      QStringLiteral("--unit=omarchy-plugin-open-") +
          QString::number(::getpid()) + QStringLiteral("-") +
          QString::number(correlation),
      QStringLiteral("--property=Type=exec"),
      QStringLiteral("--property=StandardOutput=null"),
      QStringLiteral("--property=StandardError=null"), QStringLiteral("--")};
  if (presentation == QStringLiteral("browser-tab")) {
    arguments << QString::fromLatin1(xdg_open) << QString::fromUtf8(url);
  } else {
    arguments << QString::fromLatin1(chromium)
              << QStringLiteral("--app=") + QString::fromUtf8(url);
  }
  process.setArguments(arguments);
  process.start(QIODevice::ReadOnly);
  if (!process.waitForStarted(1000))
    return false;
  if (!process.waitForFinished(2500)) {
    process.kill();
    process.waitForFinished(500);
    return false;
  }
  return process.exitStatus() == QProcess::NormalExit &&
         process.exitCode() == 0;
#endif
}

bool send_response(std::uint64_t correlation, bool success, std::string_view error) {
  return protocol::send_response(correlation, QJsonDocument(QJsonObject{
      {"ok", success}, {"error", QString::fromUtf8(error)}}).toJson(QJsonDocument::Compact));
}
} // namespace

int main() {
  return protocol::serve(decode, [&](const auto &request) {
    const auto url = authorized_url(request);
    if (!url || (request.presentation != QStringLiteral("browser-tab") &&
                 request.presentation != QStringLiteral("web-app-window"))) {
      return send_response(request.correlation, false, "url-rejected");
    }
    const bool opened =
        launch(*url, request.presentation, request.correlation);
    return send_response(request.correlation, opened,
                         opened ? "" : "desktop-open-failed");
  });
}
