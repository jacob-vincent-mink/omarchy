#include "provider_fixture.hpp"
#include <QJsonArray>
#include <iostream>

namespace {
using namespace provider_test;

std::vector<std::byte> request(std::uint64_t correlation,
                               std::string_view dataset,
                               std::string_view output = {},
                               std::string_view scope =
                                   R"({"datasets":["packages.summary","compositor.window-rectangles"]})") {
  QJsonObject payload{{"dataset", QString::fromUtf8(dataset)}};
  if (!output.empty()) payload.insert("output", QString::fromUtf8(output));
  return request_frame(correlation, "sanitized-system-observe", SYSTEM_OBSERVE_CONTRACT_DIGEST,
                       "observe", scope, QJsonDocument(payload).toJson(QJsonDocument::Compact));
}

Child start() { return start_provider({SYSTEM_OBSERVER_PATH}); }
} // namespace

int main() {
  reject_descriptors(start(), request(1, "packages.summary"));
  const auto child = start();
  auto response = roundtrip(child.channel, request(1, "packages.summary"));
  OMARCHY_CHECK(response.value("ok").toBool() &&
              response.value("pendingUpdates").toInt() == 2 &&
              response.value("orphanCount").toInt() == 1);
  response = roundtrip(child.channel,
                       request(2, "compositor.window-rectangles", "DP-1"));
  OMARCHY_CHECK(response.value("ok").toBool() &&
              response.value("output").toString() == "DP-1" &&
              response.value("reservedBottom").toInt() == 32 &&
              response.value("windows").toArray().size() == 1);
  const auto encoded = QJsonDocument(response).toJson(QJsonDocument::Compact);
  OMARCHY_CHECK(!encoded.contains("0xsecret") && !encoded.contains("must-not-leak"));
  response = roundtrip(
      child.channel,
      request(3, "packages.summary", {},
              R"({"datasets":["compositor.window-rectangles"]})"));
  OMARCHY_CHECK(!response.value("ok").toBool());
  OMARCHY_CHECK(finish(child) == 0);
  std::cout << "system observer tests passed\n";
}
