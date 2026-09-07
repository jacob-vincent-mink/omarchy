#include "omarchy/plugin_runtime/test_support/test_support.h"
#include "../concurrent_mutation.hpp"
#include "omarchy/plugin_runtime/file_metadata.hpp"
#include "omarchy/plugin_runtime/manifest_identifier.hpp"
#include "../test_assert.hpp"
#include "../child_process.hpp"
#include "../blocking_gate.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace support = omarchy::plugin_runtime::test_support;

namespace {
using omarchy::plugin_runtime::test_support::exit_assertions::fail;
using omarchy::plugin_runtime::test_support::exit_assertions::require;

void test_main_test() {
  struct Capture {
    std::ostringstream output;
    std::streambuf *previous = std::cerr.rdbuf(output.rdbuf());
    ~Capture() { std::cerr.rdbuf(previous); }
  } capture;
  int calls = 0;
  for (int code : {0, 1, 77, 99})
    require(support::test_main([&] { ++calls; return code; }) == code,
            "test runner changed the operation result");
  require(calls == 4 && capture.output.str().empty(),
          "test runner repeated a call or wrote on success");
  for (const auto prefix : {"", "fixture: "})
    require(support::test_main([]() -> int {
      struct Unwind { ~Unwind() { std::cerr << "unwound|"; } } unwind;
      throw std::runtime_error("failure");
    }, prefix) == 1, "test runner did not report a standard exception");
  require(capture.output.str() == "unwound|failure\nunwound|fixture: failure\n",
          "test runner changed unwinding or exact error output");
  require(support::throws_exception<int>([] {
    support::test_main([]() -> int { throw 42; });
  }), "test runner swallowed a nonstandard exception");
}

void blocking_gate_test() {
  support::BlockingGate gate;
  require(!gate.wait_entered_for(std::chrono::milliseconds(0)), "unentered gate reported entry");
  std::atomic<bool> returned = false;
  int published = 0;
  std::thread worker([&] {
    gate.arrive_and_wait([&] { published = 42; });
    returned = true;
  });
  gate.wait_entered();
  require(published == 42 && !returned, "gate failed to publish entry before release");
  gate.release();
  gate.release();
  worker.join();
  require(returned && gate.wait_entered_for(std::chrono::milliseconds(0)),
          "gate failed to retain entry or release its waiter");
  support::BlockingGate early;
  early.release();
  early.arrive_and_wait();
  require(early.wait_entered_for(std::chrono::milliseconds(0)), "early release lost entry");
}

void concurrent_mutation_test() {
  for (const int success_at : {0, 3, -1}) {
    std::atomic<std::size_t> changes{0};
    std::atomic<bool> wrong_thread{false};
    const auto caller = std::this_thread::get_id();
    int probes = 0;
    bool observed = false, threw = false;
    try {
      observed = support::observe_concurrent_mutation([&] {
        wrong_thread.store(std::this_thread::get_id() == caller);
        ++changes;
      }, [&] {
        require(changes >= 100, "mutation probe ran before warmup");
        ++probes;
        if (success_at == -1) throw std::runtime_error("probe failure");
        return probes == success_at;
      }, 100, 5);
    } catch (const std::runtime_error &) { threw = true; }
    const auto stopped = changes.load();
    for (int pass = 0; pass < 100; ++pass) std::this_thread::yield();
    require(!wrong_thread && changes == stopped &&
                probes == (success_at == 0 ? 5 : success_at == -1 ? 1 : 3) &&
                observed == (success_at == 3) && threw == (success_at == -1),
            "mutation runner lost bounded probes, result, or joined cleanup");
  }
}

void manifest_identifier_test() {
  using omarchy::plugin_runtime::canonical_manifest_identifier;
  constexpr std::string_view letters = "abcdefghijklmnopqrstuvwxyz";
  const std::string alphanumeric = std::string(letters) + "0123456789";
  const std::string interior = alphanumeric + ".-_";
  for (unsigned byte = 0; byte <= 255; ++byte) {
    const auto character = static_cast<char>(byte);
    const std::string one(1, character);
    require(canonical_manifest_identifier(one) ==
                (letters.find(character) != letters.npos),
            "invalid one-character identifier classification");
    require(canonical_manifest_identifier(one + "a") ==
                (letters.find(character) != letters.npos),
            "invalid initial identifier byte classification");
    require(canonical_manifest_identifier("a" + one + "b") ==
                (interior.find(character) != interior.npos),
            "invalid interior identifier byte classification");
    require(canonical_manifest_identifier("a" + one) ==
                (alphanumeric.find(character) != alphanumeric.npos),
            "invalid final identifier byte classification");
  }
  for (const char first : std::string_view(".-_"))
    for (const char second : std::string_view(".-_"))
      require(!canonical_manifest_identifier(
                  std::string("a") + first + second + "b"),
              "adjacent identifier separators accepted");
  require(!canonical_manifest_identifier("") &&
              canonical_manifest_identifier(std::string(128, 'a')) &&
              !canonical_manifest_identifier(std::string(129, 'a')) &&
              canonical_manifest_identifier("org.example-test_plugin0"),
          "identifier length or mixed separator policy changed");
}

void child_process_test() {
  const auto parent = ::getpid();
  for (const int expected : {0, 37})
    support::expect_child_exit(expected, [=] {
      ::_exit(::getpid() != parent ? expected : 99);
    });
  for (const auto probe : {+[] { ::_exit(1); },
                           +[] { ::kill(::getpid(), SIGKILL); }, +[] {}}) {
    bool rejected = false;
    try {
      support::expect_child_exit(0, probe);
    } catch (const std::runtime_error &) {
      rejected = true;
    }
    require(rejected, "wrong exit, signal, or returned child probe was accepted");
  }
}

void file_metadata_test() {
  using omarchy::plugin_runtime::same_file_metadata;
  const struct stat baseline{};
  require(same_file_metadata(baseline, baseline), "identical metadata differs");
  struct Mutation {
    const char *field;
    void (*apply)(struct stat &);
  };
  const Mutation mutations[]{
      {"device", [](struct stat &s) { ++s.st_dev; }},
      {"inode", [](struct stat &s) { ++s.st_ino; }},
      {"mode", [](struct stat &s) { ++s.st_mode; }},
      {"owner", [](struct stat &s) { ++s.st_uid; }},
      {"group", [](struct stat &s) { ++s.st_gid; }},
      {"links", [](struct stat &s) { ++s.st_nlink; }},
      {"size", [](struct stat &s) { --s.st_size; }},
      {"mtime seconds", [](struct stat &s) { ++s.st_mtim.tv_sec; }},
      {"mtime nanoseconds", [](struct stat &s) { ++s.st_mtim.tv_nsec; }},
      {"ctime seconds", [](struct stat &s) { ++s.st_ctim.tv_sec; }},
      {"ctime nanoseconds", [](struct stat &s) { ++s.st_ctim.tv_nsec; }},
  };
  for (const auto &mutation : mutations) {
    auto changed = baseline;
    mutation.apply(changed);
    require(!same_file_metadata(baseline, changed) &&
                !same_file_metadata(changed, baseline), mutation.field);
  }
  auto incidental = baseline;
  incidental.st_atim = {.tv_sec = 1, .tv_nsec = 2};
  incidental.st_blksize = 4096;
  incidental.st_blocks = 1;
  incidental.st_rdev = 2;
  require(same_file_metadata(baseline, incidental),
          "access time or allocation metadata changed stable identity");
}

void expression_check_test() {
  int calls_to_operation = 0;
  require(!support::throws_exception<std::runtime_error>([&] { ++calls_to_operation; }) &&
              calls_to_operation == 1, "normal return counted as an exception");
  require(support::throws_exception<std::runtime_error>([&] {
    ++calls_to_operation;
    throw std::runtime_error("expected");
  }) && calls_to_operation == 2, "expected exception lost or operation repeated");
  try {
    support::throws_exception<std::runtime_error>([] { throw 42; });
    fail("unexpected exception was swallowed");
  } catch (int value) {
    require(value == 42, "unexpected exception was changed");
  }
  require(support::throws_exception<std::exception>([] {
    throw std::invalid_argument("derived");
  }), "derived exception was not caught by its base type");
  int evaluations = 0;
  OMARCHY_CHECK(++evaluations == 1);
  OMARCHY_CHECK(std::array<int, 2>{1, 2}[0] == 1);
  bool caught = false;
  unsigned failure_line = 0;
  try {
    failure_line = __LINE__ + 1;
    OMARCHY_CHECK(++evaluations == 1);
  } catch (const std::runtime_error &error) {
    caught = true;
    require(evaluations == 2 &&
                error.what() == std::string(__FILE__) + ":" +
                                    std::to_string(failure_line) + ": ++evaluations == 1",
            "expression check repeated evaluation or lost failure context");
  }
  require(caught, "failed expression check did not throw");

  unsigned calls = 0;
  bool result = true;
  std::string context;
  const auto record = [&](bool value, const char *message) {
    ++calls;
    result = value;
    context = message;
  };
  const auto line = __LINE__ + 1;
  OMARCHY_CHECK_WITH(record, ++evaluations == 2);
  require(calls == 1 && evaluations == 3 && !result &&
              context == std::string(__FILE__) + ":" + std::to_string(line) +
                             ": ++evaluations == 2",
          "custom checker lost single evaluation or source context");
  OMARCHY_CHECK_WITH(require, std::array<int, 2>{1, 2}[0] == 1);
  support::expect_child_exit(37, [] {
    try {
      const auto exit_check = [](bool value, const char *) {
        std::exit(value ? 0 : 37);
      };
      OMARCHY_CHECK_WITH(exit_check, false);
    } catch (...) {
      ::_exit(99);
    }
    ::_exit(98);
  });
}

void temporary_directory_test() {
  support::TemporaryDirectory external;
  const auto protected_child = external.path() / "protected";
  std::filesystem::create_directory(protected_child);
  const auto protected_mode = std::filesystem::perms::owner_read |
                              std::filesystem::perms::owner_exec;
  std::filesystem::permissions(protected_child, protected_mode);
  std::filesystem::permissions(external.path(), protected_mode);
  const auto original = std::filesystem::status(external.path()).permissions();
  std::filesystem::path removed;
  {
    support::TemporaryDirectory tree;
    removed = tree.path();
    std::filesystem::create_directory_symlink(external.path(), tree.path() / "link");
    const auto sealed = tree.path() / "sealed";
    std::filesystem::create_directory(sealed);
    std::filesystem::permissions(sealed, std::filesystem::perms::none);
  }
  require(!std::filesystem::exists(removed), "sealed temporary tree survived cleanup");
  {
    support::TemporaryDirectory tree;
    removed = tree.path();
    std::filesystem::remove(tree.path());
    std::filesystem::create_directory_symlink(external.path(), tree.path());
  }
  require(!std::filesystem::is_symlink(removed) &&
              std::filesystem::status(external.path()).permissions() == original &&
              std::filesystem::status(protected_child).permissions() == protected_mode,
          "temporary cleanup followed a symlink or modified its target");
}

void descriptor_support_test() {
  const auto before = support::open_fd_set();
  {
    support::UniqueFd descriptor(open("/dev/null", O_RDONLY | O_CLOEXEC));
    require(static_cast<bool>(descriptor), "UniqueFd fixture could not open");
    const auto during = support::open_fd_set();
    require(during.size() == before.size() + 1,
            "open descriptor fixture is not observable");
  }
  require(support::open_fd_set() == before,
          "UniqueFd fixture leaked a descriptor");
}

} // namespace

int main() {
  test_main_test();
  blocking_gate_test();
  concurrent_mutation_test();
  child_process_test();
  manifest_identifier_test();
  file_metadata_test();
  expression_check_test();
  temporary_directory_test();
  descriptor_support_test();
  return 0;
}
