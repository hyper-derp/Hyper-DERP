/// @file test_hd_peerinfo.cc
/// @brief E2E: two HD clients receive PeerInfo about each
///   other and can exchange MeshData.

#include "harness.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cstring>

#include <gtest/gtest.h>
#include <sodium.h>

#include "hyper_derp/hd_client.h"
#include "hyper_derp/hd_protocol.h"
#include "hyper_derp/protocol.h"

namespace hyper_derp {
namespace {

class HdPeerInfoTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_GE(sodium_init(), 0);
    randombytes_buf(relay_key_.data(), kKeySize);
    port_ = test::FindFreePort();
    ASSERT_NE(port_, 0);
    relay_pid_ = test::StartHdRelay(
        port_, 1, relay_key_);
    ASSERT_GT(relay_pid_, 0);
    ASSERT_EQ(test::WaitRelayReady(port_, 5000), 0);
  }

  void TearDown() override {
    if (relay_pid_ > 0) test::StopRelay(relay_pid_);
  }

  Key relay_key_{};
  uint16_t port_ = 0;
  pid_t relay_pid_ = -1;
};

TEST_F(HdPeerInfoTest, BothClientsGetPeerInfo) {
  // Client A.
  HdClient a{};
  ASSERT_TRUE(HdClientInit(&a).has_value());
  a.relay_key = relay_key_;
  ASSERT_TRUE(
      test::ConnectHdClient(&a, "127.0.0.1", port_).has_value());
  ASSERT_TRUE(HdClientUpgrade(&a).has_value());
  ASSERT_TRUE(HdClientEnroll(&a).has_value());
  HdClientSetTimeout(&a, 3000);

  // Client B.
  HdClient b{};
  ASSERT_TRUE(HdClientInit(&b).has_value());
  b.relay_key = relay_key_;
  ASSERT_TRUE(
      test::ConnectHdClient(&b, "127.0.0.1", port_).has_value());
  ASSERT_TRUE(HdClientUpgrade(&b).has_value());
  ASSERT_TRUE(HdClientEnroll(&b).has_value());
  HdClientSetTimeout(&b, 3000);

  // A should receive PeerInfo about B.
  uint8_t buf[kHdMaxFramePayload];
  int buf_len;
  HdFrameType ftype;
  bool a_got_b = false;
  uint16_t b_peer_id = 0;

  for (int i = 0; i < 20; i++) {
    auto rv = HdClientRecvFrame(
        &a, &ftype, buf, &buf_len, sizeof(buf));
    if (!rv) break;
    if (ftype == HdFrameType::kPeerInfo &&
        buf_len >= 34) {
      Key peer_key;
      memcpy(peer_key.data(), buf, 32);
      if (peer_key == b.public_key) {
        a_got_b = true;
        b_peer_id =
            static_cast<uint16_t>(buf[32]) << 8 |
            buf[33];
        break;
      }
    }
  }
  EXPECT_TRUE(a_got_b) << "A did not get PeerInfo for B";
  EXPECT_GT(b_peer_id, 0) << "B peer_id should be > 0";

  // B should receive PeerInfo about A.
  bool b_got_a = false;
  uint16_t a_peer_id = 0;

  for (int i = 0; i < 20; i++) {
    auto rv = HdClientRecvFrame(
        &b, &ftype, buf, &buf_len, sizeof(buf));
    if (!rv) break;
    if (ftype == HdFrameType::kPeerInfo &&
        buf_len >= 34) {
      Key peer_key;
      memcpy(peer_key.data(), buf, 32);
      if (peer_key == a.public_key) {
        b_got_a = true;
        a_peer_id =
            static_cast<uint16_t>(buf[32]) << 8 |
            buf[33];
        break;
      }
    }
  }
  EXPECT_TRUE(b_got_a) << "B did not get PeerInfo for A";
  EXPECT_GT(a_peer_id, 0) << "A peer_id should be > 0";

  // A sends MeshData to B using B's peer_id.
  const char* msg = "hello-mesh-b";
  int msg_len = static_cast<int>(strlen(msg));
  auto sr = HdClientSendMeshData(
      &a, b_peer_id,
      reinterpret_cast<const uint8_t*>(msg), msg_len);
  ASSERT_TRUE(sr.has_value()) << "MeshData send failed";

  // B receives MeshData.
  bool b_got_mesh = false;
  for (int i = 0; i < 20; i++) {
    auto rv = HdClientRecvFrame(
        &b, &ftype, buf, &buf_len, sizeof(buf));
    if (!rv) break;
    if (ftype == HdFrameType::kMeshData && buf_len >= 2) {
      uint16_t src =
          static_cast<uint16_t>(buf[0]) << 8 | buf[1];
      EXPECT_EQ(src, a_peer_id);
      int payload_len = buf_len - 2;
      EXPECT_EQ(payload_len, msg_len);
      EXPECT_EQ(memcmp(buf + 2, msg, msg_len), 0);
      b_got_mesh = true;
      break;
    }
  }
  EXPECT_TRUE(b_got_mesh) << "B did not get MeshData from A";

  HdClientClose(&a);
  HdClientClose(&b);
}

}  // namespace
}  // namespace hyper_derp
