/// @file test_hd_peers.cc
/// @brief Unit tests for HD peer registry.

#include "hyper_derp/hd_peers.h"

#include <arpa/inet.h>

#include <cstring>

#include <gtest/gtest.h>
#include <sodium.h>

namespace hyper_derp {

class HdPeersTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // sodium_init() returns 0 on first init, 1 if already
    // initialized, -1 on failure.
    ASSERT_GE(sodium_init(), 0);
  }
};

TEST_F(HdPeersTest, InitSetsRelayKey) {
  HdPeerRegistry reg;
  Key relay_key;
  memset(relay_key.data(), 0x42, kKeySize);
  HdPeersInit(&reg, relay_key, HdEnrollMode::kManual);
  EXPECT_EQ(reg.relay_key, relay_key);
  EXPECT_EQ(reg.enroll_mode, HdEnrollMode::kManual);
  EXPECT_EQ(reg.peer_count, 0);
}

TEST_F(HdPeersTest, InsertAndLookup) {
  HdPeerRegistry reg;
  Key relay_key{};
  HdPeersInit(&reg, relay_key, HdEnrollMode::kManual);

  Key peer_key;
  memset(peer_key.data(), 0xAA, kKeySize);
  HdPeer* p = HdPeersInsert(&reg, peer_key, 42);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->key, peer_key);
  EXPECT_EQ(p->fd, 42);
  EXPECT_EQ(p->state, HdPeerState::kPending);
  EXPECT_EQ(reg.peer_count, 1);

  HdPeer* found = HdPeersLookup(&reg, peer_key.data());
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found, p);
}

TEST_F(HdPeersTest, InsertDuplicate) {
  HdPeerRegistry reg;
  Key relay_key{};
  HdPeersInit(&reg, relay_key, HdEnrollMode::kManual);

  Key peer_key;
  memset(peer_key.data(), 0xBB, kKeySize);
  HdPeer* p1 = HdPeersInsert(&reg, peer_key, 10);
  HdPeer* p2 = HdPeersInsert(&reg, peer_key, 20);
  EXPECT_EQ(p1, p2);
  EXPECT_EQ(reg.peer_count, 1);
}

TEST_F(HdPeersTest, ApproveTransition) {
  HdPeerRegistry reg;
  Key relay_key{};
  HdPeersInit(&reg, relay_key, HdEnrollMode::kManual);

  Key peer_key;
  memset(peer_key.data(), 0xCC, kKeySize);
  HdPeersInsert(&reg, peer_key, 5);
  EXPECT_TRUE(HdPeersApprove(&reg, peer_key.data()));

  HdPeer* p = HdPeersLookup(&reg, peer_key.data());
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->state, HdPeerState::kApproved);
}

TEST_F(HdPeersTest, DenyTransition) {
  HdPeerRegistry reg;
  Key relay_key{};
  HdPeersInit(&reg, relay_key, HdEnrollMode::kManual);

  Key peer_key;
  memset(peer_key.data(), 0xDD, kKeySize);
  HdPeersInsert(&reg, peer_key, 7);
  EXPECT_TRUE(HdPeersDeny(&reg, peer_key.data()));

  HdPeer* p = HdPeersLookup(&reg, peer_key.data());
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->state, HdPeerState::kDenied);
}

TEST_F(HdPeersTest, RemoveTombstones) {
  HdPeerRegistry reg;
  Key relay_key{};
  HdPeersInit(&reg, relay_key, HdEnrollMode::kManual);

  Key peer_key;
  memset(peer_key.data(), 0xEE, kKeySize);
  HdPeersInsert(&reg, peer_key, 9);
  EXPECT_EQ(reg.peer_count, 1);

  HdPeersRemove(&reg, peer_key.data());
  EXPECT_EQ(reg.peer_count, 0);
  EXPECT_EQ(HdPeersLookup(&reg, peer_key.data()), nullptr);
}

TEST_F(HdPeersTest, AddRule) {
  HdPeerRegistry reg;
  Key relay_key{};
  HdPeersInit(&reg, relay_key, HdEnrollMode::kManual);

  Key peer_key;
  memset(peer_key.data(), 0x11, kKeySize);
  HdPeersInsert(&reg, peer_key, 3);

  Key dst;
  memset(dst.data(), 0x22, kKeySize);
  EXPECT_TRUE(
      HdPeersAddRule(&reg, peer_key.data(), dst));

  HdPeer* p = HdPeersLookup(&reg, peer_key.data());
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->rule_count, 1);
}

TEST_F(HdPeersTest, AddRuleMax) {
  HdPeerRegistry reg;
  Key relay_key{};
  HdPeersInit(&reg, relay_key, HdEnrollMode::kManual);

  Key peer_key;
  memset(peer_key.data(), 0x33, kKeySize);
  HdPeersInsert(&reg, peer_key, 4);

  for (int i = 0; i < kHdMaxForwardRules; ++i) {
    Key dst;
    memset(dst.data(), static_cast<uint8_t>(i + 1),
           kKeySize);
    EXPECT_TRUE(
        HdPeersAddRule(&reg, peer_key.data(), dst));
  }
  // One more should fail.
  Key extra;
  memset(extra.data(), 0xFF, kKeySize);
  EXPECT_FALSE(
      HdPeersAddRule(&reg, peer_key.data(), extra));
}

TEST_F(HdPeersTest, RemoveRule) {
  HdPeerRegistry reg;
  Key relay_key{};
  HdPeersInit(&reg, relay_key, HdEnrollMode::kManual);

  Key peer_key;
  memset(peer_key.data(), 0x44, kKeySize);
  HdPeersInsert(&reg, peer_key, 6);

  Key dst;
  memset(dst.data(), 0x55, kKeySize);
  HdPeersAddRule(&reg, peer_key.data(), dst);

  HdPeer* p = HdPeersLookup(&reg, peer_key.data());
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->rule_count, 1);

  EXPECT_TRUE(HdPeersRemoveRule(
      &reg, peer_key.data(), dst.data()));
  EXPECT_EQ(p->rule_count, 0);
}

TEST_F(HdPeersTest, VerifyHmacValid) {
  HdPeerRegistry reg;
  Key relay_key;
  randombytes_buf(relay_key.data(), kKeySize);
  HdPeersInit(&reg, relay_key, HdEnrollMode::kManual);

  Key client_key;
  randombytes_buf(client_key.data(), kKeySize);

  uint8_t hmac[kHdHmacSize];
  crypto_auth(hmac, client_key.data(), kKeySize,
              relay_key.data());

  EXPECT_TRUE(
      HdVerifyEnrollment(&reg, client_key, hmac));
}

TEST_F(HdPeersTest, VerifyHmacInvalid) {
  HdPeerRegistry reg;
  Key relay_key;
  randombytes_buf(relay_key.data(), kKeySize);
  HdPeersInit(&reg, relay_key, HdEnrollMode::kManual);

  Key client_key;
  randombytes_buf(client_key.data(), kKeySize);

  uint8_t hmac[kHdHmacSize];
  memset(hmac, 0x00, kHdHmacSize);

  EXPECT_FALSE(
      HdVerifyEnrollment(&reg, client_key, hmac));
}

TEST_F(HdPeersTest, ListPeers) {
  HdPeerRegistry reg;
  Key relay_key{};
  HdPeersInit(&reg, relay_key, HdEnrollMode::kManual);

  constexpr int kCount = 5;
  Key keys[kCount];
  for (int i = 0; i < kCount; ++i) {
    memset(keys[i].data(),
           static_cast<uint8_t>(0x60 + i), kKeySize);
    HdPeersInsert(&reg, keys[i], 100 + i);
  }
  // Approve one, deny another.
  HdPeersApprove(&reg, keys[1].data());
  HdPeersDeny(&reg, keys[2].data());

  Key out_keys[kCount];
  HdPeerState out_states[kCount];
  int n = HdPeersList(&reg, out_keys, out_states,
                      kCount);
  EXPECT_EQ(n, kCount);

  // Verify all keys are present.
  for (int i = 0; i < kCount; ++i) {
    bool found = false;
    for (int j = 0; j < n; ++j) {
      if (out_keys[j] == keys[i]) {
        found = true;
        break;
      }
    }
    EXPECT_TRUE(found) << "key " << i << " not listed";
  }
}

TEST_F(HdPeersTest, PolicyAllowsUnconstrained) {
  HdPeerRegistry reg;
  Key relay_key{};
  HdPeersInit(&reg, relay_key,
              HdEnrollMode::kAutoApprove);
  Key k{};
  k[0] = 0xAA;
  EXPECT_TRUE(HdPolicyAllows(&reg, k, 0, nullptr));
}

TEST_F(HdPeersTest, PolicyMaxPeersCap) {
  HdPeerRegistry reg;
  Key relay_key{};
  HdPeersInit(&reg, relay_key,
              HdEnrollMode::kAutoApprove);
  reg.policy.max_peers = 2;
  reg.peer_count = 2;
  Key k{};
  k[0] = 1;
  const char* reason = nullptr;
  EXPECT_FALSE(HdPolicyAllows(&reg, k, 0, &reason));
  EXPECT_NE(reason, nullptr);
}

TEST_F(HdPeersTest, PolicyAllowedKeysGlob) {
  HdPeerRegistry reg;
  Key relay_key{};
  HdPeersInit(&reg, relay_key,
              HdEnrollMode::kAutoApprove);
  // Accept everything starting with "ck_00" (raw hex byte
  // 0x00 at position 0).
  reg.policy.allowed_keys = {"ck_00*"};
  Key k{};  // all zeros → hex starts with "00"
  EXPECT_TRUE(HdPolicyAllows(&reg, k, 0, nullptr));

  Key bad{};
  bad[0] = 0xFF;  // hex starts with "ff"
  const char* reason = nullptr;
  EXPECT_FALSE(HdPolicyAllows(&reg, bad, 0, &reason));
  EXPECT_NE(reason, nullptr);
}

TEST_F(HdPeersTest, RevokeAddsToDenylist) {
  HdPeerRegistry reg;
  Key relay_key{};
  HdPeersInit(&reg, relay_key, HdEnrollMode::kManual);
  Key peer_key{};
  peer_key[0] = 0x77;
  HdPeersInsert(&reg, peer_key, 99);
  ASSERT_FALSE(HdPeersIsDenied(&reg, peer_key.data()));

  HdPeersRevoke(&reg, peer_key.data());
  EXPECT_TRUE(HdPeersIsDenied(&reg, peer_key.data()));
  EXPECT_EQ(HdPeersLookup(&reg, peer_key.data()),
            nullptr);
}

TEST_F(HdPeersTest, DenylistPersistsAcrossRegistries) {
  char path[] = "/tmp/hd_denylist_XXXXXX";
  int fd = mkstemp(path);
  ASSERT_GE(fd, 0);
  close(fd);
  std::string file = path;

  Key peer_key{};
  peer_key[0] = 0xAB;

  {
    HdPeerRegistry reg;
    Key relay_key{};
    HdPeersInit(&reg, relay_key,
                HdEnrollMode::kManual);
    reg.denylist_path = file;
    HdPeersInsert(&reg, peer_key, 1);
    HdPeersRevoke(&reg, peer_key.data());
  }

  {
    HdPeerRegistry reg2;
    Key relay_key{};
    HdPeersInit(&reg2, relay_key,
                HdEnrollMode::kManual);
    reg2.denylist_path = file;
    EXPECT_TRUE(HdDenylistLoad(&reg2));
    EXPECT_TRUE(
        HdPeersIsDenied(&reg2, peer_key.data()));
  }

  unlink(file.c_str());
}

TEST_F(HdPeersTest, PolicyIpRange) {
  HdPeerRegistry reg;
  Key relay_key{};
  HdPeersInit(&reg, relay_key,
              HdEnrollMode::kAutoApprove);
  reg.policy.require_ip_range = "10.0.0.0/8";
  Key k{};
  // 10.1.2.3 — inside.
  uint32_t ip_in = htonl((10u << 24) | (1u << 16) |
                         (2u << 8) | 3u);
  EXPECT_TRUE(HdPolicyAllows(&reg, k, ip_in, nullptr));

  // 192.168.1.1 — outside.
  uint32_t ip_out = htonl((192u << 24) | (168u << 16) |
                          (1u << 8) | 1u);
  const char* reason = nullptr;
  EXPECT_FALSE(
      HdPolicyAllows(&reg, k, ip_out, &reason));
  EXPECT_NE(reason, nullptr);
}

}  // namespace hyper_derp
