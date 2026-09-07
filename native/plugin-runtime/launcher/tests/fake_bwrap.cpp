#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace {
[[noreturn]] void fail() { _exit(125); }

int integer(std::string_view value) {
  int output = -1;
  const auto result =
      std::from_chars(value.data(), value.data() + value.size(), output);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
      output < 0) {
    fail();
  }
  return output;
}

std::string read_mode(int revision_fd) {
  const int descriptor =
      openat(revision_fd, "worker-mode", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0)
    fail();
  std::array<char, 64> bytes{};
  const ssize_t count = read(descriptor, bytes.data(), bytes.size());
  close(descriptor);
  if (count <= 0 || count == static_cast<ssize_t>(bytes.size()))
    fail();
  std::string mode(bytes.data(), static_cast<std::size_t>(count));
  if (!mode.empty() && mode.back() == '\n')
    mode.pop_back();
  return mode;
}

[[noreturn]] void run_worker(int barrier_fd, std::string &worker,
                             int state_fd = -1, std::string worker_mode = {}) {
  std::byte byte{};
  ssize_t count = -1;
  do {
    count = read(barrier_fd, &byte, sizeof(byte));
  } while (count < 0 && errno == EINTR);
  if (state_fd >= 0 && (state_fd == 6 ? fcntl(state_fd, F_SETFD, 0)
                                     : dup3(state_fd, 6, 0)) < 0) {
    fail();
  }
  if (count != 0 || syscall(SYS_close_range, state_fd >= 0 ? 7U : 6U, ~0U, 0U) < 0) {
    fail();
  }
  std::array<char *, 2> arguments{worker.data(), nullptr};
  if (state_fd >= 0)
    worker_mode = "OMARCHY_PLUGIN_TEST_WORKER_MODE=" + worker_mode;
  std::array<char *, 5> environment{
      const_cast<char *>("PATH=/usr/bin"), const_cast<char *>("PWD=/"),
      state_fd >= 0 ? worker_mode.data() : nullptr,
      const_cast<char *>("OMARCHY_PLUGIN_TEST_STATE_FD=6"), nullptr};
  execve(worker.c_str(), arguments.data(), environment.data());
  fail();
}
} // namespace

int main(int argc, char **argv) {
#ifdef OMARCHY_CHANNEL_FAKE_BWRAP
  constexpr bool channel = true;
#else
  constexpr bool channel = false;
#endif
  if (argc < 1 || argv[0] == nullptr)
    fail();
  const std::string_view invocation(argv[0]);
  const auto separator = invocation.find_last_of('/');
  const auto mode = invocation.substr(
      separator == std::string_view::npos ? 0 : separator + 1);
  int status_fd = -1;
  int barrier_fd = -1;
  int revision_fd = -1;
  int state_fd = -1;
  std::string worker;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--json-status-fd" && index + 1 < argc) {
      status_fd = integer(argv[++index]);
    } else if (argument == "--block-fd" && index + 1 < argc) {
      barrier_fd = integer(argv[++index]);
    } else if (argument == "--ro-bind" && index + 2 < argc &&
               std::string_view(argv[index + 2]) == "/runtime/worker") {
      worker = argv[index + 1];
      index += 2;
    } else if (channel && argument == "--ro-bind-fd" && index + 2 < argc &&
               std::string_view(argv[index + 2]) == "/plugin") {
      revision_fd = integer(argv[index + 1]);
      index += 2;
    } else if (channel && argument == "--bind-fd" && index + 2 < argc &&
               std::string_view(argv[index + 2]) == "/state") {
      state_fd = integer(argv[index + 1]);
      index += 2;
    }
  }
  if (status_fd < 0 || barrier_fd < 0 || worker.empty() ||
      (channel && (revision_fd < 0 || state_fd < 0))) {
    fail();
  }

  const auto worker_mode = channel ? read_mode(revision_fd) : std::string{};
  const bool fork_worker = !channel && mode == "omarchy-plugin-fake-bwrap";
  pid_t worker_pid = getpid();
  if (fork_worker) {
    worker_pid = fork();
    if (worker_pid < 0) {
      fail();
    }
  }

  if (worker_pid == 0)
    run_worker(barrier_fd, worker);

  std::string status;
  if (channel) {
    status = "{\"child-pid\":" + std::to_string(getpid()) + "}\n";
  } else if (fork_worker) {
    status = "{\"future\":{\"ignored\":true},\"child-pid\":" +
             std::to_string(worker_pid) + "}\n";
  } else if (mode == "omarchy-plugin-duplicate-status-bwrap") {
    status = "{\"child-pid\":" + std::to_string(getpid()) +
             ",\"child\\u002dpid\":" + std::to_string(getpid()) + "}\n";
  } else if (mode == "omarchy-plugin-string-status-bwrap") {
    status = "{\"child-pid\":\"" + std::to_string(getpid()) + "\"}\n";
  } else if (mode == "omarchy-plugin-exited-status-bwrap") {
    status =
        "{\"child-pid\":" + std::to_string(getpid()) + ",\"exit-code\":0}\n";
  } else {
    fail();
  }
  if (write(status_fd, status.data(), status.size()) !=
      static_cast<ssize_t>(status.size())) {
    fail();
  }

  if (fork_worker) {
    if (syscall(SYS_close_range, 3U, ~0U, 0U) < 0) {
      fail();
    }
    int status_value = 0;
    pid_t waited = -1;
    do {
      waited = waitpid(worker_pid, &status_value, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != worker_pid) {
      fail();
    }
    if (WIFEXITED(status_value)) {
      _exit(WEXITSTATUS(status_value));
    }
    if (WIFSIGNALED(status_value)) {
      _exit(128 + WTERMSIG(status_value));
    }
    fail();
  }

  run_worker(barrier_fd, worker, state_fd, worker_mode);
}
