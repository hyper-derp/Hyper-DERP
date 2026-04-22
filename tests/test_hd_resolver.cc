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

}  // namespace
}  // namespace hyper_derp
