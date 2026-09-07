#include "provider_fixture.hpp"
#include "../src/provider_protocol.hpp"
#include <iostream>

namespace {
using namespace provider_test;

void protocol_contract() {
  namespace protocol = omarchy::plugin_runtime::provider_protocol;
  const std::array<QString, 5> keys{QStringLiteral("a"), QStringLiteral("b"),
                                  QStringLiteral("extra"), QStringLiteral("\u00e9"),
                                  QString::fromUtf8("a\0", 2)};
  for (unsigned mask = 0; mask < (1U << keys.size()); ++mask) {
    QJsonObject object;
    for (unsigned index = 0; index < keys.size(); ++index)
      if (mask & (1U << index))
        object.insert(keys[index], QJsonValue::Null);
    OMARCHY_CHECK(protocol::exact_keys(object, {u"a"}, {u"b"}) ==
                  ((mask & 1U) && !(mask & ~3U)));
    OMARCHY_CHECK(protocol::exact_keys(object, {}, {u"a", u"b"}) == !(mask & ~3U));
    OMARCHY_CHECK(protocol::exact_keys(object, {u"a", u"b"}) == (mask == 3U));
  }
  const std::string digest(64, 'd');
  const auto valid = request_frame(1, "adapter", digest, "read", "{}", "{}");
  const auto decoded = protocol::decode(valid);
  OMARCHY_CHECK(decoded && decoded->correlation == 1 && decoded->adapter == "adapter" &&
              decoded->contract == digest && decoded->operation == "read" &&
              decoded->scope == "{}" && decoded->payload.isEmpty());
  for (std::size_t size = 0; size < valid.size(); ++size)
    OMARCHY_CHECK(!protocol::decode(std::span(valid).first(size)));
  for (const auto offset : {0, 4, 5, 6, 7, 15, 19, 20, 21, 22 + 7 + 2 + 64 + 3}) {
    auto changed = valid;
    changed[offset] ^= std::byte{1};
    OMARCHY_CHECK(!protocol::decode(changed));
  }
  auto trailing = valid;
  trailing.push_back(std::byte{0});
  OMARCHY_CHECK(!protocol::decode(trailing));
  OMARCHY_CHECK(protocol::decode(valid, 2).has_value() && !protocol::decode(valid, 1));
  for (const auto &scope : {std::string{}, std::string("a\0b", 3), std::string(4097, 'x')})
    OMARCHY_CHECK(!protocol::decode(request_frame(1, "adapter", digest, "read", scope, "{}")));
  OMARCHY_CHECK(protocol::decode(request_frame(1, "adapter", digest, "read",
                                         std::string(4096, 'x'), "{}")).has_value());
  for (const auto &payload : {QByteArray{}, QByteArray("[]"), QByteArray("null"), QByteArray("{broken}")})
    OMARCHY_CHECK(!protocol::decode(request_frame(1, "adapter", digest, "read", "{}", payload)));
  const auto reply = protocol::response(1, "{}");
  const std::vector<std::byte> expected{
      std::byte{0x4f}, std::byte{0x50}, std::byte{0x52}, std::byte{0x56},
      std::byte{1}, std::byte{2}, std::byte{0}, std::byte{0},
      std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
      std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1},
      std::byte{0}, std::byte{0}, std::byte{0}, std::byte{3},
      std::byte{0}, std::byte{'{'}, std::byte{'}'}};
  OMARCHY_CHECK(reply == expected && protocol::response(0, "{}").empty() &&
              protocol::response(1, QByteArray(65535, 'x')).size() == 65556 &&
              protocol::response(1, QByteArray(65536, 'x')).empty());
}

std::vector<std::byte> request(std::uint64_t correlation,
                               std::string_view url,
                               std::string_view presentation,
                               std::string_view scope =
                                   R"({"origins":["https://github.com"],"userGesture":true})") {
  const auto payload = QJsonDocument(QJsonObject{
      {"url", QString::fromUtf8(url)}, {"presentation", QString::fromUtf8(presentation)}})
                           .toJson(QJsonDocument::Compact);
  return request_frame(correlation, "desktop-open-uri", DESKTOP_OPEN_CONTRACT_DIGEST,
                       "open", scope, payload);
}

Child start() { return start_provider({DESKTOP_OPENER_PATH}); }
} // namespace

int main() {
  protocol_contract();
  reject_descriptors(start(), request(1, "https://github.com", "browser-tab"));
  auto child = start();
  auto response = roundtrip(
      child.channel,
      request(1, "https://github.com/org/repo/pull/7?tab=checks#summary",
              "browser-tab"));
  OMARCHY_CHECK(response.value("ok").toBool());
  response = roundtrip(
      child.channel,
      request(2, "https://github.com:443/notifications", "web-app-window"));
  OMARCHY_CHECK(response.value("ok").toBool());
  const std::array rejected{
      std::pair{"http://github.com/org/repo", "browser-tab"},
      std::pair{"https://github.com.evil.example/org/repo", "browser-tab"},
      std::pair{"https://user@github.com/org/repo", "browser-tab"},
      std::pair{"https://github.com:444/org/repo", "browser-tab"},
      std::pair{"https://evil.example/org/repo", "browser-tab"},
      std::pair{"https://github.com/org/repo", "native-dialog"}};
  std::uint64_t correlation = 3;
  for (const auto &[url, presentation] : rejected) {
    response = roundtrip(child.channel,
                         request(correlation++, url, presentation));
    OMARCHY_CHECK(!response.value("ok").toBool() &&
                response.value("error").toString() == "url-rejected");
  }
  response = roundtrip(
      child.channel,
      request(correlation++, "https://github.com/org/repo", "browser-tab",
              R"({"origins":["https://evil.example"],"userGesture":true})"));
  OMARCHY_CHECK(!response.value("ok").toBool());
  response = roundtrip(
      child.channel,
      request(correlation++, "https://github.com/org/repo", "browser-tab",
              R"({"origins":["https://github.com"],"userGesture":false})"));
  OMARCHY_CHECK(!response.value("ok").toBool());
  response = roundtrip(
      child.channel,
      request(correlation++,
              "https://github.com/" + std::string(2048, 'a'), "browser-tab"));
  OMARCHY_CHECK(!response.value("ok").toBool());
  OMARCHY_CHECK(finish(child) == 0);
  std::cout << "desktop opener tests passed\n";
}
