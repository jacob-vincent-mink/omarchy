#pragma once

#include <cstdint>
#include <memory>

struct sqlite3;

namespace omarchy::plugin_runtime::host_session {

// Filesystem boundary only. The authority owner supplies SQL and retains its
// lifetime lock, generation comparisons and live-effect fence. SQLite owns all
// page I/O, locking and rollback recovery through its unmodified Unix backend.
// Like AuthorityStore, this protects against sandboxed plugins and accidental
// corruption, not hostile arbitrary code already running as the owning UID.
class SqliteAuthorityDatabase final {
public:
  static std::unique_ptr<SqliteAuthorityDatabase>
  open(int directory_fd, std::uint32_t expected_uid);
  ~SqliteAuthorityDatabase();
  SqliteAuthorityDatabase(const SqliteAuthorityDatabase &) = delete;
  SqliteAuthorityDatabase &operator=(const SqliteAuthorityDatabase &) = delete;

  // Statements must not outlive this object. Check the boundary before each
  // authority operation; a changed file must never become an empty authority.
  [[nodiscard]] bool intact() const;
  sqlite3 *connection() const;

private:
  struct State;
  explicit SqliteAuthorityDatabase(std::unique_ptr<State> state);
  std::unique_ptr<State> state_;
};

} // namespace omarchy::plugin_runtime::host_session
