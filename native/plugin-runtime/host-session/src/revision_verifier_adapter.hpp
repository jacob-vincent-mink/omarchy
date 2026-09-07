#pragma once

#include "activation_snapshot.hpp"

#include <cstdint>

namespace omarchy::plugin_runtime::host_session {

// Runtime adapter from the activation seam to descriptor-relative plugin
// discovery. It intentionally has no pathname-based fallback.
class DescriptorRevisionVerifier final : public RevisionVerifier {
public:
  explicit DescriptorRevisionVerifier(std::uint32_t expected_uid) noexcept
      : expected_uid_(expected_uid) {}
  [[nodiscard]] std::optional<VerifiedRevision>
  verify_open_revision(int revision_directory_fd) const override;

private:
  std::uint32_t expected_uid_;
};

} // namespace omarchy::plugin_runtime::host_session
