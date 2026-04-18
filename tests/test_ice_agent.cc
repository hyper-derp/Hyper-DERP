/// @file test_ice_agent.cc
/// @brief Tests for the SDK ICE agent.

#include <arpa/inet.h>

#include <gtest/gtest.h>
#include <sodium.h>

#include <hd/ice/ice_agent.h>

namespace hd::ice {
namespace {

TEST(IceAgentTest, GatherHostCandidates) {
  ASSERT_GE(sodium_init(), 0);

  IceConfig cfg;
  // No STUN servers — just host candidates.
  cfg.local_port = 0;

  IceAgent agent(cfg);
  std::vector<Candidate> gathered;
  auto r = agent.GatherCandidates(
      [&](const std::vector<Candidate>& c) {
    gathered = c;
  });
  ASSERT_TRUE(r.has_value());
  // Should have at least one host candidate.
  EXPECT_GE(gathered.size(), 1u);
  if (!gathered.empty()) {
    EXPECT_EQ(gathered[0].type, Candidate::kHost);
    EXPECT_NE(gathered[0].ip, 0u);
  }
}

TEST(IceAgentTest, StunFdLifecycle) {
  IceConfig cfg;
  cfg.local_port = 0;
  IceAgent agent(cfg);

  // Before gathering, no fd.
  EXPECT_EQ(agent.StunFd(), -1);

  // Gather (no STUN servers, just host).
  agent.GatherCandidates(nullptr);
  // Still no fd without STUN servers.
  EXPECT_EQ(agent.StunFd(), -1);
}

TEST(IceAgentTest, NominationCallback) {
  IceConfig cfg;
  IceAgent agent(cfg);

  uint32_t nominated_ip = 0;
  uint16_t nominated_port = 0;
  agent.OnNomination([&](uint32_t ip, uint16_t port) {
    nominated_ip = ip;
    nominated_port = port;
  });

  Candidate c{};
  c.type = Candidate::kHost;
  c.ip = 0x0100000A;  // 10.0.0.1
  c.port = htons(51820);
  agent.AddRemoteCandidates({c});

  EXPECT_EQ(nominated_ip, 0x0100000Au);
  EXPECT_EQ(nominated_port, htons(51820));
}

}  // namespace
}  // namespace hd::ice
