/// @file test_hd_reconnect.cc
/// @brief E2E: HD client reconnects after relay restart.

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

class HdReconnectTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_GE(sodium_init(), 0);
    randombytes_buf(relay_key_.data(), kKeySize);
    port_ = test::FindFreePort();
    ASSERT_NE(port_, 0);
  }

  Key relay_key_{};
  uint16_t port_ = 0;
};

TEST_F(HdReconnectTest, ClientReconnectsAfterRelayRestart) {
  // Start relay.
  pid_t pid1 = test::StartHdRelay(port_, 1, relay_key_);
  ASSERT_GT(pid1, 0);
  ASSERT_EQ(test::WaitRelayReady(port_, 5000), 0);

  // Connect client.
  HdClient c{};
  ASSERT_TRUE(HdClientInit(&c).has_value());
  c.relay_key = relay_key_;
  ASSERT_TRUE(
      HdClientConnect(&c, "127.0.0.1", port_).has_value());
  ASSERT_TRUE(HdClientUpgrade(&c).has_value());
  ASSERT_TRUE(HdClientEnroll(&c).has_value());

  // Verify connection works.
  ASSERT_TRUE(HdClientSendPing(&c).has_value());
  HdClientSetTimeout(&c, 2000);
  uint8_t buf[256];
  int buf_len;
  HdFrameType ftype;
  bool got_pong = false;
  for (int i = 0; i < 5; i++) {
    if (HdClientRecvFrame(&c, &ftype, buf, &buf_len,
                          sizeof(buf)).has_value()) {
      if (ftype == HdFrameType::kPong) {
        got_pong = true;
        break;
      }
    }
  }
  EXPECT_TRUE(got_pong) << "Initial ping/pong failed";

  // Kill relay.
  test::StopRelay(pid1);
  usleep(200000);

  // Reconnect should work with HdClientReconnect.
  pid_t pid2 = test::StartHdRelay(port_, 1, relay_key_);
  ASSERT_GT(pid2, 0);
  ASSERT_EQ(test::WaitRelayReady(port_, 5000), 0);

  auto recon = HdClientReconnect(&c);
  ASSERT_TRUE(recon.has_value()) << "Reconnect failed: "
      << recon.error().message;
  EXPECT_TRUE(c.approved);

  // Verify reconnected connection works.
  ASSERT_TRUE(HdClientSendPing(&c).has_value());
  got_pong = false;
  for (int i = 0; i < 5; i++) {
    if (HdClientRecvFrame(&c, &ftype, buf, &buf_len,
                          sizeof(buf)).has_value()) {
      if (ftype == HdFrameType::kPong) {
        got_pong = true;
        break;
      }
    }
  }
  EXPECT_TRUE(got_pong) << "Post-reconnect ping/pong failed";

  HdClientClose(&c);
  test::StopRelay(pid2);
}

}  // namespace
}  // namespace hyper_derp
