#pragma once

#include <unistd.h>
#include <utility>

namespace omarchy::plugin_runtime {

// Owns an already-open descriptor, not the authority to reopen its pathname.
class UniqueFd final {
public:
  UniqueFd() = default;
  explicit UniqueFd(int descriptor) noexcept : descriptor_(descriptor) {}
  UniqueFd(const UniqueFd &) = delete;
  UniqueFd &operator=(const UniqueFd &) = delete;
  UniqueFd(UniqueFd &&other) noexcept : descriptor_(other.release()) {}
  UniqueFd &operator=(UniqueFd &&other) noexcept {
    if (this != &other)
      reset(other.release());
    return *this;
  }
  ~UniqueFd() { reset(); }

  [[nodiscard]] int get() const noexcept { return descriptor_; }
  [[nodiscard]] explicit operator bool() const noexcept { return descriptor_ >= 0; }
  [[nodiscard]] int release() noexcept { return std::exchange(descriptor_, -1); }
  void reset(int descriptor = -1) noexcept {
    if (descriptor_ == descriptor)
      return;
    if (descriptor_ >= 0)
      ::close(descriptor_);
    descriptor_ = descriptor;
  }

private:
  int descriptor_ = -1;
};

} // namespace omarchy::plugin_runtime
