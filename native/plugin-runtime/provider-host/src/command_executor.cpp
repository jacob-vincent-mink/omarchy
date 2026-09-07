#include "secure_path.hpp"
#include "provider_protocol.hpp"
#include "omarchy/plugin_runtime/unique_fd.hpp"
#include "command_contract.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace protocol = omarchy::plugin_runtime::provider_protocol;

constexpr std::size_t kHeaderBytes = 20;
constexpr std::size_t kMaximumFrameBytes = 64 * 1024 + kHeaderBytes;
constexpr std::size_t kMaximumCommands = 16;
constexpr std::size_t kMaximumRules = 128;
constexpr std::size_t kMaximumMatchers = 64;
constexpr std::size_t kMaximumArgumentBytes = 4096;
constexpr std::size_t kMaximumTotalArgumentBytes = 16 * 1024;
constexpr std::size_t kMaximumStdoutBytes = 48 * 1024;
constexpr std::size_t kMaximumStderrBytes = 8 * 1024;
constexpr std::chrono::milliseconds kMaximumCommandTimeout{29000};

using ::omarchy::plugin_runtime::UniqueFd;

struct Matcher final {
  std::optional<std::string> exact;
  QRegularExpression expression;
};

struct Rule final {
  std::vector<Matcher> arguments;
};

struct Policy final {
  std::string command;
  std::string executable_path;
  std::chrono::milliseconds timeout{};
  std::size_t stdout_limit = 0;
  std::size_t stderr_limit = 0;
  bool account_home = false;
  std::vector<std::pair<QByteArray, QByteArray>> environment;
  std::vector<Rule> rules;
};

bool trusted_owner(uid_t owner) {
#ifdef OMARCHY_COMMAND_EXECUTOR_TESTING
  (void)owner;
  return true;
#else
  return owner == 0;
#endif
}


bool canonical_component(std::string_view value) {
  if (value.empty() || value.size() > 128)
    return false;
  return std::ranges::all_of(value, [](unsigned char byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
           byte == '-' || byte == '_' || byte == '.';
  });
}

using omarchy::plugin_runtime::provider_host::detail::canonical_absolute_path;

UniqueFd open_secure(std::string_view path) {
  return omarchy::plugin_runtime::provider_host::detail::open_secure_path(
      AT_FDCWD, "/", path,
      [](const struct stat &metadata) {
        return S_ISDIR(metadata.st_mode) && trusted_owner(metadata.st_uid) &&
               (metadata.st_mode & 0022) == 0;
      },
      [](const struct stat &metadata) {
        return S_ISREG(metadata.st_mode) && trusted_owner(metadata.st_uid) &&
               (metadata.st_mode & 0022) == 0 &&
               (metadata.st_mode & (S_ISUID | S_ISGID)) == 0 &&
               (metadata.st_mode & 0100) != 0 &&
               metadata.st_size >= 0;
      });
}

std::optional<std::uint32_t> positive_u32(const QJsonValue &value,
                                          std::uint32_t maximum) {
  if (!value.isDouble())
    return std::nullopt;
  const double number = value.toDouble();
  if (number < 1 || number > maximum || number != std::floor(number))
    return std::nullopt;
  return static_cast<std::uint32_t>(number);
}

std::optional<Policy> parse_policy(const QJsonObject &object) {
  if (!protocol::exact_keys(object, {},
                           {u"command", u"executable", u"timeoutMs", u"stdoutBytes",
                            u"stderrBytes", u"accountHome", u"environment", u"rules"}) ||
      !object.value("command").isString() ||
      !object.value("executable").isString() ||
      !object.value("accountHome").isBool() ||
      !object.value("environment").isObject() ||
      !object.value("rules").isArray())
    return std::nullopt;
  Policy policy;
  policy.command = object.value("command").toString().toStdString();
  policy.executable_path = object.value("executable").toString().toStdString();
  const auto timeout = positive_u32(object.value("timeoutMs"),
                                    kMaximumCommandTimeout.count());
  const auto stdout_limit = positive_u32(object.value("stdoutBytes"),
                                         kMaximumStdoutBytes);
  const auto stderr_limit = positive_u32(object.value("stderrBytes"),
                                         kMaximumStderrBytes);
  if (!canonical_component(policy.command) ||
      !canonical_absolute_path(policy.executable_path) || !timeout ||
      !stdout_limit || !stderr_limit)
    return std::nullopt;
  policy.timeout = std::chrono::milliseconds(*timeout);
  policy.stdout_limit = *stdout_limit;
  policy.stderr_limit = *stderr_limit;
  policy.account_home = object.value("accountHome").toBool();
  const auto environment = object.value("environment").toObject();
  if (environment.size() > 16)
    return std::nullopt;
  static const QRegularExpression environment_name(
      QStringLiteral("\\A[A-Z][A-Z0-9_]{0,63}\\z"));
  for (auto iterator = environment.begin(); iterator != environment.end();
       ++iterator) {
    if (!iterator.value().isString() ||
        !environment_name.match(iterator.key()).hasMatch() ||
        iterator.key() == QStringLiteral("PATH") ||
        iterator.key() == QStringLiteral("HOME") ||
        iterator.key() == QStringLiteral("LANG") ||
        iterator.key() == QStringLiteral("LC_ALL") ||
        iterator.key().startsWith(QStringLiteral("LD_")) ||
        iterator.key().startsWith(QStringLiteral("QT_")))
      return std::nullopt;
    const auto value = iterator.value().toString().toUtf8();
    if (value.size() > 1024 || value.contains('\0'))
      return std::nullopt;
    policy.environment.emplace_back(iterator.key().toUtf8(), value);
  }

  const auto rules = object.value("rules").toArray();
  if (rules.empty() || rules.size() > static_cast<qsizetype>(kMaximumRules))
    return std::nullopt;
  for (const auto &rule_value : rules) {
    if (!rule_value.isArray())
      return std::nullopt;
    const auto matcher_values = rule_value.toArray();
    if (matcher_values.size() > static_cast<qsizetype>(kMaximumMatchers))
      return std::nullopt;
    Rule rule;
    for (const auto &matcher_value : matcher_values) {
      if (!matcher_value.isObject())
        return std::nullopt;
      const auto matcher_object = matcher_value.toObject();
      if (!protocol::exact_keys(matcher_object, {}, {u"exact", u"regex"}) ||
          matcher_object.size() != 1)
        return std::nullopt;
      Matcher matcher;
      if (matcher_object.value("exact").isString()) {
        matcher.exact = matcher_object.value("exact").toString().toStdString();
        if (matcher.exact->size() > kMaximumArgumentBytes ||
            matcher.exact->find('\0') != std::string::npos)
          return std::nullopt;
      } else if (matcher_object.value("regex").isString()) {
        const auto source = matcher_object.value("regex").toString();
        if (source.isEmpty() || source.size() > 1024)
          return std::nullopt;
        matcher.expression = QRegularExpression(
            QStringLiteral("\\A(?:") + source + QStringLiteral(")\\z"),
            QRegularExpression::DontCaptureOption);
        if (!matcher.expression.isValid())
          return std::nullopt;
      } else {
        return std::nullopt;
      }
      rule.arguments.push_back(std::move(matcher));
    }
    policy.rules.push_back(std::move(rule));
  }
  return policy;
}

std::optional<std::vector<Policy>> policies_from_scope(std::string_view scope) {
  if (scope.size() > 4096)
    return std::nullopt;
  QJsonParseError error{};
  const auto document = QJsonDocument::fromJson(
      QByteArray(scope.data(), static_cast<qsizetype>(scope.size())), &error);
  if (error.error != QJsonParseError::NoError || !document.isObject())
    return std::nullopt;
  const auto object = document.object();
  if (object.size() != 1 || !object.value("commands").isArray())
    return std::nullopt;
  const auto commands = object.value("commands").toArray();
  if (commands.empty() || commands.size() > static_cast<qsizetype>(kMaximumCommands))
    return std::nullopt;
  std::vector<Policy> policies;
  for (const auto &command : commands) {
    auto policy = command.isObject() ? parse_policy(command.toObject()) : std::nullopt;
    if (!policy || std::ranges::any_of(policies, [&](const auto &existing) {
          return existing.command == policy->command;
        }))
      return std::nullopt;
    policies.push_back(std::move(*policy));
  }
  return policies;
}

struct Request final {
  std::uint64_t correlation = 0;
  std::string_view operation;
  std::string_view demand_scope;
  std::string command;
  std::vector<std::string> arguments;
};

std::optional<Request> decode_request(std::span<const std::byte> frame) {
  const auto decoded = protocol::decode(frame);
  if (!decoded || decoded->adapter != "bounded-command-execute" || decoded->operation != "run" ||
      decoded->contract != omarchy::plugin_runtime::command_executor::kContractDigest)
    return std::nullopt;
  const auto correlation = decoded->correlation;
  const auto demand_scope = decoded->scope;
  const auto operation = decoded->operation;
  const auto &object = decoded->payload;
  if (!protocol::exact_keys(object, {u"command", u"arguments"}) ||
      !object.value("command").isString() ||
      !object.value("arguments").isArray())
    return std::nullopt;
  Request request{.correlation = correlation,
                  .operation = operation,
                  .demand_scope = demand_scope,
                  .command = object.value("command").toString().toStdString(),
                  .arguments = {}};
  const auto arguments = object.value("arguments").toArray();
  if (arguments.size() > static_cast<qsizetype>(kMaximumMatchers))
    return std::nullopt;
  std::size_t total = request.command.size();
  for (const auto &argument : arguments) {
    if (!argument.isString())
      return std::nullopt;
    auto value = argument.toString().toStdString();
    if (value.size() > kMaximumArgumentBytes ||
        value.find('\0') != std::string::npos ||
        total > kMaximumTotalArgumentBytes - value.size())
      return std::nullopt;
    total += value.size();
    request.arguments.push_back(std::move(value));
  }
  return request;
}

bool matches(const Policy &policy, const Request &request) {
  if (request.command != policy.command)
    return false;
  return std::ranges::any_of(policy.rules, [&](const Rule &rule) {
    if (rule.arguments.size() != request.arguments.size())
      return false;
    for (std::size_t index = 0; index < rule.arguments.size(); ++index) {
      const auto &matcher = rule.arguments[index];
      const auto &argument = request.arguments[index];
      if (matcher.exact && argument != *matcher.exact)
        return false;
      if (!matcher.exact) {
        const auto text = QString::fromUtf8(argument);
        if (text.toUtf8().toStdString() != argument ||
            !matcher.expression.match(text).hasMatch())
          return false;
      }
    }
    return true;
  });
}

QByteArray fixed_home() {
  const auto *account = ::getpwuid(::getuid());
  if (!account || !account->pw_dir)
    return "/nonexistent";
  const QByteArray home(account->pw_dir);
  if (!home.startsWith('/') || home.contains('\0'))
    return "/nonexistent";
  return home;
}

struct CommandResult final {
  int exit_code = 126;
  QByteArray standard_output;
  QByteArray standard_error;
  bool timed_out = false;
};

bool append_pipe(UniqueFd &descriptor, QByteArray &output,
                 std::size_t limit, bool &exceeded) {
  std::array<char, 4096> buffer{};
  while (descriptor) {
    const auto count = ::read(descriptor.get(), buffer.data(), buffer.size());
    if (count > 0) {
      const auto incoming = static_cast<std::size_t>(count);
      if (incoming > limit ||
          static_cast<std::size_t>(output.size()) > limit - incoming)
        exceeded = true;
      else
        output.append(buffer.data(), count);
      continue;
    }
    if (count == 0) {
      descriptor.reset();
      return true;
    }
    if (errno == EINTR)
      continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return true;
    return false;
  }
  return true;
}

void kill_group(pid_t pid) {
  if (pid > 0)
    (void)::kill(-pid, SIGKILL);
}

CommandResult run(const Policy &policy, const Request &request) {
  CommandResult result;
  auto executable = open_secure(policy.executable_path);
  if (!executable) {
    result.standard_error = "command-unavailable";
    return result;
  }
  int stdout_pipe[2] = {-1, -1};
  int stderr_pipe[2] = {-1, -1};
  if (::pipe2(stdout_pipe, O_CLOEXEC) < 0 ||
      ::pipe2(stderr_pipe, O_CLOEXEC) < 0) {
    if (stdout_pipe[0] >= 0) {
      ::close(stdout_pipe[0]);
      ::close(stdout_pipe[1]);
    }
    result.standard_error = "command-launch-failed";
    return result;
  }
  UniqueFd stdout_read(stdout_pipe[0]);
  UniqueFd stdout_write(stdout_pipe[1]);
  UniqueFd stderr_read(stderr_pipe[0]);
  UniqueFd stderr_write(stderr_pipe[1]);
  if (::fcntl(stdout_read.get(), F_SETFL,
              ::fcntl(stdout_read.get(), F_GETFL) | O_NONBLOCK) < 0 ||
      ::fcntl(stderr_read.get(), F_SETFL,
              ::fcntl(stderr_read.get(), F_GETFL) | O_NONBLOCK) < 0) {
    result.standard_error = "command-launch-failed";
    return result;
  }
  std::vector<char *> argv;
  argv.reserve(request.arguments.size() + 2);
  argv.push_back(const_cast<char *>(policy.command.c_str()));
  for (const auto &argument : request.arguments)
    argv.push_back(const_cast<char *>(argument.c_str()));
  argv.push_back(nullptr);
  const auto home = policy.account_home ? fixed_home() : QByteArray("/nonexistent");
  std::vector<QByteArray> environment_values{
      "PATH=/usr/bin", "LANG=C.UTF-8", "LC_ALL=C.UTF-8", "HOME=" + home};
  for (const auto &[name, value] : policy.environment)
    environment_values.push_back(name + "=" + value);
  std::vector<char *> environment;
  environment.reserve(environment_values.size() + 1);
  for (auto &value : environment_values)
    environment.push_back(value.data());
  environment.push_back(nullptr);

  const pid_t child = ::fork();
  if (child < 0) {
    result.standard_error = "command-launch-failed";
    return result;
  }
  if (child == 0) {
    (void)::setpgid(0, 0);
    (void)::umask(0077);
    stdout_read.reset();
    stderr_read.reset();
    const int null_fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (::chdir("/") < 0 || null_fd < 0 ||
        ::dup2(null_fd, STDIN_FILENO) != STDIN_FILENO ||
        ::dup2(stdout_write.get(), STDOUT_FILENO) != STDOUT_FILENO ||
        ::dup2(stderr_write.get(), STDERR_FILENO) != STDERR_FILENO ||
        ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0 ||
        ::syscall(SYS_close_range, 3U, UINT_MAX, CLOSE_RANGE_CLOEXEC) < 0)
      _exit(126);
    (void)::syscall(SYS_execveat, executable.get(), "", argv.data(),
                    environment.data(), AT_EMPTY_PATH);
    _exit(127);
  }
  (void)::setpgid(child, child);
  stdout_write.reset();
  stderr_write.reset();
  const auto deadline = std::chrono::steady_clock::now() + policy.timeout;
  bool exceeded = false;
  int status = 0;
  bool reaped = false;
  while (stdout_read || stderr_read || !reaped) {
    if (std::chrono::steady_clock::now() >= deadline) {
      result.timed_out = true;
      kill_group(child);
    }
    std::array<pollfd, 2> poll_items{{
        {.fd = stdout_read.get(), .events = POLLIN, .revents = 0},
        {.fd = stderr_read.get(), .events = POLLIN, .revents = 0}}};
    (void)::poll(poll_items.data(), poll_items.size(), result.timed_out ? 0 : 25);
    if (!append_pipe(stdout_read, result.standard_output, policy.stdout_limit,
                     exceeded) ||
        !append_pipe(stderr_read, result.standard_error, policy.stderr_limit,
                     exceeded)) {
      exceeded = true;
    }
    if (exceeded)
      kill_group(child);
    if (!reaped) {
      const auto waited = ::waitpid(child, &status, WNOHANG);
      reaped = waited == child || (waited < 0 && errno == ECHILD);
    }
    if ((exceeded || result.timed_out) && !reaped) {
      while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
      reaped = true;
    }
  }
  if (exceeded) {
    result.exit_code = 125;
    result.standard_output.clear();
    result.standard_error = "command-output-limit";
  } else if (result.timed_out) {
    result.exit_code = 124;
    result.standard_output.clear();
    result.standard_error = "command-timeout";
  } else if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result.exit_code = 128 + WTERMSIG(status);
  }
  // A reviewed CLI is expected to finish its own helpers. Kill anything that
  // nevertheless retained the command process group before returning success.
  kill_group(child);
  return result;
}

QByteArray result_json(const CommandResult &result) {
  return QJsonDocument(QJsonObject{
                           {"exitCode", result.exit_code},
                           {"stdout", QString::fromUtf8(result.standard_output)},
                           {"stderr", QString::fromUtf8(result.standard_error)},
                           {"timedOut", result.timed_out}})
      .toJson(QJsonDocument::Compact);
}

using protocol::send_response;
} // namespace

int main(int argc, char **argv) {
  bool validate_only = false;
#ifdef OMARCHY_COMMAND_EXECUTOR_TESTING
  validate_only = argc == 2 && std::string_view(argv[1]) == "--validate-only";
#else
  (void)argv;
#endif
  if (argc != 1 && !validate_only)
    return 64;
  return protocol::serve(decode_request, [&](const auto &request) {
    const auto policies = policies_from_scope(request.demand_scope);
    const Policy *policy = nullptr;
    if (policies) {
      const auto found = std::ranges::find(*policies, request.command, &Policy::command);
      if (found != policies->end()) policy = &*found;
    }
    CommandResult result;
    if (!policy || !matches(*policy, request)) {
      result.standard_error = "command-rejected";
    } else if (validate_only) {
      result = {.exit_code = 0,
                .standard_output = "policy-accepted",
                .standard_error = {},
                .timed_out = false};
    } else {
      result = run(*policy, request);
    }
    auto payload = result_json(result);
    if (static_cast<std::size_t>(payload.size()) >
        kMaximumFrameBytes - kHeaderBytes - 1) {
      result = {.exit_code = 125,
                .standard_output = {},
                .standard_error = "command-output-encoding-limit",
                .timed_out = false};
      payload = result_json(result);
    }
    return send_response(request.correlation, payload);
  });
}
