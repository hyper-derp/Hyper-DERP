/// @file test_open_conn.cc
/// @brief Integration tests for the OpenConnection
///   round-trip on the server side (Phase 2.3). Uses
///   HdClient directly to speak the wire format; SDK
///   integration lands in Phase 2.4.

#include "harness.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cstring>

#include <gtest/gtest.h>
#include <sodium.h>

#include "hyper_derp/hd_client.h"
#include "hyper_derp/hd_peers.h"
#include "hyper_derp/hd_protocol.h"
#include "hyper_derp/protocol.h"

namespace hyper_derp {
namespace {

class OpenConnTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_GE(sodium_init(), 0);
    randombytes_buf(relay_key_.data(), kKeySize);
    port_ = test::FindFreePort();
    ASSERT_NE(port_, 0);
    relay_pid_ = test::StartHdRelay(port_, 1, relay_key_);
    ASSERT_GT(relay_pid_, 0);
    ASSERT_EQ(test::WaitRelayReady(port_, 5000), 0);
  }

  void TearDown() override {
    if (relay_pid_ > 0) test::StopRelay(relay_pid_);
  }

  // Enrolls a client with fresh keys and returns by
  // filling `out`. Leaves the connection open.
  void EnrollClient(HdClient* out) {
    uint8_t pub[kKeySize], priv[kKeySize];
    crypto_box_keypair(pub, priv);
    HdClientInitWithKeys(out, pub, priv, relay_key_);
    ASSERT_TRUE(test::ConnectHdClient(out, "127.0.0.1",
                                      port_)
                    .has_value());
    ASSERT_TRUE(HdClientUpgrade(out).has_value());
    ASSERT_TRUE(HdClientEnroll(out).has_value());
  }

  // Sends a raw frame on the client's fd.
  bool SendRaw(HdClient* c, const uint8_t* buf, int n) {
    int total = 0;
    while (total < n) {
      int w = ::write(c->fd, buf + total, n - total);
      if (w <= 0) return false;
      total += w;
    }
    return true;
  }

  Key relay_key_{};
  uint16_t port_ = 0;
  pid_t relay_pid_ = -1;
};

TEST_F(OpenConnTest, CrossRelayDeniedImmediately) {
  HdClient a;
  EnrollClient(&a);
  (void)HdClientSetTimeout(&a, 3000);

  uint8_t buf[kHdFrameHeaderSize + kHdOpenConnSize];
  HdBuildOpenConnection(buf, /*target_peer_id=*/1,
                        /*target_relay_id=*/99,
                        HdIntent::kRequireRelay,
                        kHdFlagAllowUpgrade, 0xA1);
  ASSERT_TRUE(
      SendRaw(&a, buf, sizeof(buf)));

  // Skip PeerInfo / peer-presence frames the relay may
  // push during enrollment; wait for the result.
  HdFrameType ft;
  uint8_t pbuf[512];
  int plen = 0;
  for (int i = 0; i < 16; i++) {
    ASSERT_TRUE(HdClientRecvFrame(&a, &ft, pbuf, &plen,
                                  sizeof(pbuf))
                    .has_value());
    if (ft == HdFrameType::kOpenConnectionResult) break;
  }
  ASSERT_EQ(ft, HdFrameType::kOpenConnectionResult);
  HdOpenConnectionResult r;
  ASSERT_TRUE(HdParseOpenConnectionResult(pbuf, plen, &r));
  EXPECT_EQ(r.correlation_id, 0xA1u);
  EXPECT_EQ(r.mode, HdConnMode::kDenied);
  EXPECT_EQ(r.deny_reason,
            HdDenyReason::kFleetRoutingNotImplemented);

  HdClientClose(&a);
}

TEST_F(OpenConnTest, UnknownTargetPeerUnreachable) {
  HdClient a;
  EnrollClient(&a);
  (void)HdClientSetTimeout(&a, 3000);

  uint8_t buf[kHdFrameHeaderSize + kHdOpenConnSize];
  HdBuildOpenConnection(buf, /*target_peer_id=*/9999,
                        /*target_relay_id=*/0,
                        HdIntent::kRequireRelay,
                        kHdFlagAllowUpgrade, 0xB2);
  ASSERT_TRUE(SendRaw(&a, buf, sizeof(buf)));

  // Skip PeerInfo / peer-presence frames the relay may
  // push during enrollment; wait for the result.
  HdFrameType ft;
  uint8_t pbuf[512];
  int plen = 0;
  for (int i = 0; i < 16; i++) {
    ASSERT_TRUE(HdClientRecvFrame(&a, &ft, pbuf, &plen,
                                  sizeof(pbuf))
                    .has_value());
    if (ft == HdFrameType::kOpenConnectionResult) break;
  }
  ASSERT_EQ(ft, HdFrameType::kOpenConnectionResult);
  HdOpenConnectionResult r;
  ASSERT_TRUE(HdParseOpenConnectionResult(pbuf, plen, &r));
  EXPECT_EQ(r.mode, HdConnMode::kDenied);
  EXPECT_EQ(r.deny_reason,
            HdDenyReason::kPeerUnreachable);

  HdClientClose(&a);
}

TEST_F(OpenConnTest, TargetUnresponsiveTimesOut) {
  // Client B enrolls but does not respond to the
  // IncomingConnection; the resolver should fire the
  // 5s deadline and hand back kTargetUnresponsive.
  HdClient b;
  EnrollClient(&b);

  // Wait for B's registration to propagate.
  usleep(200000);

  HdClient a;
  EnrollClient(&a);
  (void)HdClientSetTimeout(&a, 10000);

  // We don't know B's local peer_id; both clients are
  // enrolled in a fresh registry, so B is peer_id=1 and
  // A is peer_id=2.
  uint8_t buf[kHdFrameHeaderSize + kHdOpenConnSize];
  HdBuildOpenConnection(buf, /*target_peer_id=*/1,
                        /*target_relay_id=*/0,
                        HdIntent::kRequireRelay,
                        kHdFlagAllowUpgrade, 0xC3);
  ASSERT_TRUE(SendRaw(&a, buf, sizeof(buf)));

  // Wait up to the 5s deadline for the timeout.
  HdFrameType ft = HdFrameType::kData;
  uint8_t pbuf[512];
  int plen = 0;
  for (int i = 0; i < 16; i++) {
    ASSERT_TRUE(HdClientRecvFrame(&a, &ft, pbuf, &plen,
                                  sizeof(pbuf))
                    .has_value());
    if (ft == HdFrameType::kOpenConnectionResult) break;
  }
  ASSERT_EQ(ft, HdFrameType::kOpenConnectionResult);
  HdOpenConnectionResult r;
  ASSERT_TRUE(HdParseOpenConnectionResult(pbuf, plen, &r));
  EXPECT_EQ(r.correlation_id, 0xC3u);
  EXPECT_EQ(r.mode, HdConnMode::kDenied);
  EXPECT_EQ(r.deny_reason,
            HdDenyReason::kTargetUnresponsive);

  HdClientClose(&a);
  HdClientClose(&b);
}

}  // namespace
}  // namespace hyper_derp
