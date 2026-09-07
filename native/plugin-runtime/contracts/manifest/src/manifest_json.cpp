#include "manifest_json.hpp"

#include <nlohmann/json.hpp>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace omarchy::plugins::manifest::detail {
namespace {
constexpr std::size_t kMaximumManifestBytes = 1024 * 1024;
constexpr std::size_t kMaximumStringBytes = 16 * 1024;
constexpr std::size_t kMaximumDepth = 32;
constexpr std::size_t kMaximumContainerEntries = 256;

void require(bool condition, std::string_view message) {
  if (!condition)
    throw std::runtime_error(std::string(message));
}
} // namespace

// The library owns syntax, Unicode decoding and tree construction. The callback
// enforces bounds before insertion, rejects duplicate keys, and normalizes ints.
nlohmann::json parse_manifest_json(std::string_view input) {
  using Json = nlohmann::json;
  require(input.size() <= kMaximumManifestBytes, "manifest is too large");
  require(!input.starts_with("\xef\xbb\xbf"), "JSON byte order marks are unsupported");
  struct Frame {
    bool object;
    std::size_t entries = 0;
    std::set<std::string> keys;
  };
  std::vector<Frame> stack;
  using Event = Json::parse_event_t;
  try {
    return Json::parse(input.begin(), input.end(), [&](int depth, Event event, Json &value) {
      if (event == Event::object_end || event == Event::array_end) {
        stack.pop_back();
        return true;
      }
      if (event == Event::key) {
        const auto &key = value.get_ref<const std::string &>();
        require(key.size() <= kMaximumStringBytes, "JSON key is too long");
        auto &keys = stack.back().keys;
        require(keys.size() < kMaximumContainerEntries && keys.insert(key).second,
                "duplicate or excessive JSON object keys");
        return true;
      }
      require(depth <= static_cast<int>(kMaximumDepth), "JSON nesting is too deep");
      if (!stack.empty() && !stack.back().object)
        require(++stack.back().entries <= kMaximumContainerEntries, "JSON array is too large");
      if (event == Event::object_start || event == Event::array_start) {
        stack.push_back({.object = event == Event::object_start, .entries = 0, .keys = {}});
      } else {
        require(!value.is_number_float(), "non-integer JSON numbers are unsupported");
        if (value.is_number_unsigned()) {
          require(value.get<std::uint64_t>() <= INT64_MAX, "JSON integer is out of range");
          value = value.get<std::int64_t>();
        }
        if (value.is_string())
          require(value.get_ref<const std::string &>().size() <= kMaximumStringBytes,
                  "JSON string is too long");
      }
      return true;
    });
  } catch (const Json::exception &) {
    throw std::runtime_error("invalid JSON syntax or Unicode");
  }
}

} // namespace omarchy::plugins::manifest::detail
