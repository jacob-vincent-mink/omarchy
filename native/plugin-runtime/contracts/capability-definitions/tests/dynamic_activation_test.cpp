#include "../../../tests/support/gesture_clock.hpp"
#include "../../../tests/support/authenticated_broker_fixture.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>

namespace {
using namespace omarchy::plugins::definitions;
namespace permissions = omarchy::plugins::permissions;
void require(bool value,std::string_view message){if(!value)throw std::runtime_error(std::string(message));}
Digest digest(char value){return Digest(std::string(64,value));}
bool adapter_dispatch(const AuthorizedDynamicRequest &request,std::span<std::byte> response,std::size_t &written,void *context) noexcept {
 auto &calls=*static_cast<int*>(context);if((request.operation!="read"&&request.operation!="write")||request.demand_scope!="wide"||request.payload.size()!=1||response.empty()||request.authorization.binding.plugin.view()!="org.example.dynamic"||request.authorization.binding.generation!=5||request.authorization.definition.canonical_name.view()!="local.status"||request.authorization.grant_epoch!=8)return false;++calls;response[0]=request.payload[0];written=1;return true;}
CapabilityDefinition definition(){CapabilityDefinition d{.canonical_name=Name("local.status"),.authority_identity=Name("local.status-v1"),.enforcement_family=EnforcementFamily::network_fetch,.display_category_id=Name("local.services"),.display_category_label=Label("Local services"),.title=Label("Read selected status"),.risk_text=Label("Sends a bounded request to a selected service"),.risk=RiskLevel::moderate,.revocation=RevocationPolicy::cancel_inflight,.adapter={.adapter_class=Name("status-adapter"),.contract_digest=digest('a'),.abi_version=1},.operations={}};d.operations.insert({.name=Name("read"),.label=Label("Read status")});d.operations.insert({.name=Name("write"),.label=Label("Change status"),.mutating=true,.requires_fresh_gesture=true});return d;}
permissions::ActivationBinding binding(std::uint64_t generation=5){return {.plugin=permissions::PluginId("org.example.dynamic"),.revision=digest('b'),.policy_fingerprint=digest('c'),.generation=generation};}
}

void dynamic_activation_tests(){
 TrustedDefinitionRegistry registry;auto def=definition();require(registry.install(def,3),"definition install failed");auto resolved=registry.find("local.status");require(resolved.has_value(),"definition missing");
 DynamicRevisionGrant revision{.binding=binding(),.request={.definition={.canonical_name=Name("local.status"),.definition_generation=3,.definition_digest=resolved->digest},.operations={},.scope=CanonicalScope("wide"),.required=true},.grant={.operations={},.state=permissions::GrantState::granted,.epoch=8}};
 revision.request.operations.insert(Name("read"));revision.grant.operations.insert(Name("read"));require(review_dynamic_grant(registry,revision),"review rejected exact grant");
 std::array<std::byte,16384> persisted{};std::size_t persisted_size=0;require(encode_dynamic_grant(revision,persisted,persisted_size),"grant persistence encode failed");DynamicRevisionGrant restored;require(decode_dynamic_grant(std::span(persisted).first(persisted_size),restored)&&review_dynamic_grant(registry,restored),"persisted grant did not review");
 require(restored.binding==revision.binding&&restored.request==revision.request&&restored.grant.operations==revision.grant.operations&&restored.grant.epoch==8,"persisted dynamic grant did not reconstruct exactly");
 auto undefined_operation=restored;undefined_operation.request.operations.insert(Name("admin"));undefined_operation.grant.operations.insert(Name("admin"));require(!review_dynamic_grant(registry,undefined_operation),"persisted undefined operation passed review");
 auto stale_definition=restored;stale_definition.request.definition.definition_generation=4;require(!review_dynamic_grant(registry,stale_definition),"stale persisted definition generation passed review");
 auto zero_epoch=restored;zero_epoch.grant.epoch=0;require(!review_dynamic_grant(registry,zero_epoch),"zero persisted grant epoch passed review");
 auto empty_granted=restored;empty_granted.grant.operations={};require(!review_dynamic_grant(registry,empty_granted),"empty granted operation set passed review");
 auto empty_revoked=restored;empty_revoked.grant.state=permissions::GrantState::revoked;empty_revoked.grant.operations={};require(!review_dynamic_grant(registry,empty_revoked),"empty revoked operation set passed review");
 auto denied_exact=restored;denied_exact.grant.state=permissions::GrantState::denied;require(review_dynamic_grant(registry,denied_exact),"exact denied operation set failed review");
 auto denied_empty=denied_exact;denied_empty.grant.operations={};require(!review_dynamic_grant(registry,denied_empty),"denied persisted grant dropped requested operations");
 auto partial_required=restored;partial_required.request.operations.insert(Name("write"));require(review_dynamic_grant(registry,partial_required),"nonempty partial required grant failed review");
 auto partial_revoked=partial_required;partial_revoked.grant.state=permissions::GrantState::revoked;require(review_dynamic_grant(registry,partial_revoked),"revocation lost a nonempty partial operation set");
 auto malformed_state=restored;malformed_state.grant.state=static_cast<permissions::GrantState>(99);require(!review_dynamic_grant(registry,malformed_state),"malformed dynamic grant state passed review");
 const auto reference_bytes = std::string("\0\x0c", 2) + "local.status" +
                              std::string("\0\0\0\x03\0\x40", 6) +
                              std::string(resolved->digest.view());
 const auto grant_prefix = std::string("OMDGRNT\x02\0\x13", 10) +
                           "org.example.dynamic" + std::string("\0\x40", 2) +
                           std::string(64, 'b') + std::string("\0\x40", 2) +
                           std::string(64, 'c') + std::string("\0\0\0\0\0\0\0\x05", 8) +
                           reference_bytes + std::string("\0\x04wide\x01\x01\0\x04read", 14);
 const auto grant_suffix = std::string("\0\0\0\0\0\0\0\0\x08\x01\0\x04read", 16);
 const auto grant_golden = grant_prefix + grant_suffix;
 require(std::ranges::equal(std::span(persisted).first(persisted_size),
                            std::as_bytes(std::span(grant_golden))),
         "grant record duplicated its request identity or scope");
 auto legacy = grant_prefix + reference_bytes + std::string("\0\x04wide", 6) + grant_suffix;
 legacy[7] = '\x01';
 DynamicRevisionGrant rejected;
 require(!decode_dynamic_grant(std::as_bytes(std::span(legacy)), rejected),
         "legacy duplicated grant metadata was accepted");
 for (const auto version : {0, 1, 3, 255}) {
   auto unsupported = persisted;
   unsupported[7] = static_cast<std::byte>(version);
   require(!decode_dynamic_grant(std::span(unsupported).first(persisted_size), rejected),
           "unsupported dynamic grant record version decoded");
 }
 for (std::size_t size = 0; size < persisted_size; ++size)
   require(!decode_dynamic_grant(std::span(persisted).first(size), rejected),
           "truncated dynamic grant record decoded");
 persisted[persisted_size]=std::byte{0};require(!decode_dynamic_grant(std::span(persisted).first(persisted_size+1),rejected),"dynamic grant record with trailing data decoded");
 const std::array payload{std::byte{0x2a}};DynamicInvocation invocation{.definition=revision.request.definition,.operation=Name("read"),.gesture={},.payload=payload};std::array<std::byte,kMaximumDynamicEnvelopeBytes> envelope{};std::size_t envelope_size=0;require(encode_dynamic_invocation(invocation,envelope,envelope_size),"invoke encode failed");const auto valid_envelope_size=envelope_size;
 const auto golden = std::string("OMDINVK\x02\0\x0c", 10) + "local.status" +
                     std::string("\0\0\0\x03\0\x40", 6) +
                     std::string(resolved->digest.view()) +
                     std::string("\0\x04read\0\0\0\0\x01*", 12);
 require(std::ranges::equal(std::span(envelope).first(envelope_size),
                            std::as_bytes(std::span(golden))),
         "dynamic invocation field widths or byte order changed");
 for (std::size_t size = 0; size < envelope_size; ++size) {
   DynamicInvocation truncated;
   require(!decode_dynamic_invocation(std::span(envelope).first(size), truncated),
           "truncated dynamic invocation decoded");
 }
 int calls=0;
 DynamicAdapter adapter{.binding=def.adapter,.dispatch=[&calls](const auto &request, auto response, std::size_t &written) noexcept { return adapter_dispatch(request,response,written,&calls); }};
 std::array<std::byte,8> response{};
 std::size_t written=0;
 namespace runtime = omarchy::plugin_runtime::runtime;
 using namespace omarchy::plugin_runtime::host_session;
 namespace support = omarchy::plugin_runtime::test_support;
 using Clock = omarchy::plugin_runtime::test_support::GestureClock<1>;
 auto run=[&](const DynamicRevisionGrant &r,
              const permissions::ActivationBinding &b, const DynamicAdapter &a,
              std::span<const std::byte> bytes, bool fresh = false) {
   support::AdmittedBrokerFixture fixture(r.binding, registry,
                                           {{.grant=r,.adapter=a}});
   runtime::GestureEligibilityLatch gestures(std::make_shared<Clock>());
   if (fresh)
     require(gestures.arm(b, {1, b.generation, 1}), "test gesture arm failed");
   if (b != r.binding) {
     support::AdmittedBrokerFixture foreign(b, registry, {});
     auto admitted = foreign.admission.admit(
         {omarchy::plugin_runtime::broker::kDynamicInvokeMessage, 1, bytes});
     require(static_cast<bool>(admitted), "foreign admission failed");
     const auto result = fixture.broker.dispatch(
         std::move(*admitted.request), response, &gestures);
     require(result.fatal() == DispatchFatal::identity_mismatch,
             "foreign broker authority was not rejected");
     return std::optional<ReplyKind>{};
   }
   auto result = fixture.dispatch(bytes, response, &gestures);
   written = result.provider_response_bytes();
   if (result.state() == TransactionState::fatal)
     return std::optional<ReplyKind>{};
   const auto kind = result.reply_kind();
   require(fixture.broker.commit_sent(std::move(result)), "reply settlement failed");
   return std::optional(kind);
 };

 require(run(restored,binding(),adapter,std::span(envelope).first(valid_envelope_size))==ReplyKind::result&&calls==1&&written==1,"end-to-end dynamic dispatch failed");
 auto missing_adapter=adapter;missing_adapter.dispatch=nullptr;
 bool missing_rejected=false;
 try { (void)run(restored,binding(),missing_adapter,std::span(envelope).first(valid_envelope_size)); }
 catch (const std::runtime_error &) { missing_rejected=true; }
 require(missing_rejected&&calls==1,"missing adapter was admitted");
 auto mutated=envelope;mutated[12]^=std::byte{1};require(run(restored,binding(),adapter,std::span(mutated).first(envelope_size))!=ReplyKind::result&&calls==1,"spoofed definition name dispatched");
 auto wrong_digest=invocation;wrong_digest.definition.definition_digest=digest('f');require(encode_dynamic_invocation(wrong_digest,mutated,envelope_size)&&run(restored,binding(),adapter,std::span(mutated).first(envelope_size))==ReplyKind::denied,"spoofed definition digest dispatched");
 auto wrong_generation=invocation;wrong_generation.definition.definition_generation=4;require(encode_dynamic_invocation(wrong_generation,mutated,envelope_size)&&run(restored,binding(),adapter,std::span(mutated).first(envelope_size))==ReplyKind::denied,"spoofed definition generation dispatched");
 auto wrong_adapter=adapter;wrong_adapter.binding.contract_digest=digest('e');require(run(restored,binding(),wrong_adapter,std::span(envelope).first(envelope_size))==ReplyKind::denied,"spoofed adapter dispatched");
 auto undeclared=invocation;undeclared.operation=Name("write");undeclared.gesture=DynamicInvocation::GestureClaim{1,5,1};require(encode_dynamic_invocation(undeclared,mutated,envelope_size)&&run(restored,binding(),adapter,std::span(mutated).first(envelope_size))==ReplyKind::denied,"undeclared operation dispatched");
 auto gesture_revision=restored;gesture_revision.request.operations.insert(Name("write"));gesture_revision.grant.operations.insert(Name("write"));require(run(gesture_revision,binding(),adapter,std::span(mutated).first(envelope_size))==ReplyKind::denied&&calls==1,"mutation without fresh gesture dispatched");require(run(gesture_revision,binding(),adapter,std::span(mutated).first(envelope_size),true)==ReplyKind::result&&calls==2,"fresh gesture did not authorize declared mutation");

 auto denied=restored;denied.grant.state=permissions::GrantState::denied;require(run(denied,binding(),adapter,std::span(envelope).first(valid_envelope_size))==ReplyKind::denied,"denied grant dispatched");auto revoked=restored;revoked.grant.state=permissions::GrantState::revoked;require(run(revoked,binding(),adapter,std::span(envelope).first(valid_envelope_size))==ReplyKind::denied,"revoked grant dispatched");
 require(run(restored,binding(6),adapter,std::span(envelope).first(valid_envelope_size))==std::nullopt,"stale plugin generation dispatched");auto stale_revision=binding();stale_revision.revision=digest('d');require(run(restored,stale_revision,adapter,std::span(envelope).first(valid_envelope_size))==std::nullopt,"stale plugin revision dispatched");
 TrustedDefinitionRegistry empty;
 require(!review_dynamic_grant(empty,restored),"unknown definition passed review");

}
