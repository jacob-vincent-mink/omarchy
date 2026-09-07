#include "dynamic_activation.hpp"
#include "omarchy/plugin_runtime/byte_reader.hpp"

#include <algorithm>
#include <cstring>

namespace omarchy::plugins::definitions {
namespace {
constexpr std::array<std::byte, 8> kGrantMagic{
    std::byte{'O'}, std::byte{'M'}, std::byte{'D'}, std::byte{'G'},
    std::byte{'R'}, std::byte{'N'}, std::byte{'T'}, std::byte{2}};
constexpr std::array<std::byte, 8> kInvokeMagic{
    std::byte{'O'}, std::byte{'M'}, std::byte{'D'}, std::byte{'I'},
    std::byte{'N'}, std::byte{'V'}, std::byte{'K'}, std::byte{2}};

struct Writer {
  std::span<std::byte> bytes;
  std::size_t offset = 0;
  bool raw(std::span<const std::byte> value) {
    if (value.size() > bytes.size() - std::min(offset, bytes.size())) return false;
    std::copy(value.begin(), value.end(), bytes.begin() + offset);
    offset += value.size(); return true;
  }
  template <typename T> bool integer(T value) {
    std::array<std::byte, sizeof(T)> encoded{};
    omarchy::plugin_runtime::big_endian::put<T>(encoded, 0, value);
    return raw(encoded);
  }
  bool text(std::string_view value) {
    return value.size() <= UINT16_MAX && integer<std::uint16_t>(static_cast<std::uint16_t>(value.size())) &&
           raw(std::as_bytes(std::span(value.data(), value.size())));
  }
};

using Reader = omarchy::plugin_runtime::ByteReader;

bool write_reference(Writer &writer, const CapabilityReference &reference) {
  return writer.text(reference.canonical_name.view()) &&
         writer.integer<std::uint32_t>(reference.definition_generation) &&
         writer.text(reference.definition_digest.view());
}
bool read_reference(Reader &reader, CapabilityReference &reference) {
  std::string_view name,digest; std::uint32_t generation=0;
  if(!reader.text(name)||!reader.integer(generation)||!reader.text(digest))return false;
  try { reference={.canonical_name=Name(name),.definition_generation=generation,
                   .definition_digest=Digest(digest)}; } catch(...) { return false; }
  return generation>0;
}
} // namespace

bool valid_dynamic_grant_shape(const DynamicRevisionGrant &revision) {
  if (revision.grant.epoch == 0 ||
      static_cast<std::uint8_t>(revision.grant.state) >
          static_cast<std::uint8_t>(permissions::GrantState::revoked))
    return false;
  if (revision.grant.state == permissions::GrantState::denied)
    return revision.grant.operations == revision.request.operations;
  return revision.grant.operations.size() != 0 &&
         std::ranges::all_of(revision.grant.operations.values(), [&](const auto &op) {
           return revision.request.operations.contains(op);
         });
}

bool review_dynamic_grant(const TrustedDefinitionRegistry &registry,
                          const DynamicRevisionGrant &revision) {
  const auto resolved = registry.resolve(revision.request.definition);
  return resolved && valid_dynamic_grant_shape(revision) &&
         std::ranges::all_of(revision.request.operations.values(), [&](const auto &op) {
           return std::ranges::any_of(resolved->definition->operations.values(),
                                     [&](const auto &defined) { return defined.name == op; });
         });
}

bool encode_dynamic_grant(const DynamicRevisionGrant &revision,
                          std::span<std::byte> output, std::size_t &written) {
  written=0; Writer w{output};
  if(!w.raw(kGrantMagic)||!w.text(revision.binding.plugin.view())||
     !w.text(revision.binding.revision.view())||!w.text(revision.binding.policy_fingerprint.view())||
     !w.integer<std::uint64_t>(revision.binding.generation)||!write_reference(w,revision.request.definition)||
     !w.text(revision.request.scope.view())||!w.integer<std::uint8_t>(revision.request.required?1:0)||
     !w.integer<std::uint8_t>(static_cast<std::uint8_t>(revision.request.operations.size())))return false;
  for(const auto &op:revision.request.operations.values())if(!w.text(op.view()))return false;
  if(!w.integer<std::uint8_t>(static_cast<std::uint8_t>(revision.grant.state))||!w.integer<std::uint64_t>(revision.grant.epoch)||
     !w.integer<std::uint8_t>(static_cast<std::uint8_t>(revision.grant.operations.size())))return false;
  for(const auto &op:revision.grant.operations.values())if(!w.text(op.view()))return false;
  written=w.offset; return true;
}

bool decode_dynamic_grant(std::span<const std::byte> input,
                          DynamicRevisionGrant &output) {
  output={}; Reader r{input}; std::span<const std::byte> magic;
  std::string_view plugin,revision,policy,scope; std::uint8_t count=0,value=0;
  try {
    if(!r.raw(8,magic)||!std::equal(magic.begin(),magic.end(),kGrantMagic.begin())||
       !r.text(plugin)||!r.text(revision)||!r.text(policy)||!r.integer(output.binding.generation)||
       !read_reference(r,output.request.definition)||!r.text(scope))return false;
    output.binding.plugin=permissions::PluginId(plugin); output.binding.revision=Digest(revision);
    output.binding.policy_fingerprint=Digest(policy); output.request.scope=CanonicalScope(scope);
    if (!r.integer(value) || value > 1 || !r.integer(count) || count > 16)
      return false;
    output.request.required = value == 1;
    for(std::uint8_t i=0;i<count;++i){std::string_view op;if(!r.text(op)||!output.request.operations.insert(Name(op)))return false;}
    if(!r.integer(value)||value>static_cast<std::uint8_t>(permissions::GrantState::revoked)||!r.integer(output.grant.epoch)||!r.integer(count)||count>16)return false;
    output.grant.state=static_cast<permissions::GrantState>(value);
    for(std::uint8_t i=0;i<count;++i){std::string_view op;if(!r.text(op)||!output.grant.operations.insert(Name(op)))return false;}
  } catch(...) { return false; }
  return r.offset==input.size();
}

bool encode_dynamic_invocation(const DynamicInvocation &invocation,
                               std::span<std::byte> output,std::size_t &written){
  written=0;if(invocation.payload.size()>kMaximumDynamicPayloadBytes)return false;Writer w{output};
  if(!w.raw(kInvokeMagic)||!write_reference(w,invocation.definition)||!w.text(invocation.operation.view())||
     !w.integer<std::uint8_t>(invocation.gesture ? 1 : 0))return false;
  if(invocation.gesture&&(!w.integer<std::uint64_t>(invocation.gesture->surface_id)||
     !w.integer<std::uint64_t>(invocation.gesture->surface_generation)||
     !w.integer<std::uint64_t>(invocation.gesture->input_sequence)))return false;
  if(!w.integer<std::uint32_t>(static_cast<std::uint32_t>(invocation.payload.size()))||!w.raw(invocation.payload))return false;
  written=w.offset;return true;
}
bool decode_dynamic_invocation(std::span<const std::byte> input,DynamicInvocation &output){
  output={};if(input.size()>kMaximumDynamicEnvelopeBytes)return false;Reader r{input};std::span<const std::byte> magic,payload;std::string_view op;std::uint32_t size=0;std::uint8_t gesture=0;
  try{if(!r.raw(8,magic)||!std::equal(magic.begin(),magic.end(),kInvokeMagic.begin())||!read_reference(r,output.definition)||!r.text(op)||!r.integer(gesture)||gesture>1)return false;
  if(gesture){DynamicInvocation::GestureClaim claim;if(!r.integer(claim.surface_id)||!r.integer(claim.surface_generation)||!r.integer(claim.input_sequence)||claim.surface_id==0||claim.surface_generation==0||claim.input_sequence==0)return false;output.gesture=claim;}
  if(!r.integer(size)||size>kMaximumDynamicPayloadBytes||!r.raw(size,payload)||r.offset!=input.size())return false;
  output.operation=Name(op);output.payload=payload;}catch(...){return false;}return true;
}

} // namespace omarchy::plugins::definitions
