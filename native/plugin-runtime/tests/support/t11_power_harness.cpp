// Standalone QEMU power-loss harness for the SQLite authority store (T11).
// Build as a one-off executable; this is a test artifact, not a committed test.
//
//   publish N  - open the authority store at $AUTH_DIR (default /root/auth) and
//                perform N atomic publish+promote generations, printing
//                DURABLE-GEN-<i> only after each transaction is durably
//                committed. The host process can hard-kill (power-loss) at any
//                point so that a reboot lands with the disk in whatever state
//                the last committed fsync left.
//
//   verify     - reopen the store from disk and report whether the persisted
//                authority is a fully coherent committed generation, a clean/
//                empty store, or (the unsafe case) claims an active authority
//                it cannot fully resolve. The store's fail-closed path must
//                never surface a torn half-published grant as a valid active
//                authority.
#include "authority_store.hpp"

#include "manifest_contract.hpp"
#include "permission_fixture.hpp"
#include "capability_fixture.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace host = omarchy::plugin_runtime::host_session;
namespace policy = omarchy::plugin_runtime::policy;
namespace permissions = omarchy::plugins::permissions;
namespace definitions = omarchy::plugins::definitions;
namespace manifest = omarchy::plugins::manifest;
namespace test_support = omarchy::plugin_runtime::test_support;

constexpr std::string_view kPlugin = "org.example.authority";

static std::string hex(char value) { return std::string(64, value); }

static host::VerifiedRevision make_verified(manifest::ManifestV2 manifest,
                                            char rev,
                                            const policy::GrantSnapshot &snap) {
  return host::VerifiedRevision{
      .manifest = std::move(manifest),
      .tree_sha256 = hex(rev),
      .request_sha256 = std::string(snap.binding.policy_fingerprint.view())};
}

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s publish N | verify\n", argv[0]);
    return 2;
  }
  const char *dir = std::getenv("AUTH_DIR");
  if (dir == nullptr || *dir == '\0') dir = "/root/auth";
  ::mkdir(dir, 0700);
  ::chmod(dir, 0700);
  const int rootfd = ::open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (rootfd < 0) {
    std::perror("open auth dir");
    return 3;
  }
  std::unique_ptr<host::AuthorityStore> store =
      host::AuthorityStore::open(rootfd, static_cast<std::uint32_t>(::getuid()),
                                 permissions::PluginId(kPlugin));
  if (!store) {
    std::fprintf(stderr, "store open failed (errno=%d)\n", errno);
    struct stat st {};
    if (::fstat(rootfd, &st) == 0)
      std::fprintf(stderr, "diag dir mode=%o uid=%d\n", st.st_mode, (int)st.st_uid);
    std::fprintf(stderr, "diag /proc/self/fd accessible=%d\n",
                 ::access("/proc/self/fd", R_OK | X_OK) == 0);
    std::fprintf(stderr, "diag uid(eff/real)=%d/%d\n", (int)::geteuid(), (int)::getuid());
    return 4;
  }

  const std::string mode = argv[1];

  if (mode == "publish") {
    const int gens = argc > 2 ? std::atoi(argv[2]) : 5;
    const long sleep_ms =
        std::getenv("SLEEP_MS") ? std::atol(std::getenv("SLEEP_MS")) : 0L;
    auto registry = test_support::packaged_registry();
    auto initial = store->read_slots();
    const std::uint64_t start =
        initial && initial->generation_high_watermark > 0
            ? initial->generation_high_watermark
            : 0;
    for (int i = 1; i <= gens; ++i) {
      const std::uint64_t gen = start + static_cast<std::uint64_t>(i);
      std::printf("BEGIN-GEN-%llu\n",
                  static_cast<unsigned long long>(gen));
      std::fflush(stdout);
      if (sleep_ms > 0) ::usleep(static_cast<useconds_t>(sleep_ms) * 1000);
      const char rev =
          "0123456789abcdef"[(gen - 1) % 16];
      manifest::ManifestV2 manifest;
      manifest.id = kPlugin;
      manifest.requests.push_back(test_support::capability_request(
          registry, "notifications.send", "{\"categories\":[\"status\"]}", true));
      auto snap = test_support::permission_snapshot(
          registry, manifest, hex(rev), gen,
          permissions::GrantState::granted);
      host::VerifiedRevision verified = make_verified(std::move(manifest), rev, snap);
      auto slots = store->read_slots();
      const std::uint64_t seq = slots ? slots->sequence : 0;
      const auto m1 = store->publish_candidate(verified, snap, seq, registry);
      const auto m2 = store->promote_candidate(snap.binding, seq + 1);
      if (m1 == host::AuthorityMutationResult::applied &&
          m2 == host::AuthorityMutationResult::applied) {
        std::printf("DURABLE-GEN-%llu\n", static_cast<unsigned long long>(gen));
      } else {
        std::printf("FAILED-GEN-%d m1=%d m2=%d\n", i, static_cast<int>(m1),
                    static_cast<int>(m2));
        std::fflush(stdout);
        return 5;
      }
      std::fflush(stdout);
    }
    return 0;
  }

  if (mode == "verify") {
    auto slots = store->read_slots();
    auto view = store->read_authority_view();
    const long active_gen =
        slots && slots->active ? static_cast<long>(slots->active->generation) : -1;
    const long cand_gen =
        slots && slots->candidate ? static_cast<long>(slots->candidate->generation) : -1;
    std::printf("slots seq=%llu hw=%llu active_gen=%ld cand_gen=%ld\n",
                static_cast<unsigned long long>(slots ? slots->sequence : 0),
                static_cast<unsigned long long>(
                    slots ? slots->generation_high_watermark : 0),
                active_gen, cand_gen);
    if (slots && slots->active && (!view || !view->active)) {
      // The store claims an active authority that it cannot decode into a
      // coherent GrantSnapshot: a torn/half-published surface. This is the
      // unsafe outcome.
      std::printf("VERIFY-TORN-UNSAFE\n");
      std::fflush(stdout);
      return 6;
    }
    if (view && view->active) {
      std::printf("VERIFY-OK-ACTIVE-GEN-%llu\n",
                  static_cast<unsigned long long>(view->active->binding.generation));
    } else if (slots) {
      std::printf("VERIFY-CLEAN-NO-ACTIVE\n");
    } else {
      std::printf("VERIFY-POISONED-OR-UNAVAILABLE\n");
    }
    std::fflush(stdout);
    return 0;
  }

  std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
  return 2;
}
