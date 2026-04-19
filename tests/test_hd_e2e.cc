/// @file test_hd_e2e.cc
/// @brief End-to-end integration tests for the HD protocol:
///   relay server with two HD clients using enrollment.

#include "harness.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include <gtest/gtest.h>
#include <sodium.h>

#include "hyper_derp/hd_client.h"
#include "hyper_derp/hd_peers.h"
#include "hyper_derp/hd_protocol.h"
#include "hyper_derp/protocol.h"

namespace hyper_derp {
namespace {

class HdE2ETest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_GE(sodium_init(), 0);

    // Generate a relay key for HMAC enrollment.
    randombytes_buf(relay_key_.data(), kKeySize);

    port_ = test::FindFreePort();
    ASSERT_NE(port_, 0) << "Could not find a free port";
    relay_pid_ = test::StartHdRelay(
        port_, 1, relay_key_);
    ASSERT_GT(relay_pid_, 0) << "Failed to fork relay";
    ASSERT_EQ(test::WaitRelayReady(port_, 5000), 0)
        << "Relay did not start on port " << port_;
  }

  void TearDown() override {
    if (relay_pid_ > 0) {
      test::StopRelay(relay_pid_);
    }
  }

  Key relay_key_{};
  uint16_t port_ = 0;
  pid_t relay_pid_ = -1;
};

TEST_F(HdE2ETest, SingleDataRelay) {
  // Pre-generate key pairs.
  uint8_t pub_a[kKeySize], priv_a[kKeySize];
  uint8_t pub_b[kKeySize], priv_b[kKeySize];
  crypto_box_keypair(pub_a, priv_a);
  crypto_box_keypair(pub_b, priv_b);

  // Pipe for B to signal it's connected.
  int b_ready[2];
  ASSERT_EQ(pipe(b_ready), 0);

  // Pipe for B to report test result.
  int result_pipe[2];
  ASSERT_EQ(pipe(result_pipe), 0);

  const char* test_data = "hello-hd-from-a";
  int test_data_len =
      static_cast<int>(strlen(test_data));

  // Fork client B (receiver).
  pid_t pid_b = fork();
  ASSERT_GE(pid_b, 0);

  if (pid_b == 0) {
    close(b_ready[0]);
    close(result_pipe[0]);

    HdClient client;
    HdClientInitWithKeys(&client, pub_b, priv_b,
                         relay_key_);

    bool ok = true;
    ok = ok && test::ConnectHdClient(
        &client, "127.0.0.1", port_).has_value();
    ok = ok && HdClientUpgrade(&client).has_value();
    ok = ok && HdClientEnroll(&client).has_value();

    // Signal ready.
    write(b_ready[1], "R", 1);
    close(b_ready[1]);

    if (!ok) {
      write(result_pipe[1], "F", 1);
      close(result_pipe[1]);
      _exit(1);
    }

    (void)HdClientSetTimeout(&client, 5000);

    // Receive frames until Data arrives.
    uint8_t buf[kHdMaxFramePayload];
    int buf_len = 0;
    HdFrameType ftype;
    bool got_data = false;

    for (int i = 0; i < 20; i++) {
      if (!HdClientRecvFrame(&client, &ftype, buf,
                             &buf_len, sizeof(buf))) {
        break;
      }
      if (ftype == HdFrameType::kData) {
        if (buf_len == test_data_len &&
            memcmp(buf, test_data,
                   test_data_len) == 0) {
          got_data = true;
        }
        break;
      }
    }

    HdClientClose(&client);
    write(result_pipe[1], got_data ? "P" : "F", 1);
    close(result_pipe[1]);
    _exit(got_data ? 0 : 1);
  }

  // Parent: wait for B to be ready.
  close(b_ready[1]);
  close(result_pipe[1]);

  char sig = 0;
  ASSERT_EQ(read(b_ready[0], &sig, 1), 1);
  ASSERT_EQ(sig, 'R') << "Client B failed to connect";
  close(b_ready[0]);

  // Allow time for peer registration.
  usleep(100000);

  // Fork client A (sender).
  pid_t pid_a = fork();
  ASSERT_GE(pid_a, 0);

  if (pid_a == 0) {
    close(result_pipe[0]);

    HdClient client;
    HdClientInitWithKeys(&client, pub_a, priv_a,
                         relay_key_);

    bool ok = true;
    ok = ok && test::ConnectHdClient(
        &client, "127.0.0.1", port_).has_value();
    ok = ok && HdClientUpgrade(&client).has_value();
    ok = ok && HdClientEnroll(&client).has_value();

    if (!ok) {
      _exit(1);
    }

    // Allow time for A to be registered.
    usleep(100000);

    auto rc = HdClientSendData(
        &client,
        reinterpret_cast<const uint8_t*>(test_data),
        test_data_len);

    HdClientClose(&client);
    _exit(rc.has_value() ? 0 : 1);
  }

  // Wait for sender A.
  int status_a;
  waitpid(pid_a, &status_a, 0);
  EXPECT_TRUE(WIFEXITED(status_a));
  EXPECT_EQ(WEXITSTATUS(status_a), 0)
      << "Client A (sender) failed";

  // Wait for receiver B.
  int status_b;
  waitpid(pid_b, &status_b, 0);
  EXPECT_TRUE(WIFEXITED(status_b));
  EXPECT_EQ(WEXITSTATUS(status_b), 0)
      << "Client B (receiver) failed";

  // Check B's result.
  char result = 0;
  ASSERT_EQ(read(result_pipe[0], &result, 1), 1);
  EXPECT_EQ(result, 'P')
      << "Client B did not receive expected data";
  close(result_pipe[0]);
}

TEST_F(HdE2ETest, BidirectionalRelay) {
  uint8_t pub_a[kKeySize], priv_a[kKeySize];
  uint8_t pub_b[kKeySize], priv_b[kKeySize];
  crypto_box_keypair(pub_a, priv_a);
  crypto_box_keypair(pub_b, priv_b);

  int a_ready[2], b_ready[2];
  int a_result[2], b_result[2];
  ASSERT_EQ(pipe(a_ready), 0);
  ASSERT_EQ(pipe(b_ready), 0);
  ASSERT_EQ(pipe(a_result), 0);
  ASSERT_EQ(pipe(b_result), 0);

  const char* msg_a_to_b = "hd-msg-a-to-b";
  const char* msg_b_to_a = "hd-msg-b-to-a";

  // Fork client A: sends to B, then receives from B.
  pid_t pid_a = fork();
  ASSERT_GE(pid_a, 0);

  if (pid_a == 0) {
    close(a_ready[0]);
    close(b_ready[0]);
    close(b_ready[1]);
    close(a_result[0]);
    close(b_result[0]);
    close(b_result[1]);

    HdClient client;
    HdClientInitWithKeys(&client, pub_a, priv_a,
                         relay_key_);

    bool ok = true;
    ok = ok && test::ConnectHdClient(
        &client, "127.0.0.1", port_).has_value();
    ok = ok && HdClientUpgrade(&client).has_value();
    ok = ok && HdClientEnroll(&client).has_value();

    write(a_ready[1], "R", 1);
    close(a_ready[1]);

    if (!ok) {
      write(a_result[1], "F", 1);
      close(a_result[1]);
      _exit(1);
    }

    usleep(200000);
    (void)HdClientSetTimeout(&client, 5000);

    // Send to B.
    int slen = static_cast<int>(strlen(msg_a_to_b));
    (void)HdClientSendData(
        &client,
        reinterpret_cast<const uint8_t*>(msg_a_to_b),
        slen);

    // Receive from B.
    uint8_t buf[kHdMaxFramePayload];
    int buf_len = 0;
    HdFrameType ftype;
    bool got = false;

    int rlen = static_cast<int>(strlen(msg_b_to_a));
    for (int i = 0; i < 20; i++) {
      if (!HdClientRecvFrame(&client, &ftype, buf,
                             &buf_len, sizeof(buf))) {
        break;
      }
      if (ftype == HdFrameType::kData &&
          buf_len == rlen &&
          memcmp(buf, msg_b_to_a, rlen) == 0) {
        got = true;
        break;
      }
    }

    HdClientClose(&client);
    write(a_result[1], got ? "P" : "F", 1);
    close(a_result[1]);
    _exit(got ? 0 : 1);
  }

  // Fork client B: sends to A, then receives from A.
  pid_t pid_b = fork();
  ASSERT_GE(pid_b, 0);

  if (pid_b == 0) {
    close(a_ready[0]);
    close(a_ready[1]);
    close(b_ready[0]);
    close(a_result[0]);
    close(a_result[1]);
    close(b_result[0]);

    HdClient client;
    HdClientInitWithKeys(&client, pub_b, priv_b,
                         relay_key_);

    bool ok = true;
    ok = ok && test::ConnectHdClient(
        &client, "127.0.0.1", port_).has_value();
    ok = ok && HdClientUpgrade(&client).has_value();
    ok = ok && HdClientEnroll(&client).has_value();

    write(b_ready[1], "R", 1);
    close(b_ready[1]);

    if (!ok) {
      write(b_result[1], "F", 1);
      close(b_result[1]);
      _exit(1);
    }

    usleep(200000);
    (void)HdClientSetTimeout(&client, 5000);

    // Send to A.
    int slen = static_cast<int>(strlen(msg_b_to_a));
    (void)HdClientSendData(
        &client,
        reinterpret_cast<const uint8_t*>(msg_b_to_a),
        slen);

    // Receive from A.
    uint8_t buf[kHdMaxFramePayload];
    int buf_len = 0;
    HdFrameType ftype;
    bool got = false;

    int rlen = static_cast<int>(strlen(msg_a_to_b));
    for (int i = 0; i < 20; i++) {
      if (!HdClientRecvFrame(&client, &ftype, buf,
                             &buf_len, sizeof(buf))) {
        break;
      }
      if (ftype == HdFrameType::kData &&
          buf_len == rlen &&
          memcmp(buf, msg_a_to_b, rlen) == 0) {
        got = true;
        break;
      }
    }

    HdClientClose(&client);
    write(b_result[1], got ? "P" : "F", 1);
    close(b_result[1]);
    _exit(got ? 0 : 1);
  }

  // Parent: close write ends.
  close(a_ready[1]);
  close(b_ready[1]);
  close(a_result[1]);
  close(b_result[1]);

  // Wait for both to connect.
  char sig = 0;
  ASSERT_EQ(read(a_ready[0], &sig, 1), 1);
  ASSERT_EQ(sig, 'R');
  close(a_ready[0]);

  ASSERT_EQ(read(b_ready[0], &sig, 1), 1);
  ASSERT_EQ(sig, 'R');
  close(b_ready[0]);

  // Wait for both to finish.
  int status_a, status_b;
  waitpid(pid_a, &status_a, 0);
  waitpid(pid_b, &status_b, 0);

  EXPECT_TRUE(WIFEXITED(status_a));
  EXPECT_EQ(WEXITSTATUS(status_a), 0)
      << "Client A failed";

  EXPECT_TRUE(WIFEXITED(status_b));
  EXPECT_EQ(WEXITSTATUS(status_b), 0)
      << "Client B failed";

  char ra = 0, rb = 0;
  ASSERT_EQ(read(a_result[0], &ra, 1), 1);
  ASSERT_EQ(read(b_result[0], &rb, 1), 1);
  close(a_result[0]);
  close(b_result[0]);

  EXPECT_EQ(ra, 'P') << "A did not receive B's message";
  EXPECT_EQ(rb, 'P') << "B did not receive A's message";
}

}  // namespace
}  // namespace hyper_derp
