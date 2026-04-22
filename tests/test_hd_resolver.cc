/// @file test_hd_resolver.cc
/// @brief Unit tests for the HD routing policy resolver.

#include "hyper_derp/hd_resolver.h"

#include <gtest/gtest.h>

namespace hyper_derp {
namespace {

HdLayerView OpenLayer() { return HdLayerView{}; }

TEST(HdResolverTest, PreferDirectDefaultsToDirect) {
  HdClientView c;
  c.intent = HdIntent::kPreferDirect;
  auto d = HdResolve(OpenLayer(), OpenLayer(),
                     OpenLayer(), OpenLayer(), c,
                     HdCapability{}, 0);
  EXPECT_EQ(d.mode, HdConnMode::kDirect);
  EXPECT_EQ(d.deny_reason, HdDenyReason::kNone);
}

TEST(HdResolverTest, PreferDirectNoNatFallsBack) {
  HdClientView c;
  c.intent = HdIntent::kPreferDirect;
  HdCapability cap;
  cap.can_direct = false;
  auto d = HdResolve(OpenLayer(), OpenLayer(),
                     OpenLayer(), OpenLayer(), c, cap, 0);
  EXPECT_EQ(d.mode, HdConnMode::kRelayed);
}

TEST(HdResolverTest, RequireDirectNoNatDenies) {
  HdClientView c;
  c.intent = HdIntent::kRequireDirect;
  HdCapability cap;
  cap.can_direct = false;
  auto d = HdResolve(OpenLayer(), OpenLayer(),
                     OpenLayer(), OpenLayer(), c, cap, 0);
  EXPECT_EQ(d.mode, HdConnMode::kDenied);
  EXPECT_EQ(d.deny_reason,
            HdDenyReason::kNatIncompatible);
}

TEST(HdResolverTest, RequireRelay) {
  HdClientView c;
  c.intent = HdIntent::kRequireRelay;
  auto d = HdResolve(OpenLayer(), OpenLayer(),
                     OpenLayer(), OpenLayer(), c,
                     HdCapability{}, 0);
  EXPECT_EQ(d.mode, HdConnMode::kRelayed);
}

TEST(HdResolverTest, PreferRelay) {
  HdClientView c;
  c.intent = HdIntent::kPreferRelay;
  auto d = HdResolve(OpenLayer(), OpenLayer(),
                     OpenLayer(), OpenLayer(), c,
                     HdCapability{}, 0);
  EXPECT_EQ(d.mode, HdConnMode::kRelayed);
}

TEST(HdResolverTest, PeerLayerForbidsDirect) {
  HdLayerView peer;
  peer.allowed = kModeRelayed;
  HdClientView c;
  c.intent = HdIntent::kPreferDirect;
  auto d = HdResolve(OpenLayer(), OpenLayer(),
                     OpenLayer(), peer, c,
                     HdCapability{}, 0);
  EXPECT_EQ(d.mode, HdConnMode::kRelayed);
}

TEST(HdResolverTest, PeerPinsRequireRelay) {
  HdLayerView peer;
  peer.pinned_intent = HdIntent::kRequireRelay;
  HdClientView c;
  c.intent = HdIntent::kRequireDirect;
  auto d = HdResolve(OpenLayer(), OpenLayer(),
                     OpenLayer(), peer, c,
                     HdCapability{}, 0);
  // Peer pin wins — client wanted direct, peer forces
  // relay.
  EXPECT_EQ(d.mode, HdConnMode::kRelayed);
  EXPECT_EQ(d.resolved_intent,
            HdIntent::kRequireRelay);
}

TEST(HdResolverTest, FleetPinTrumpsPeerPin) {
  HdLayerView fleet;
  fleet.pinned_intent = HdIntent::kRequireDirect;
  HdLayerView peer;
  peer.pinned_intent = HdIntent::kRequireRelay;
  HdClientView c;
  auto d = HdResolve(fleet, OpenLayer(), OpenLayer(),
                     peer, c, HdCapability{}, 0);
  EXPECT_EQ(d.mode, HdConnMode::kDirect);
  EXPECT_EQ(d.resolved_intent,
            HdIntent::kRequireDirect);
}

TEST(HdResolverTest, FleetForbidsDirectAndRelayed) {
  HdLayerView fleet;
  fleet.allowed = 0;
  HdClientView c;
  auto d = HdResolve(fleet, OpenLayer(), OpenLayer(),
                     OpenLayer(), c, HdCapability{}, 0);
  EXPECT_EQ(d.mode, HdConnMode::kDenied);
  EXPECT_EQ(d.deny_reason,
            HdDenyReason::kPolicyForbids);
}

TEST(HdResolverTest, CrossRelayNotImplemented) {
  HdClientView c;
  auto d = HdResolve(OpenLayer(), OpenLayer(),
                     OpenLayer(), OpenLayer(), c,
                     HdCapability{}, 42);
  EXPECT_EQ(d.mode, HdConnMode::kDenied);
  EXPECT_EQ(d.deny_reason,
            HdDenyReason::kFleetRoutingNotImplemented);
}

TEST(HdResolverTest, AllowUpgradeDowngradeFlowThrough) {
  HdClientView c;
  c.allow_upgrade = false;
  c.allow_downgrade = false;
  auto d = HdResolve(OpenLayer(), OpenLayer(),
                     OpenLayer(), OpenLayer(), c,
                     HdCapability{}, 0);
  EXPECT_FALSE(d.allow_upgrade);
  EXPECT_FALSE(d.allow_downgrade);
}

TEST(HdResolverTest, ExhaustiveIntentCapabilityMatrix) {
  struct Case {
    HdIntent intent;
    bool can_direct;
    HdConnMode expected_mode;
    HdDenyReason expected_deny;
  };
  const Case cases[] = {
      {HdIntent::kPreferDirect, true,
       HdConnMode::kDirect, HdDenyReason::kNone},
      {HdIntent::kPreferDirect, false,
       HdConnMode::kRelayed, HdDenyReason::kNone},
      {HdIntent::kRequireDirect, true,
       HdConnMode::kDirect, HdDenyReason::kNone},
      {HdIntent::kRequireDirect, false,
       HdConnMode::kDenied,
       HdDenyReason::kNatIncompatible},
      {HdIntent::kPreferRelay, true,
       HdConnMode::kRelayed, HdDenyReason::kNone},
      {HdIntent::kPreferRelay, false,
       HdConnMode::kRelayed, HdDenyReason::kNone},
      {HdIntent::kRequireRelay, true,
       HdConnMode::kRelayed, HdDenyReason::kNone},
      {HdIntent::kRequireRelay, false,
       HdConnMode::kRelayed, HdDenyReason::kNone},
  };
  for (const auto& tc : cases) {
    HdClientView c;
    c.intent = tc.intent;
    HdCapability cap;
    cap.can_direct = tc.can_direct;
    auto d = HdResolve(OpenLayer(), OpenLayer(),
                       OpenLayer(), OpenLayer(), c, cap,
                       0);
    EXPECT_EQ(d.mode, tc.expected_mode)
        << "intent=" << static_cast<int>(tc.intent)
        << " can_direct=" << tc.can_direct;
    EXPECT_EQ(d.deny_reason, tc.expected_deny);
  }
}

TEST(HdPeerPolicyMergeTest, OverrideClientWins) {
  HdPeerPolicy pol;
  pol.has_pin = true;
  pol.override_client = true;
  pol.pinned_intent = HdIntent::kRequireRelay;
  auto eff = HdApplyPeerPolicyIntent(
      pol, HdIntent::kRequireDirect);
  EXPECT_EQ(eff, HdIntent::kRequireRelay);
}

TEST(HdPeerPolicyMergeTest, NoPinLeavesWireIntentAlone) {
  HdPeerPolicy pol;
  pol.has_pin = false;
  pol.override_client = true;
  auto eff = HdApplyPeerPolicyIntent(
      pol, HdIntent::kPreferDirect);
  EXPECT_EQ(eff, HdIntent::kPreferDirect);
}

TEST(HdPeerPolicyMergeTest, NarrowingPinRestrictsAllowed) {
  HdPeerPolicy pol;
  pol.has_pin = true;
  pol.override_client = false;
  pol.pinned_intent = HdIntent::kRequireRelay;
  auto view = HdBuildPeerView(pol, HdIntent::kPreferDirect);
  EXPECT_EQ(view.allowed, kModeRelayed);
  EXPECT_FALSE(view.pinned_intent.has_value());
}

TEST(HdPeerPolicyMergeTest, OverridePinSetsResolverPin) {
  HdPeerPolicy pol;
  pol.has_pin = true;
  pol.override_client = true;
  pol.pinned_intent = HdIntent::kRequireRelay;
  // After override, the "effective" intent is the pin
  // itself; HdBuildPeerView then exposes it via
  // pinned_intent in the HdLayerView.
  auto view = HdBuildPeerView(pol, HdIntent::kRequireRelay);
  ASSERT_TRUE(view.pinned_intent.has_value());
  EXPECT_EQ(*view.pinned_intent, HdIntent::kRequireRelay);
}

TEST(HdPeerPolicyMergeTest, EndToEndComplianceScenario) {
  // Camera is pinned to require_relay with override;
  // client asks for require_direct. Expected: Relayed,
  // not Denied, and not Direct.
  HdPeerPolicy camera_pol;
  camera_pol.has_pin = true;
  camera_pol.override_client = true;
  camera_pol.pinned_intent = HdIntent::kRequireRelay;

  HdClientView client;
  client.intent = HdIntent::kRequireDirect;

  HdIntent eff = HdApplyPeerPolicyIntent(
      camera_pol, client.intent);
  // Simulating A's side: client intent replaced by pin.
  client.intent = eff;

  HdLayerView peer_view = HdBuildPeerView(camera_pol, eff);
  HdCapability cap;
  cap.can_direct = true;
  auto d = HdResolve(HdLayerView{}, HdLayerView{},
                     HdLayerView{}, peer_view, client,
                     cap, 0);
  EXPECT_EQ(d.mode, HdConnMode::kRelayed);
}

}  // namespace
}  // namespace hyper_derp
