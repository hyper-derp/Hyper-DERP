/// @file test_wg_peer.cc
/// @brief Tests for WG peer state machine table.

#include <gtest/gtest.h>
#include <cstring>

#include "hd/wg/wg_peer.h"

namespace hyper_derp {
namespace {

TEST(WgPeerTest, AddFind) {
  WgPeerTable t{};
  Key k{};
  k[0] = 0x42;
  auto* p = WgPeerAdd(&t, k, 7);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(t.count, 1);
  EXPECT_EQ(p->hd_peer_id, 7);
  EXPECT_EQ(p->state, WgPeerState::kNew);

  EXPECT_EQ(WgPeerFind(&t, k), p);
  EXPECT_EQ(WgPeerFindById(&t, 7), p);
  EXPECT_EQ(WgPeerFindById(&t, 99), nullptr);
}

TEST(WgPeerTest, DuplicateAdd) {
  WgPeerTable t{};
  Key k{};
  k[0] = 1;
  auto* p1 = WgPeerAdd(&t, k, 1);
  auto* p2 = WgPeerAdd(&t, k, 1);
  EXPECT_EQ(p1, p2);
  EXPECT_EQ(t.count, 1);
}

TEST(WgPeerTest, Remove) {
  WgPeerTable t{};
  Key k{};
  k[0] = 5;
  WgPeerAdd(&t, k, 5);
  EXPECT_EQ(t.count, 1);

  WgPeerRemove(&t, k);
  EXPECT_EQ(t.count, 0);
  EXPECT_EQ(WgPeerFind(&t, k), nullptr);
}

TEST(WgPeerTest, MultiplePeers) {
  WgPeerTable t{};
  for (int i = 0; i < 10; i++) {
    Key k{};
    k[0] = static_cast<uint8_t>(i);
    auto* p = WgPeerAdd(&t, k, static_cast<uint16_t>(i));
    ASSERT_NE(p, nullptr);
  }
  EXPECT_EQ(t.count, 10);

  // Find each.
  for (int i = 0; i < 10; i++) {
    Key k{};
    k[0] = static_cast<uint8_t>(i);
    auto* p = WgPeerFind(&t, k);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->hd_peer_id, i);
  }

  // Remove odd ones.
  for (int i = 1; i < 10; i += 2) {
    Key k{};
    k[0] = static_cast<uint8_t>(i);
    WgPeerRemove(&t, k);
  }
  EXPECT_EQ(t.count, 5);

  // Even ones still findable.
  for (int i = 0; i < 10; i += 2) {
    Key k{};
    k[0] = static_cast<uint8_t>(i);
    EXPECT_NE(WgPeerFind(&t, k), nullptr);
  }
}

}  // namespace
}  // namespace hyper_derp
