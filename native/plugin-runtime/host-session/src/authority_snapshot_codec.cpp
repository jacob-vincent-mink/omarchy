#include "authority_snapshot_codec.hpp"
#include "omarchy/plugin_runtime/byte_reader.hpp"

#include "manifest_contract.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>

namespace omarchy::plugin_runtime::host_session::authority_snapshot_codec {
namespace {

constexpr std::size_t kMaximumDynamicGrants = 128;
constexpr std::array<std::byte, 8> kSnapshotMagic{
    std::byte{'O'}, std::byte{'M'}, std::byte{'G'}, std::byte{'R'},
    std::byte{'A'}, std::byte{'N'}, std::byte{'T'}, std::byte{3}};
constexpr std::array<std::byte, 8> kSlotsMagic{
    std::byte{'O'}, std::byte{'M'}, std::byte{'S'}, std::byte{'L'},
    std::byte{'O'}, std::byte{'T'}, std::byte{'S'}, std::byte{1}};

struct Writer {
  std::vector<std::byte> bytes;

  bool raw(std::span<const std::byte> value) {
    if (value.size() > kMaximumEncodedAuthorityBytes - bytes.size())
      return false;
    bytes.insert(bytes.end(), value.begin(), value.end());
    return true;
  }
  template <typename T> bool integer(T value) {
    std::array<std::byte, sizeof(T)> encoded{};
    omarchy::plugin_runtime::big_endian::put<T>(encoded, 0, value);
    return raw(encoded);
  }
  bool text(std::string_view value) {
    return !value.empty() && value.size() <= UINT16_MAX &&
           integer<std::uint16_t>(static_cast<std::uint16_t>(value.size())) &&
           raw(std::as_bytes(std::span(value.data(), value.size())));
  }
};

using Reader = omarchy::plugin_runtime::ByteReader;

bool write_reference(Writer &writer,
                     const std::optional<AuthorityRevisionRef> &reference) {
  if (!writer.integer<std::uint8_t>(reference.has_value()))
    return false;
  return !reference || (writer.text(reference->snapshot_digest.view()) &&
                        writer.integer<std::uint64_t>(reference->generation));
}

bool read_reference(Reader &reader,
                    std::optional<AuthorityRevisionRef> &reference) {
  std::uint8_t present = 0;
  std::string_view text;
  if (!reader.integer(present) || present > 1)
    return false;
  if (!present) {
    reference.reset();
    return true;
  }
  try {
    AuthorityRevisionRef value;
    if (!reader.text(text))
      return false;
    value.snapshot_digest = permissions::Digest(text);
    if (!reader.integer(value.generation) || value.generation == 0)
      return false;
    reference = std::move(value);
    return true;
  } catch (...) {
    return false;
  }
}

} // namespace

bool encode_snapshot(const policy::GrantSnapshot &snapshot,
                     std::vector<std::byte> &output) {
  Writer writer;
  if (!writer.raw(kSnapshotMagic) ||
      !writer.text(snapshot.binding.plugin.view()) ||
      !writer.text(snapshot.binding.revision.view()) ||
      !writer.text(snapshot.binding.policy_fingerprint.view()) ||
      !writer.integer<std::uint64_t>(snapshot.binding.generation))
    return false;
  if (snapshot.dynamic_grants.size() > kMaximumDynamicGrants ||
      !writer.integer<std::uint8_t>(static_cast<std::uint8_t>(snapshot.dynamic_grants.size())))
    return false;
  std::array<std::byte, definitions::kMaximumDynamicEnvelopeBytes> encoded{};
  for (const auto &grant : snapshot.dynamic_grants) {
    std::size_t size = 0;
    if (!definitions::encode_dynamic_grant(grant, encoded, size) ||
        size > definitions::kMaximumDynamicEnvelopeBytes ||
        !writer.integer<std::uint32_t>(static_cast<std::uint32_t>(size)) ||
        !writer.raw(std::span(encoded).first(size)))
      return false;
  }
  output = std::move(writer.bytes);
  return true;
}

bool decode_snapshot(std::span<const std::byte> bytes,
                     policy::GrantSnapshot &snapshot) {
  Reader reader{bytes};
  std::span<const std::byte> magic;
  std::string_view text;
  std::uint8_t count = 0;
  snapshot = {};
  try {
    if (!reader.raw(kSnapshotMagic.size(), magic) ||
        !std::ranges::equal(magic, kSnapshotMagic) || !reader.text(text))
      return false;
    snapshot.binding.plugin = permissions::PluginId(text);
    if (!reader.text(text))
      return false;
    snapshot.binding.revision = permissions::Digest(text);
    if (!reader.text(text))
      return false;
    snapshot.binding.policy_fingerprint = permissions::Digest(text);
    if (!reader.integer(snapshot.binding.generation))
      return false;
    if (!reader.integer(count) || count > kMaximumDynamicGrants)
      return false;
    for (std::uint8_t index = 0; index < count; ++index) {
      std::uint32_t size = 0;
      std::span<const std::byte> encoded;
      definitions::DynamicRevisionGrant grant;
      if (!reader.integer(size) ||
          size > definitions::kMaximumDynamicEnvelopeBytes ||
          !reader.raw(size, encoded) ||
          !definitions::decode_dynamic_grant(encoded, grant))
        return false;
      snapshot.dynamic_grants.push_back(std::move(grant));
    }
  } catch (...) {
    return false;
  }
  return reader.offset == bytes.size();
}

bool complete_snapshot(const VerifiedRevision &verified,
                       const policy::GrantSnapshot &snapshot,
                       const definitions::TrustedDefinitionRegistry &registry) {
  try {
    if (snapshot.binding.plugin.view() != verified.manifest.id ||
        snapshot.binding.revision.view() != verified.tree_sha256 ||
        verified.request_sha256 !=
            plugins::manifest::requested_capability_fingerprint(
                verified.manifest.requests) ||
        snapshot.binding.generation == 0 ||
        snapshot.binding.policy_fingerprint.view() !=
            verified.request_sha256)
      return false;
    const auto requested = definitions::dynamic_requests_from_manifest(verified.manifest, registry);
    if (!requested || requested->size() != snapshot.dynamic_grants.size() ||
        requested->size() > kMaximumDynamicGrants)
      return false;
    for (std::size_t index = 0; index < requested->size(); ++index) {
      const auto &grant = snapshot.dynamic_grants[index];
      if (grant.binding != snapshot.binding ||
          grant.request != (*requested)[index] ||
          !definitions::review_dynamic_grant(registry, grant))
        return false;
      if (index > 0 && !(snapshot.dynamic_grants[index - 1]
                             .request.definition.canonical_name <
                         grant.request.definition.canonical_name))
        return false;
    }
    return true;
  } catch (...) {
    return false;
  }
}

AuthorityRevisionRef reference_for(const policy::GrantSnapshot &snapshot,
                                   std::string_view digest) {
  return {.snapshot_digest = permissions::Digest(digest),
          .generation = snapshot.binding.generation};
}

std::vector<std::byte> encode_slots(const AuthoritySlots &slots) {
  Writer writer;
  if (!writer.raw(kSlotsMagic) || !writer.integer<std::uint64_t>(slots.sequence) ||
      !writer.integer<std::uint64_t>(slots.generation_high_watermark) ||
      !write_reference(writer, slots.active) ||
      !write_reference(writer, slots.candidate))
    return {};
  return std::move(writer.bytes);
}

std::optional<AuthoritySlots> decode_slots(std::span<const std::byte> bytes) {
  Reader reader{bytes};
  std::span<const std::byte> magic;
  AuthoritySlots slots;
  if (!reader.raw(kSlotsMagic.size(), magic) ||
      !std::ranges::equal(magic, kSlotsMagic) || !reader.integer(slots.sequence) ||
      !reader.integer(slots.generation_high_watermark) ||
      !read_reference(reader, slots.active) ||
      !read_reference(reader, slots.candidate) || reader.offset != bytes.size())
    return std::nullopt;
  return slots;
}

} // namespace omarchy::plugin_runtime::host_session::authority_snapshot_codec
