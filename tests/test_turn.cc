/// @file test_turn.cc
/// @brief Unit tests for TURN allocation manager.

#include "hyper_derp/turn.h"

#include <arpa/inet.h>
#include <cstring>
#include <memory>
#include <thread>

#include <gtest/gtest.h>

namespace hyper_derp {

// Helper: create a manager on the heap (too large for
// stack due to kTurnMaxAllocations * TurnAllocation).
static std::unique_ptr<TurnManager> MakeMgr() {
  auto mgr = std::make_unique<TurnManager>();
  TurnInit(mgr.get(), inet_addr("10.0.0.1"), "test.realm");
  return mgr;
}

TEST(TurnTest, AllocateBasic) {
  auto mgr = MakeMgr();
  auto* a = TurnAllocate(
      mgr.get(), inet_addr("192.168.1.1"), 5000, 600);
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(a->occupied, 1);
  EXPECT_EQ(a->client_ip, inet_addr("192.168.1.1"));
  EXPECT_EQ(a->client_port, 5000);
  EXPECT_EQ(a->relay_ip, inet_addr("10.0.0.1"));
  EXPECT_NE(a->relay_port, 0);
  EXPECT_GT(a->expires_at, 0u);
  EXPECT_EQ(TurnAllocCount(mgr.get()), 1);
}

TEST(TurnTest, AllocateFull) {
  auto mgr = MakeMgr();
  for (int i = 0; i < kTurnMaxAllocations; i++) {
    auto* a = TurnAllocate(
        mgr.get(),
        static_cast<uint32_t>(i + 1),
        static_cast<uint16_t>(i % 65535),
        600);
    ASSERT_NE(a, nullptr) << "Failed at allocation " << i;
  }
  EXPECT_EQ(TurnAllocCount(mgr.get()),
            kTurnMaxAllocations);
  // Next allocation should fail.
  auto* over = TurnAllocate(
      mgr.get(), 0xFFFFFFFF, 9999, 600);
  EXPECT_EQ(over, nullptr);
}

TEST(TurnTest, FindAlloc) {
  auto mgr = MakeMgr();
  uint32_t cip = inet_addr("10.1.2.3");
  uint16_t cport = 4444;
  auto* a = TurnAllocate(mgr.get(), cip, cport, 600);
  ASSERT_NE(a, nullptr);
  auto* found = TurnFindAlloc(mgr.get(), cip, cport);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->id, a->id);
  // Non-existent returns nullptr.
  auto* miss = TurnFindAlloc(
      mgr.get(), inet_addr("10.9.9.9"), 1111);
  EXPECT_EQ(miss, nullptr);
}

TEST(TurnTest, Refresh) {
  auto mgr = MakeMgr();
  auto* a = TurnAllocate(
      mgr.get(), inet_addr("10.0.0.2"), 6000, 60);
  ASSERT_NE(a, nullptr);
  uint64_t old_exp = a->expires_at;
  // Small sleep so NowNs() advances.
  std::this_thread::sleep_for(
      std::chrono::milliseconds(5));
  bool ok = TurnRefresh(mgr.get(), a->id, 3600);
  EXPECT_TRUE(ok);
  EXPECT_GT(a->expires_at, old_exp);
}

TEST(TurnTest, RefreshZeroDeallocates) {
  auto mgr = MakeMgr();
  auto* a = TurnAllocate(
      mgr.get(), inet_addr("10.0.0.3"), 7000, 600);
  ASSERT_NE(a, nullptr);
  uint32_t aid = a->id;
  EXPECT_EQ(TurnAllocCount(mgr.get()), 1);
  bool ok = TurnRefresh(mgr.get(), aid, 0);
  EXPECT_TRUE(ok);
  EXPECT_EQ(TurnAllocCount(mgr.get()), 0);
}

TEST(TurnTest, Deallocate) {
  auto mgr = MakeMgr();
  auto* a = TurnAllocate(
      mgr.get(), inet_addr("10.0.0.4"), 8000, 600);
  ASSERT_NE(a, nullptr);
  uint32_t aid = a->id;
  EXPECT_EQ(TurnAllocCount(mgr.get()), 1);
  TurnDeallocate(mgr.get(), aid);
  EXPECT_EQ(TurnAllocCount(mgr.get()), 0);
  EXPECT_EQ(a->occupied, 0);
}

TEST(TurnTest, CreatePermission) {
  auto mgr = MakeMgr();
  auto* a = TurnAllocate(
      mgr.get(), inet_addr("10.0.0.5"), 9000, 600);
  ASSERT_NE(a, nullptr);
  uint32_t peer = inet_addr("172.16.0.1");
  bool ok = TurnCreatePermission(
      mgr.get(), a->id, peer);
  EXPECT_TRUE(ok);
  EXPECT_TRUE(TurnCheckPermission(a, peer));
  // Non-permitted peer returns false.
  EXPECT_FALSE(TurnCheckPermission(
      a, inet_addr("172.16.0.99")));
}

TEST(TurnTest, PermissionExpiry) {
  auto mgr = MakeMgr();
  auto* a = TurnAllocate(
      mgr.get(), inet_addr("10.0.0.6"), 1234, 600);
  ASSERT_NE(a, nullptr);
  uint32_t peer = inet_addr("172.16.1.1");
  TurnCreatePermission(mgr.get(), a->id, peer);
  EXPECT_TRUE(TurnCheckPermission(a, peer));
  // Force expiration by zeroing expires_at.
  for (int i = 0; i < kTurnMaxPermissions; i++) {
    if (a->permissions[i].occupied == 1 &&
        a->permissions[i].peer_ip == peer) {
      a->permissions[i].expires_at = 1;
      break;
    }
  }
  EXPECT_FALSE(TurnCheckPermission(a, peer));
}

TEST(TurnTest, ChannelBind) {
  auto mgr = MakeMgr();
  auto* a = TurnAllocate(
      mgr.get(), inet_addr("10.0.0.7"), 2222, 600);
  ASSERT_NE(a, nullptr);
  uint32_t peer = inet_addr("172.16.2.1");
  uint16_t pport = htons(5555);
  bool ok = TurnChannelBind(
      mgr.get(), a->id, 0x4000, peer, pport);
  EXPECT_TRUE(ok);
  const TurnChannel* ch = TurnFindChannel(a, 0x4000);
  ASSERT_NE(ch, nullptr);
  EXPECT_EQ(ch->peer_ip, peer);
  EXPECT_EQ(ch->peer_port, pport);
}

TEST(TurnTest, ChannelRange) {
  auto mgr = MakeMgr();
  auto* a = TurnAllocate(
      mgr.get(), inet_addr("10.0.0.8"), 3333, 600);
  ASSERT_NE(a, nullptr);
  uint32_t peer = inet_addr("172.16.3.1");
  // Below range.
  EXPECT_FALSE(TurnChannelBind(
      mgr.get(), a->id, 0x3FFF, peer, 100));
  // Above range.
  EXPECT_FALSE(TurnChannelBind(
      mgr.get(), a->id, 0x8000, peer, 100));
  // At boundaries.
  EXPECT_TRUE(TurnChannelBind(
      mgr.get(), a->id, kTurnChannelMin, peer, 100));
  EXPECT_TRUE(TurnChannelBind(
      mgr.get(), a->id, kTurnChannelMax,
      inet_addr("172.16.3.2"), 200));
}

TEST(TurnTest, ChannelDuplicate) {
  auto mgr = MakeMgr();
  auto* a = TurnAllocate(
      mgr.get(), inet_addr("10.0.0.9"), 4444, 600);
  ASSERT_NE(a, nullptr);
  uint32_t p1 = inet_addr("172.16.4.1");
  uint32_t p2 = inet_addr("172.16.4.2");
  uint16_t port = htons(6666);
  // Bind channel 0x4001 to peer1.
  EXPECT_TRUE(TurnChannelBind(
      mgr.get(), a->id, 0x4001, p1, port));
  // Same channel, different peer: rejected.
  EXPECT_FALSE(TurnChannelBind(
      mgr.get(), a->id, 0x4001, p2, port));
  // Same channel, same peer: refresh (accepted).
  EXPECT_TRUE(TurnChannelBind(
      mgr.get(), a->id, 0x4001, p1, port));
}

TEST(TurnTest, Expire) {
  auto mgr = MakeMgr();
  auto* a = TurnAllocate(
      mgr.get(), inet_addr("10.0.0.10"), 5555, 600);
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(TurnAllocCount(mgr.get()), 1);
  // Force allocation to be expired.
  a->expires_at = 1;
  int n = TurnExpire(mgr.get());
  EXPECT_GE(n, 1);
  EXPECT_EQ(TurnAllocCount(mgr.get()), 0);
  EXPECT_EQ(a->occupied, 0);
}

TEST(TurnTest, AllocCount) {
  auto mgr = MakeMgr();
  EXPECT_EQ(TurnAllocCount(mgr.get()), 0);
  auto* a1 = TurnAllocate(
      mgr.get(), inet_addr("10.1.0.1"), 100, 600);
  EXPECT_EQ(TurnAllocCount(mgr.get()), 1);
  auto* a2 = TurnAllocate(
      mgr.get(), inet_addr("10.1.0.2"), 200, 600);
  EXPECT_EQ(TurnAllocCount(mgr.get()), 2);
  ASSERT_NE(a1, nullptr);
  ASSERT_NE(a2, nullptr);
  TurnDeallocate(mgr.get(), a1->id);
  EXPECT_EQ(TurnAllocCount(mgr.get()), 1);
  TurnDeallocate(mgr.get(), a2->id);
  EXPECT_EQ(TurnAllocCount(mgr.get()), 0);
}

}  // namespace hyper_derp
