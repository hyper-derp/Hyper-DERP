/// @file test_policy.cc
/// @brief Tests for routing policy evaluation.

#include <gtest/gtest.h>

#include <hd/policy/policy.h>

namespace hd::policy {
namespace {

TEST(PolicyTest, PreferDirectDefaults) {
  hd::sdk::ClientConfig cfg;
  cfg.default_routing = Intent::PreferDirect;
  cfg.allow_upgrade = true;
  cfg.allow_downgrade = true;

  hd::sdk::TunnelOptions opts;
  auto d = Evaluate(cfg, opts);

  EXPECT_EQ(d.intent, Intent::PreferDirect);
  EXPECT_TRUE(d.try_ice);
  EXPECT_FALSE(d.require_direct);
  EXPECT_TRUE(d.allow_upgrade);
  EXPECT_TRUE(d.allow_downgrade);
}

TEST(PolicyTest, RequireRelay) {
  hd::sdk::ClientConfig cfg;
  cfg.default_routing = Intent::PreferDirect;

  hd::sdk::TunnelOptions opts;
  opts.routing = Intent::RequireRelay;

  auto d = Evaluate(cfg, opts);

  EXPECT_EQ(d.intent, Intent::RequireRelay);
  EXPECT_FALSE(d.try_ice);
  EXPECT_FALSE(d.require_direct);
  EXPECT_FALSE(d.allow_upgrade);
}

TEST(PolicyTest, RequireDirect) {
  hd::sdk::ClientConfig cfg;
  cfg.default_routing = Intent::PreferDirect;

  hd::sdk::TunnelOptions opts;
  opts.routing = Intent::RequireDirect;

  auto d = Evaluate(cfg, opts);

  EXPECT_EQ(d.intent, Intent::RequireDirect);
  EXPECT_TRUE(d.try_ice);
  EXPECT_TRUE(d.require_direct);
}

TEST(PolicyTest, OverrideUpgrade) {
  hd::sdk::ClientConfig cfg;
  cfg.allow_upgrade = true;

  hd::sdk::TunnelOptions opts;
  opts.allow_upgrade = false;

  auto d = Evaluate(cfg, opts);
  EXPECT_FALSE(d.allow_upgrade);
}

TEST(PolicyTest, ReasonPassthrough) {
  hd::sdk::ClientConfig cfg;
  hd::sdk::TunnelOptions opts;
  opts.reason = "compliance-zone-A";

  auto d = Evaluate(cfg, opts);
  EXPECT_EQ(d.reason, "compliance-zone-A");
}

}  // namespace
}  // namespace hd::policy
