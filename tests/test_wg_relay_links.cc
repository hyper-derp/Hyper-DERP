/// @file test_wg_relay_links.cc
/// @brief Integration tests for the wg-relay link table — in
///        particular the iteration-1 invariant "each peer is
///        in at most one link" and the distinct error codes
///        the daemon surfaces when it's hit.
///
/// Drives `WgRelayLinkAddDetail` directly (no einheit channel
/// in the loop) so a star-topology config landing on the
/// limit produces a `kWgLinkLimitExceeded` outcome rather
/// than a silent drop.

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "hyper_derp/server.h"
#include "hyper_derp/wg_relay.h"

namespace hyper_derp {
namespace {

uint16_t PickFreeUdpPort() {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return 0;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr),
            sizeof(addr)) < 0) {
    close(fd);
    return 0;
  }
  socklen_t len = sizeof(addr);
  getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
  uint16_t p = ntohs(addr.sin_port);
  close(fd);
  return p;
}

class WgRelayLinkTest : public ::testing::Test {
 protected:
  void SetUp() override {
    cfg_.port = PickFreeUdpPort();
    ASSERT_NE(cfg_.port, 0);
    // Four peers — operator-distinct names with placeholder
    // endpoints. The link table operates on names; the
    // forwarder doesn't care about endpoint reachability for
    // these tests.
    for (auto name : {"alice", "bob", "carol", "dave"}) {
      WgRelayConfig::PeerEntry pe;
      pe.name = name;
      pe.endpoint = "127.0.0.1:1";
      cfg_.peers.push_back(std::move(pe));
    }
    relay_ = WgRelayStart(cfg_);
    ASSERT_NE(relay_, nullptr);
  }

  void TearDown() override {
    if (relay_) WgRelayStop(relay_);
  }

  WgRelayConfig cfg_;
  WgRelay* relay_ = nullptr;
};

// First link in any pairing succeeds.
TEST_F(WgRelayLinkTest, FirstLinkOk) {
  EXPECT_EQ(WgRelayLinkAddDetail(relay_, "alice", "bob"),
            kWgLinkOk);
}

// Star topology (alice ↔ {bob, carol, dave}) is exactly the
// runner config from the brief. The first link must succeed
// and the second + third must be rejected with the distinct
// link_limit_exceeded outcome — not a generic "link failed".
TEST_F(WgRelayLinkTest, StarTopologyRejectsExtraLinks) {
  EXPECT_EQ(WgRelayLinkAddDetail(relay_, "alice", "bob"),
            kWgLinkOk);
  EXPECT_EQ(WgRelayLinkAddDetail(relay_, "alice", "carol"),
            kWgLinkLimitExceeded);
  EXPECT_EQ(WgRelayLinkAddDetail(relay_, "alice", "dave"),
            kWgLinkLimitExceeded);
}

// Disjoint pairs (alice↔bob, carol↔dave) — the runner's
// fallback if it hits the limit — must succeed entirely.
TEST_F(WgRelayLinkTest, DisjointPairsBothOk) {
  EXPECT_EQ(WgRelayLinkAddDetail(relay_, "alice", "bob"),
            kWgLinkOk);
  EXPECT_EQ(WgRelayLinkAddDetail(relay_, "carol", "dave"),
            kWgLinkOk);
}

// Adding the literal same pair twice is the duplicate
// outcome — distinct from limit_exceeded so the runner can
// surface "this link already exists" cleanly.
TEST_F(WgRelayLinkTest, DuplicateRejected) {
  EXPECT_EQ(WgRelayLinkAddDetail(relay_, "alice", "bob"),
            kWgLinkOk);
  EXPECT_EQ(WgRelayLinkAddDetail(relay_, "alice", "bob"),
            kWgLinkDuplicate);
  // Reverse direction is also caught as duplicate, since
  // links are undirected.
  EXPECT_EQ(WgRelayLinkAddDetail(relay_, "bob", "alice"),
            kWgLinkDuplicate);
}

// Self-link rejected with its own outcome code.
TEST_F(WgRelayLinkTest, SelfLinkRejected) {
  EXPECT_EQ(WgRelayLinkAddDetail(relay_, "alice", "alice"),
            kWgLinkSelfLink);
}

// Unknown peer rejected with its own outcome code.
TEST_F(WgRelayLinkTest, UnknownPeerRejected) {
  EXPECT_EQ(WgRelayLinkAddDetail(relay_, "alice", "ghost"),
            kWgLinkUnknownPeer);
  EXPECT_EQ(WgRelayLinkAddDetail(relay_, "ghost", "alice"),
            kWgLinkUnknownPeer);
}

// Bool-returning wrapper preserves the success/failure
// invariant — any non-OK detail maps to false.
TEST_F(WgRelayLinkTest, BoolWrapperConsistent) {
  EXPECT_TRUE(WgRelayLinkAdd(relay_, "alice", "bob"));
  EXPECT_FALSE(WgRelayLinkAdd(relay_, "alice", "carol"));
  EXPECT_FALSE(WgRelayLinkAdd(relay_, "ghost", "alice"));
  EXPECT_FALSE(WgRelayLinkAdd(relay_, "alice", "alice"));
}

}  // namespace
}  // namespace hyper_derp
