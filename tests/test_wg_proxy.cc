/// @file test_wg_proxy.cc
/// @brief Tests for WG proxy WGEX serialization and peer table.

#include <gtest/gtest.h>
#include <cstring>

#include "wg_proxy.h"
#include "hyper_derp/protocol.h"

namespace hyper_derp {
namespace {

TEST(WgProxyTest, WgexRoundtrip) {
  uint8_t pubkey[32];
  for (int i = 0; i < 32; i++) pubkey[i] = i;
  uint32_t ip = 0x0A630001;  // 10.99.0.1

  uint8_t buf[64];
  int len = WgSerializeWgex(pubkey, ip, buf, sizeof(buf));
  ASSERT_EQ(len, kWgexSize);

  uint8_t out_key[32];
  uint32_t out_ip;
  ASSERT_EQ(WgParseWgex(buf, len, out_key, &out_ip), 0);
  EXPECT_EQ(memcmp(pubkey, out_key, 32), 0);
  EXPECT_EQ(out_ip, ip);
}

TEST(WgProxyTest, WgexMagicCheck) {
  uint8_t buf[40] = {};
  uint8_t key[32];
  uint32_t ip;

  // Wrong magic.
  buf[0] = 'X';
  EXPECT_EQ(WgParseWgex(buf, 40, key, &ip), -1);

  // Too short.
  EXPECT_EQ(WgParseWgex(buf, 10, key, &ip), -1);
}

TEST(WgProxyTest, WgexBufferTooSmall) {
  uint8_t pubkey[32] = {};
  uint8_t buf[10];
  EXPECT_EQ(WgSerializeWgex(pubkey, 0, buf, sizeof(buf)),
            -1);
}

TEST(WgProxyTest, PeerAddRemove) {
  WgProxy proxy{};
  proxy.udp_fd = -1;  // Don't need a real socket.

  Key k1{};
  k1[0] = 1;
  uint8_t wg[32] = {};
  wg[0] = 0xAA;

  auto* p = WgProxyAddPeer(&proxy, k1, 42, wg, 0x01020304);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(proxy.peer_count, 1);
  EXPECT_EQ(p->hd_peer_id, 42);
  EXPECT_EQ(p->wg_pubkey[0], 0xAA);
  EXPECT_EQ(p->tunnel_ip, 0x01020304u);

  // Duplicate add returns existing.
  auto* p2 = WgProxyAddPeer(&proxy, k1, 42, wg, 0);
  EXPECT_EQ(p2, p);
  EXPECT_EQ(proxy.peer_count, 1);

  // Find by HD ID.
  EXPECT_EQ(WgProxyFindByHdId(&proxy, 42), p);
  EXPECT_EQ(WgProxyFindByHdId(&proxy, 99), nullptr);

  // Remove.
  WgProxyRemovePeer(&proxy, wg);
  EXPECT_EQ(proxy.peer_count, 0);
  EXPECT_EQ(WgProxyFindByHdId(&proxy, 42), nullptr);
}

TEST(WgProxyTest, PeerTableFull) {
  WgProxy proxy{};
  proxy.udp_fd = -1;
  uint8_t wg[32] = {};

  // Fill all slots.
  for (int i = 0; i < kWgMaxProxyPeers; i++) {
    Key k{};
    k[0] = static_cast<uint8_t>(i);
    k[1] = static_cast<uint8_t>(i >> 8);
    wg[0] = static_cast<uint8_t>(i);
    auto* p = WgProxyAddPeer(&proxy, k,
        static_cast<uint16_t>(i + 1), wg, 0);
    ASSERT_NE(p, nullptr) << "slot " << i;
  }
  EXPECT_EQ(proxy.peer_count, kWgMaxProxyPeers);

  // Next add should fail.
  Key kx{};
  kx[0] = 0xFF;
  kx[1] = 0xFF;
  EXPECT_EQ(WgProxyAddPeer(&proxy, kx, 999, wg, 0),
            nullptr);
}

}  // namespace
}  // namespace hyper_derp
