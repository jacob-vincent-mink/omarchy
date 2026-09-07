#pragma once

#include <cstdint>
#include <optional>

namespace omarchy::plugin_runtime::surface {

inline constexpr std::uint32_t kSoftwareProfileVersion = 1;
inline constexpr std::uint32_t kRgba8888Premultiplied = 1;
inline constexpr std::uint32_t kMaximumPixelDimension = 4096;
inline constexpr std::uint64_t kMaximumFrameBytes = 64ULL * 1024ULL * 1024ULL;

struct ProfileOffer {
  std::uint32_t version;
  std::uint32_t maximum_pixel_dimension;
  std::uint64_t maximum_frame_bytes;
};

struct ProfileSelection {
  std::uint32_t version;
  std::uint32_t pixel_format;
};

[[nodiscard]] constexpr ProfileOffer software_profile_offer() {
  return {.version = kSoftwareProfileVersion,
          .maximum_pixel_dimension = kMaximumPixelDimension,
          .maximum_frame_bytes = kMaximumFrameBytes};
}

[[nodiscard]] constexpr std::optional<ProfileSelection>
select_software_profile(std::uint32_t offered_version) {
  if (offered_version != kSoftwareProfileVersion)
    return std::nullopt;
  return ProfileSelection{.version = kSoftwareProfileVersion,
                          .pixel_format = kRgba8888Premultiplied};
}

} // namespace omarchy::plugin_runtime::surface
