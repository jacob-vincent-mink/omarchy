#include "provider_fixture.hpp"
#include <QJsonArray>
#include <QFile>
#include <iostream>

namespace {
using namespace provider_test;

std::vector<std::byte> request(std::uint64_t correlation,
                               std::string_view scope,
                               std::string_view command,
                               std::span<const std::string_view> arguments) {
  QJsonArray json_arguments;
  for (const auto argument : arguments)
    json_arguments.append(QString::fromUtf8(argument));
  const auto payload = QJsonDocument(QJsonObject{
      {"command", QString::fromUtf8(command)}, {"arguments", json_arguments}})
                           .toJson(QJsonDocument::Compact);
  return request_frame(correlation, "bounded-command-execute",
      COMMAND_CONTRACT_DIGEST,
      "run", scope, payload);
}

std::string scope_fixture(const char *name) {
  QFile file(QStringLiteral(COMMAND_SCOPE_ROOT "/") + QString::fromLatin1(name));
  OMARCHY_CHECK(file.open(QIODevice::ReadOnly));
  return QJsonDocument::fromJson(file.readAll()).toJson(QJsonDocument::Compact).toStdString();
}

Child start(bool validate_only = false) {
  std::vector<std::string> arguments{COMMAND_EXECUTOR_PATH};
  if (validate_only) arguments.push_back("--validate-only");
  return start_provider(std::move(arguments));
}

void manifest_scope_contract() {
  auto child = start(true);
  const auto base = QJsonDocument::fromJson(QByteArray::fromStdString(
      scope_fixture("test-command-scope.json"))).object().value("commands").toArray()[0].toObject();
  const std::array<std::string_view, 2> arguments{"%s", "hello"};
  std::uint64_t correlation = 1;
  const auto check = [&](const QJsonObject &scope, bool accepted) {
    const auto result = roundtrip(child.channel, request(correlation++,
        QJsonDocument(scope).toJson(QJsonDocument::Compact).toStdString(), "printf", arguments));
    OMARCHY_CHECK(result.value("exitCode").toInt(-1) == (accepted ? 0 : 126));
  };
  const auto command = [&](const QJsonObject &policy, bool accepted) {
    check(QJsonObject{{"commands", QJsonArray{policy}}}, accepted);
  };
  command(base, true);
  for (const auto &bad_scope : {
      QJsonObject{{"profile", "github-api-v1"}}, QJsonObject{},
      QJsonObject{{"commands", QJsonArray{}}},
      QJsonObject{{"commands", QJsonArray{base, base}}},
      QJsonObject{{"commands", QJsonArray{base, 1}}},
      QJsonObject{{"commands", QJsonArray{base}}, {"profile", "override"}}})
    check(bad_scope, false);
  for (const auto &[key, value] : std::vector<std::pair<QString, QJsonValue>>{
      {"unknown", true}, {"rules", QJsonArray{}}, {"rules", QJsonArray{1}},
      {"rules", QJsonArray{QJsonArray{QJsonObject{{"regex", "["}}}}},
      {"rules", QJsonArray{QJsonArray{QJsonObject{{"exact", "%s"}, {"regex", ".*"}}}}},
      {"executable", "/usr/bin/../bin/printf"}, {"executable", "printf"},
      {"command", "../printf"}, {"accountHome", "yes"},
      {"timeoutMs", 0}, {"timeoutMs", 29001}, {"timeoutMs", 1.5},
      {"stdoutBytes", 49153}, {"stderrBytes", 8193},
      {"environment", QJsonObject{{"HOME", "/tmp"}}},
      {"environment", QJsonObject{{"LD_PRELOAD", "/tmp/evil.so"}}},
      {"environment", QJsonObject{{"QT_PLUGIN_PATH", "/tmp"}}}}) {
    auto changed = base;
    changed.insert(key, value);
    command(changed, false);
  }
  for (const auto &key : base.keys()) {
    auto missing = base;
    missing.remove(key);
    command(missing, false);
  }
  // Interpreter access is governed by the approved executable and argv, not
  // a hidden administrator policy or a command-name blacklist.
  auto interpreter = base;
  interpreter.insert("command", "sh");
  interpreter.insert("executable", "/usr/bin/sh");
  interpreter.insert("rules", QJsonArray{QJsonArray{
      QJsonObject{{"exact", "-c"}}, QJsonObject{{"exact", "printf approved"}}}});
  const std::array<std::string_view, 2> shell_arguments{"-c", "printf approved"};
  const auto shell_scope = QJsonDocument(QJsonObject{{"commands", QJsonArray{interpreter}}})
      .toJson(QJsonDocument::Compact).toStdString();
  OMARCHY_CHECK(roundtrip(child.channel, request(correlation++, shell_scope, "sh", shell_arguments))
                   .value("exitCode").toInt(-1) == 0);
  auto changed_rules = base;
  changed_rules.insert("rules", QJsonArray{QJsonArray{QJsonObject{{"exact", "different"}}}});
  command(changed_rules, false);
  command(base, true); // No scope or denial cached across requests.
  QJsonArray commands;
  auto bounded = base;
  bounded.insert("rules", QJsonArray{QJsonArray{}});
  bounded.insert("timeoutMs", 1);
  bounded.insert("stdoutBytes", 1);
  bounded.insert("stderrBytes", 1);
  for (int count = 1; count <= 17; ++count) {
    bounded.insert("command", QStringLiteral("c%1").arg(count));
    commands.append(bounded);
    const auto scope = QJsonDocument(QJsonObject{{"commands", commands}})
        .toJson(QJsonDocument::Compact).toStdString();
    OMARCHY_CHECK(scope.size() <= 4096);
    OMARCHY_CHECK(roundtrip(child.channel, request(correlation++, scope, "c1", {}))
                     .value("exitCode").toInt(-1) == (count <= 16 ? 0 : 126));
  }
  OMARCHY_CHECK(finish(child) == 0);

  for (const auto &frame : {
      request_frame(1, "bounded-command-execute", COMMAND_CONTRACT_DIGEST, "run", "{}",
                    R"({"command":"printf","arguments":[],"commands":[]})"),
      request_frame(1, "bounded-command-execute",
                    "7cfb8547d49ea0d43248227c049f19d9f711c7838452789c9ef2fdfe41e82142",
                    "run", "{}", R"({"command":"printf","arguments":[]})")}) {
    child = start();
    OMARCHY_CHECK(::send(child.channel, frame.data(), frame.size(), MSG_NOSIGNAL) ==
                 static_cast<ssize_t>(frame.size()));
    OMARCHY_CHECK(finish(child) == 2);
  }
}
} // namespace

int main() {
  manifest_scope_contract();
  const auto test_scope = scope_fixture("test-command-scope.json");
  const auto github_scope = scope_fixture("github-command-scope.json");
  reject_descriptors(start(), request(1, test_scope, "printf", {}));
  auto child = start();
  const std::array<std::string_view, 2> print_arguments{"%s", "hello"};
  auto response = roundtrip(
      child.channel,
      request(1, test_scope, "printf", print_arguments));
  OMARCHY_CHECK(response.value("exitCode").toInt(-1) == 0 &&
              response.value("stdout").toString() == "hello" &&
              response.value("stderr").toString().isEmpty());

  const std::array<std::string_view, 2> rejected_arguments{"%s", "@secret"};
  response = roundtrip(child.channel,
                      request(2, test_scope, "printf",
                              rejected_arguments));
  OMARCHY_CHECK(response.value("exitCode").toInt() == 126 &&
              response.value("stderr").toString() == "command-rejected");

  response = roundtrip(child.channel,
                      request(3, test_scope, "yes", {}));
  OMARCHY_CHECK(response.value("exitCode").toInt() == 125 &&
              response.value("stderr").toString() == "command-output-limit");

  const std::array<std::string_view, 1> sleep_arguments{"5"};
  response = roundtrip(child.channel,
                      request(4, test_scope, "sleep", sleep_arguments));
  OMARCHY_CHECK(response.value("exitCode").toInt() == 124 &&
              response.value("timedOut").toBool() &&
              response.value("stderr").toString() == "command-timeout");
  OMARCHY_CHECK(finish(child) == 0);

  child = start(true);
  const std::array<std::string_view, 4> mark_arguments{
      "api", "--method", "PATCH", "/notifications/threads/12345"};
  response = roundtrip(child.channel,
                       request(5, github_scope, "gh", mark_arguments));
  OMARCHY_CHECK(response.value("exitCode").toInt(-1) == 0 &&
              response.value("stdout").toString() == "policy-accepted");

  const std::array<std::string_view, 4> auth_arguments{
      "auth", "status", "--hostname", "github.com"};
  const std::array<std::string_view, 4> notification_arguments{
      "api", "-H",
      "Accept: application/vnd.github+json",
      "/notifications?all=false&participating=false&per_page=10&page=1"};
  const std::array<std::string_view, 4> search_arguments{
      "api", "-H",
      "Accept: application/vnd.github+json",
      "/search/issues?q=is%3Aopen+is%3Apr+review-requested%3A%40me+draft%3Afalse+archived%3Afalse&per_page=10&page=37"};
  const std::array<std::string_view, 6> pull_arguments{
      "api", "graphql", "-f",
      "query=query($search:String!) { search(query:$search,type:ISSUE,first:50) { issueCount nodes { ... on PullRequest { number title url updatedAt isDraft repository { nameWithOwner } commits(last:1) { nodes { commit { statusCheckRollup { state } } } } } } } } }",
      "-F", "search=is:open is:pr author:@me sort:updated-desc archived:false"};
  const std::array<std::string_view, 4> repository_arguments{
      "api", "graphql", "-f",
      "query=query($cursor:String) { viewer { login repositories(first:100,after:$cursor,ownerAffiliations:[OWNER,ORGANIZATION_MEMBER],orderBy:{field:UPDATED_AT,direction:DESC}) { nodes { name nameWithOwner url isArchived isFork stargazerCount updatedAt issues(states:OPEN){totalCount} pullRequests(states:OPEN){totalCount} } pageInfo { hasNextPage endCursor } } } rateLimit { remaining resetAt cost } }"};
  for (const auto &[correlation, arguments] :
       std::array<std::pair<std::uint64_t, std::span<const std::string_view>>, 5>{
           std::pair{std::uint64_t{11}, std::span<const std::string_view>(auth_arguments)},
           std::pair{std::uint64_t{12}, std::span<const std::string_view>(notification_arguments)},
           std::pair{std::uint64_t{13}, std::span<const std::string_view>(search_arguments)},
           std::pair{std::uint64_t{14}, std::span<const std::string_view>(pull_arguments)},
           std::pair{std::uint64_t{15}, std::span<const std::string_view>(repository_arguments)}}) {
    response = roundtrip(
        child.channel, request(correlation, github_scope, "gh", arguments));
    OMARCHY_CHECK(response.value("exitCode").toInt(-1) == 0);
  }

  const auto rejected = [&](std::uint64_t correlation,
                            std::span<const std::string_view> arguments) {
    const auto result = roundtrip(
        child.channel,
        request(correlation, github_scope, "gh", arguments));
    return result.value("exitCode").toInt() == 126 &&
           result.value("stderr").toString() == "command-rejected";
  };
  const std::array<std::string_view, 3> token_arguments{
      "auth", "status", "--show-token"};
  const std::array<std::string_view, 3> input_arguments{
      "api", "--input", "/etc/passwd"};
  const std::array<std::string_view, 2> content_arguments{
      "api", "/repos/private/project/contents/secret"};
  const std::array<std::string_view, 6> file_field_arguments{
      "api", "--method", "PUT", "/notifications", "-f",
      "last_read_at=@/etc/passwd"};
  const std::array<std::string_view, 4> alternate_host_arguments{
      "auth", "status", "--hostname", "attacker.example"};
  const std::array<std::string_view, 4> unbounded_page_arguments{
      "api", "-H", "Accept: application/vnd.github+json",
      "/notifications?all=false&participating=false&per_page=100&page=1"};
  const std::array<std::string_view, 4> zero_page_arguments{
      "api", "-H", "Accept: application/vnd.github+json",
      "/notifications?all=false&participating=false&per_page=10&page=0"};
  OMARCHY_CHECK(rejected(16, token_arguments) && rejected(17, input_arguments) &&
              rejected(18, content_arguments) &&
              rejected(19, file_field_arguments) &&
              rejected(20, alternate_host_arguments) &&
              rejected(21, unbounded_page_arguments) &&
              rejected(22, zero_page_arguments));
  OMARCHY_CHECK(finish(child) == 0);
  std::cout << "command executor tests passed\n";
}
