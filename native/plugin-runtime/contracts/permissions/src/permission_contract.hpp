#pragma once

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace omarchy::plugins::permissions {

template <std::size_t Maximum> class BoundedString {
public:
  BoundedString() = default;
  explicit BoundedString(std::string_view value) { assign(value); }

  void assign(std::string_view value) {
    if (value.empty() || value.size() > Maximum ||
        value.find('\0') != std::string_view::npos) {
      throw std::runtime_error("bounded string has invalid length or NUL");
    }
    bytes_.fill(0);
    size_ = value.size();
    std::copy(value.begin(), value.end(), bytes_.begin());
  }

  [[nodiscard]] std::string_view view() const { return {bytes_.data(), size_}; }
  [[nodiscard]] std::size_t size() const { return size_; }
  auto operator<=>(const BoundedString &) const = default;

private:
  std::array<char, Maximum> bytes_{};
  std::size_t size_ = 0;
};

template <typename T, std::size_t Capacity> class FixedSet {
public:
  bool insert(T value) {
    const auto position =
        std::lower_bound(values_.begin(), values_.begin() + size_, value);
    if (position != values_.begin() + size_ && *position == value)
      return false;
    if (size_ == Capacity)
      throw std::runtime_error("fixed set is full");
    std::move_backward(position, values_.begin() + size_,
                       values_.begin() + size_ + 1);
    *position = std::move(value);
    ++size_;
    return true;
  }
  [[nodiscard]] bool contains(const T &value) const {
    return std::binary_search(values_.begin(), values_.begin() + size_, value);
  }
  [[nodiscard]] std::span<const T> values() const {
    return {values_.data(), size_};
  }
  [[nodiscard]] std::size_t size() const { return size_; }
  bool operator==(const FixedSet &other) const {
    return size_ == other.size_ &&
           std::equal(values_.begin(), values_.begin() + size_,
                      other.values_.begin());
  }

private:
  std::array<T, Capacity> values_{};
  std::size_t size_ = 0;
};

template <typename T, std::size_t Capacity> class FixedVector {
public:
  FixedVector() { values_.reserve(Capacity); }

  void push_back(T value) {
    if (values_.size() == Capacity)
      throw std::runtime_error("fixed vector is full");
    values_.push_back(std::move(value));
  }
  [[nodiscard]] std::span<const T> values() const { return values_; }
  [[nodiscard]] std::span<T> values() { return values_; }
  [[nodiscard]] std::size_t size() const { return values_.size(); }
  [[nodiscard]] bool empty() const { return values_.empty(); }
  T &operator[](std::size_t index) {
    if (index >= values_.size())
      throw std::runtime_error("fixed vector index out of range");
    return values_[index];
  }
  const T &operator[](std::size_t index) const {
    if (index >= values_.size())
      throw std::runtime_error("fixed vector index out of range");
    return values_[index];
  }

private:
  std::vector<T> values_;
};

using PluginId = BoundedString<128>;
using CapabilityId = BoundedString<128>;
using Digest = BoundedString<64>;

enum class UserDecision : std::uint8_t { grant, deny };
enum class DecisionActor : std::uint8_t {
  trusted_ui,
  interactive_cli
};

enum class GrantState : std::uint8_t { granted, denied, revoked };

struct ActivationBinding {
  PluginId plugin;
  Digest revision;
  Digest policy_fingerprint;
  std::uint64_t generation = 0;
  bool operator==(const ActivationBinding &) const = default;
};

enum class GrantDecisionCode : std::uint8_t {
  allowed,
  unknown_operation,
  capability_undeclared,
  ungranted,
  explicitly_denied,
  revoked,
  activation_mismatch,
  outside_scope,
  gesture_missing,
};

enum class AuditProducer : std::uint8_t {
  lifecycle,
  supervisor,
  broker,
  surface_host
};
bool valid_audit_producer(AuditProducer producer);
enum class AuditEvent : std::uint8_t {
  operation_decided,
  worker_started,
  worker_health,
  worker_crashed,
  worker_stopped,
  worker_disabled,
  operation_completed,
};
enum class AuditOutcome : std::uint8_t { allowed, denied, cancelled, failed };
enum class AuditMetric : std::uint8_t {
  request_bytes,
  response_bytes,
  item_count,
  duration_milliseconds,
  retry_after_seconds,
};

struct AuditMetadata {
  AuditMetric metric{};
  std::int64_t value = 0;
  bool operator==(const AuditMetadata &) const = default;
};

struct DynamicAuditIdentity {
  CapabilityId capability;
  std::uint32_t definition_generation = 0;
  Digest definition_digest;
  BoundedString<128> operation;
  std::uint64_t grant_epoch = 0;
  bool operator==(const DynamicAuditIdentity &) const = default;
};

// Rejected dynamic requests must not promote plugin-provided names into the
// trusted audit vocabulary. The broker records only a host-computed digest of
// the bounded invocation envelope until an installed definition and operation
// have both resolved.
struct DynamicAuditAttemptIdentity {
  Digest opaque_digest;
  bool operator==(const DynamicAuditAttemptIdentity &) const = default;
};

struct AuditDraft {
  AuditEvent event{};
  AuditOutcome outcome{};
  PluginId plugin;
  Digest revision;
  std::uint64_t generation = 0;
  std::uint64_t correlation = 0;
  std::optional<DynamicAuditIdentity> dynamic_operation;
  std::optional<DynamicAuditAttemptIdentity> dynamic_attempt;
  GrantDecisionCode decision = GrantDecisionCode::ungranted;
  FixedVector<AuditMetadata, 8> metadata;
};

struct AuditRecord : AuditDraft {
  std::uint64_t sequence = 0;
  std::uint64_t wall_seconds = 0;
  std::uint64_t monotonic_ns = 0;
  AuditProducer producer = AuditProducer::broker;
};

void validate_audit_draft(const AuditDraft &draft);

} // namespace omarchy::plugins::permissions
