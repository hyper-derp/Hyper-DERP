/// @file test_e2e.cc
/// @brief End-to-end integration tests: relay server with
///   two clients in separate processes.

#include "harness.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>
#include <sodium.h>

#include "hyper_derp/client.h"
#include "hyper_derp/ktls.h"
#include "hyper_derp/protocol.h"

namespace hyper_derp {
namespace {

class E2ETest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(sodium_init() >= 0);
    port_ = test::FindFreePort();
    ASSERT_NE(port_, 0) << "Could not find a free port";
    relay_pid_ = test::StartRelay(port_, 1);
    ASSERT_GT(relay_pid_, 0) << "Failed to fork relay";
    ASSERT_EQ(test::WaitRelayReady(port_, 5000), 0)
        << "Relay did not start on port " << port_;
  }

  void TearDown() override {
    if (relay_pid_ > 0) {
      test::StopRelay(relay_pid_);
    }
  }

  uint16_t port_ = 0;
  pid_t relay_pid_ = -1;
};

TEST_F(E2ETest, SinglePacketRelay) {
  // Pre-generate key pairs so both children know each
  // other's public key.
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

  const char* test_data = "hello-from-client-a";
  int test_data_len = static_cast<int>(strlen(test_data));

  // Fork client B (receiver).
  pid_t pid_b = fork();
  ASSERT_GE(pid_b, 0);

  if (pid_b == 0) {
    close(b_ready[0]);
    close(result_pipe[0]);

    DerpClient client;
    ClientInitWithKeys(&client, pub_b, priv_b);

    bool ok = true;
    ok = ok && ClientConnect(
        &client, "127.0.0.1", port_).has_value();
    ok = ok && ClientUpgrade(&client).has_value();
    ok = ok && ClientHandshake(&client).has_value();

    // Signal ready.
    write(b_ready[1], "R", 1);
    close(b_ready[1]);

    if (!ok) {
      write(result_pipe[1], "F", 1);
      close(result_pipe[1]);
      _exit(1);
    }

    (void)ClientSetTimeout(&client, 5000);

    // Receive frames until RecvPacket arrives.
    uint8_t buf[kMaxFramePayload];
    int buf_len = 0;
    FrameType ftype;
    bool got_packet = false;

    for (int i = 0; i < 20; i++) {
      if (!ClientRecvFrame(&client, &ftype, buf,
                           &buf_len, sizeof(buf))) {
        break;
      }
      if (ftype == FrameType::kRecvPacket) {
        // Payload: [32B src_key][data]
        if (buf_len >= kKeySize + test_data_len &&
            memcmp(buf, pub_a, kKeySize) == 0 &&
            memcmp(buf + kKeySize, test_data,
                   test_data_len) == 0) {
          got_packet = true;
        }
        break;
      }
    }

    ClientClose(&client);
    write(result_pipe[1], got_packet ? "P" : "F", 1);
    close(result_pipe[1]);
    _exit(got_packet ? 0 : 1);
  }

  // Parent: wait for B to be ready.
  close(b_ready[1]);
  close(result_pipe[1]);

  char sig = 0;
  ASSERT_EQ(read(b_ready[0], &sig, 1), 1);
  ASSERT_EQ(sig, 'R') << "Client B failed to connect";
  close(b_ready[0]);

  // Allow time for DpAddPeer to complete in the
  // data plane worker's event loop.
  usleep(100000);

  // Fork client A (sender).
  pid_t pid_a = fork();
  ASSERT_GE(pid_a, 0);

  if (pid_a == 0) {
    close(result_pipe[0]);

    DerpClient client;
    ClientInitWithKeys(&client, pub_a, priv_a);

    bool ok = true;
    ok = ok && ClientConnect(
        &client, "127.0.0.1", port_).has_value();
    ok = ok && ClientUpgrade(&client).has_value();
    ok = ok && ClientHandshake(&client).has_value();

    if (!ok) {
      _exit(1);
    }

    // Allow time for A to be registered in data plane.
    usleep(100000);

    auto rc = ClientSendPacket(
        &client, ToKey(pub_b),
        reinterpret_cast<const uint8_t*>(test_data),
        test_data_len);

    ClientClose(&client);
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
      << "Client B did not receive expected packet";
  close(result_pipe[0]);
}

TEST_F(E2ETest, BurstRelay) {
  uint8_t pub_a[kKeySize], priv_a[kKeySize];
  uint8_t pub_b[kKeySize], priv_b[kKeySize];
  crypto_box_keypair(pub_a, priv_a);
  crypto_box_keypair(pub_b, priv_b);

  int b_ready[2];
  int result_pipe[2];
  ASSERT_EQ(pipe(b_ready), 0);
  ASSERT_EQ(pipe(result_pipe), 0);

  static constexpr int kBurstCount = 100;
  static constexpr int kPayloadSize = 128;

  // Fork client B (receiver).
  pid_t pid_b = fork();
  ASSERT_GE(pid_b, 0);

  if (pid_b == 0) {
    close(b_ready[0]);
    close(result_pipe[0]);

    DerpClient client;
    ClientInitWithKeys(&client, pub_b, priv_b);

    bool ok = true;
    ok = ok && ClientConnect(
        &client, "127.0.0.1", port_).has_value();
    ok = ok && ClientUpgrade(&client).has_value();
    ok = ok && ClientHandshake(&client).has_value();

    write(b_ready[1], "R", 1);
    close(b_ready[1]);

    if (!ok) {
      write(result_pipe[1], "F", 1);
      close(result_pipe[1]);
      _exit(1);
    }

    (void)ClientSetTimeout(&client, 15000);

    uint8_t buf[kMaxFramePayload];
    int buf_len = 0;
    FrameType ftype;
    int received = 0;

    for (int attempt = 0;
         attempt < kBurstCount * 2; attempt++) {
      if (!ClientRecvFrame(&client, &ftype, buf,
                           &buf_len, sizeof(buf))) {
        break;
      }
      if (ftype == FrameType::kRecvPacket) {
        received++;
        if (received >= kBurstCount) {
          break;
        }
      }
    }

    ClientClose(&client);
    bool pass = (received == kBurstCount);
    write(result_pipe[1], pass ? "P" : "F", 1);
    close(result_pipe[1]);
    _exit(pass ? 0 : 1);
  }

  close(b_ready[1]);
  close(result_pipe[1]);

  char sig = 0;
  ASSERT_EQ(read(b_ready[0], &sig, 1), 1);
  ASSERT_EQ(sig, 'R');
  close(b_ready[0]);

  usleep(100000);

  // Fork client A (sender).
  pid_t pid_a = fork();
  ASSERT_GE(pid_a, 0);

  if (pid_a == 0) {
    close(result_pipe[0]);

    DerpClient client;
    ClientInitWithKeys(&client, pub_a, priv_a);

    bool ok = true;
    ok = ok && ClientConnect(
        &client, "127.0.0.1", port_).has_value();
    ok = ok && ClientUpgrade(&client).has_value();
    ok = ok && ClientHandshake(&client).has_value();

    if (!ok) {
      _exit(1);
    }

    usleep(100000);

    uint8_t payload[kPayloadSize];
    for (int i = 0; i < kPayloadSize; i++) {
      payload[i] = static_cast<uint8_t>(i & 0xff);
    }

    for (int i = 0; i < kBurstCount; i++) {
      // Tag with sequence number.
      payload[0] = static_cast<uint8_t>(i >> 24);
      payload[1] = static_cast<uint8_t>(i >> 16);
      payload[2] = static_cast<uint8_t>(i >> 8);
      payload[3] = static_cast<uint8_t>(i);

      if (!ClientSendPacket(&client, ToKey(pub_b),
                            payload, kPayloadSize)) {
        ClientClose(&client);
        _exit(1);
      }
      // Pace sends so the relay can drain its send
      // queue. Throughput testing uses the benchmark
      // client, not correctness tests.
      usleep(200);
    }

    ClientClose(&client);
    _exit(0);
  }

  int status_a;
  waitpid(pid_a, &status_a, 0);
  EXPECT_TRUE(WIFEXITED(status_a));
  EXPECT_EQ(WEXITSTATUS(status_a), 0)
      << "Client A (sender) failed";

  int status_b;
  waitpid(pid_b, &status_b, 0);
  EXPECT_TRUE(WIFEXITED(status_b));
  EXPECT_EQ(WEXITSTATUS(status_b), 0)
      << "Client B (receiver) failed";

  char result = 0;
  ASSERT_EQ(read(result_pipe[0], &result, 1), 1);
  EXPECT_EQ(result, 'P')
      << "Client B did not receive all " << kBurstCount
      << " packets";
  close(result_pipe[0]);
}

TEST_F(E2ETest, BidirectionalRelay) {
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

  const char* msg_a_to_b = "msg-a-to-b";
  const char* msg_b_to_a = "msg-b-to-a";

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

    DerpClient client;
    ClientInitWithKeys(&client, pub_a, priv_a);

    bool ok = true;
    ok = ok && ClientConnect(
        &client, "127.0.0.1", port_).has_value();
    ok = ok && ClientUpgrade(&client).has_value();
    ok = ok && ClientHandshake(&client).has_value();

    write(a_ready[1], "R", 1);
    close(a_ready[1]);

    if (!ok) {
      write(a_result[1], "F", 1);
      close(a_result[1]);
      _exit(1);
    }

    // Wait for orchestrator go signal via blocking read
    // on stdin — not needed, just add a delay.
    usleep(200000);
    (void)ClientSetTimeout(&client, 5000);

    // Send to B.
    int slen = static_cast<int>(strlen(msg_a_to_b));
    (void)ClientSendPacket(
        &client, ToKey(pub_b),
        reinterpret_cast<const uint8_t*>(msg_a_to_b),
        slen);

    // Receive from B.
    uint8_t buf[kMaxFramePayload];
    int buf_len = 0;
    FrameType ftype;
    bool got = false;

    int rlen = static_cast<int>(strlen(msg_b_to_a));
    for (int i = 0; i < 20; i++) {
      if (!ClientRecvFrame(&client, &ftype, buf,
                           &buf_len, sizeof(buf))) {
        break;
      }
      if (ftype == FrameType::kRecvPacket &&
          buf_len >= kKeySize + rlen &&
          memcmp(buf, pub_b, kKeySize) == 0 &&
          memcmp(buf + kKeySize, msg_b_to_a, rlen) == 0) {
        got = true;
        break;
      }
    }

    ClientClose(&client);
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

    DerpClient client;
    ClientInitWithKeys(&client, pub_b, priv_b);

    bool ok = true;
    ok = ok && ClientConnect(
        &client, "127.0.0.1", port_).has_value();
    ok = ok && ClientUpgrade(&client).has_value();
    ok = ok && ClientHandshake(&client).has_value();

    write(b_ready[1], "R", 1);
    close(b_ready[1]);

    if (!ok) {
      write(b_result[1], "F", 1);
      close(b_result[1]);
      _exit(1);
    }

    usleep(200000);
    (void)ClientSetTimeout(&client, 5000);

    // Send to A.
    int slen = static_cast<int>(strlen(msg_b_to_a));
    (void)ClientSendPacket(
        &client, ToKey(pub_a),
        reinterpret_cast<const uint8_t*>(msg_b_to_a),
        slen);

    // Receive from A.
    uint8_t buf[kMaxFramePayload];
    int buf_len = 0;
    FrameType ftype;
    bool got = false;

    int rlen = static_cast<int>(strlen(msg_a_to_b));
    for (int i = 0; i < 20; i++) {
      if (!ClientRecvFrame(&client, &ftype, buf,
                           &buf_len, sizeof(buf))) {
        break;
      }
      if (ftype == FrameType::kRecvPacket &&
          buf_len >= kKeySize + rlen &&
          memcmp(buf, pub_a, kKeySize) == 0 &&
          memcmp(buf + kKeySize, msg_a_to_b, rlen) == 0) {
        got = true;
        break;
      }
    }

    ClientClose(&client);
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
  EXPECT_EQ(WEXITSTATUS(status_a), 0) << "Client A failed";

  EXPECT_TRUE(WIFEXITED(status_b));
  EXPECT_EQ(WEXITSTATUS(status_b), 0) << "Client B failed";

  char ra = 0, rb = 0;
  ASSERT_EQ(read(a_result[0], &ra, 1), 1);
  ASSERT_EQ(read(b_result[0], &rb, 1), 1);
  close(a_result[0]);
  close(b_result[0]);

  EXPECT_EQ(ra, 'P') << "A did not receive B's message";
  EXPECT_EQ(rb, 'P') << "B did not receive A's message";
}

// -- Multi-peer / cross-shard test with 2 workers --

class E2EMultiPeerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(sodium_init() >= 0);
    port_ = test::FindFreePort();
    ASSERT_NE(port_, 0);
    relay_pid_ = test::StartRelay(port_, 2);
    ASSERT_GT(relay_pid_, 0);
    ASSERT_EQ(test::WaitRelayReady(port_, 5000), 0);
  }

  void TearDown() override {
    if (relay_pid_ > 0) {
      test::StopRelay(relay_pid_);
    }
  }

  uint16_t port_ = 0;
  pid_t relay_pid_ = -1;
};

TEST_F(E2EMultiPeerTest, MultiPeerRelay) {
  // 10 peers form 5 sender-receiver pairs.
  // Uses 2 workers, so some pairs will cross shards.
  static constexpr int kPairs = 5;
  static constexpr int kTotal = kPairs * 2;

  uint8_t pubs[kTotal][kKeySize];
  uint8_t privs[kTotal][kKeySize];
  for (int i = 0; i < kTotal; i++) {
    crypto_box_keypair(pubs[i], privs[i]);
  }

  // Result pipe: each receiver writes 'P' or 'F'.
  int result_pipe[2];
  ASSERT_EQ(pipe(result_pipe), 0);

  // Ready pipe: each peer writes 'R' when connected.
  int ready_pipe[2];
  ASSERT_EQ(pipe(ready_pipe), 0);

  pid_t pids[kTotal];

  // Fork all peers.
  for (int i = 0; i < kTotal; i++) {
    pids[i] = fork();
    ASSERT_GE(pids[i], 0);

    if (pids[i] == 0) {
      close(ready_pipe[0]);
      close(result_pipe[0]);

      DerpClient client;
      ClientInitWithKeys(&client, pubs[i], privs[i]);

      bool ok = true;
      ok = ok && ClientConnect(
          &client, "127.0.0.1", port_).has_value();
      ok = ok && ClientUpgrade(&client).has_value();
      ok = ok && ClientHandshake(&client).has_value();

      write(ready_pipe[1], "R", 1);
      close(ready_pipe[1]);

      if (!ok) {
        if (i >= kPairs) {
          write(result_pipe[1], "F", 1);
        }
        close(result_pipe[1]);
        _exit(1);
      }

      (void)ClientSetTimeout(&client, 10000);

      // Wait for all peers to connect.
      usleep(500000);

      if (i < kPairs) {
        // Sender: send one packet to the paired receiver.
        int dst = i + kPairs;
        uint8_t payload[64];
        memset(payload, 0, sizeof(payload));
        payload[0] = static_cast<uint8_t>(i);
        (void)ClientSendPacket(
            &client, ToKey(pubs[dst]),
            payload, sizeof(payload));
        usleep(500000);
      } else {
        // Receiver: wait for one RecvPacket.
        uint8_t buf[kMaxFramePayload];
        int buf_len = 0;
        FrameType ftype;
        bool got = false;

        for (int a = 0; a < 30; a++) {
          if (!ClientRecvFrame(&client, &ftype, buf,
                               &buf_len, sizeof(buf))) {
            break;
          }
          if (ftype == FrameType::kRecvPacket) {
            int src = i - kPairs;
            if (buf_len >= kKeySize + 1 &&
                memcmp(buf, pubs[src], kKeySize) == 0 &&
                buf[kKeySize] ==
                    static_cast<uint8_t>(src)) {
              got = true;
            }
            break;
          }
        }
        write(result_pipe[1], got ? "P" : "F", 1);
      }

      ClientClose(&client);
      close(result_pipe[1]);
      _exit(0);
    }
  }

  close(ready_pipe[1]);
  close(result_pipe[1]);

  // Wait for all peers to connect.
  for (int i = 0; i < kTotal; i++) {
    char sig = 0;
    ASSERT_EQ(read(ready_pipe[0], &sig, 1), 1);
    ASSERT_EQ(sig, 'R') << "Peer " << i
                         << " failed to connect";
  }
  close(ready_pipe[0]);

  // Wait for all children.
  for (int i = 0; i < kTotal; i++) {
    int status;
    waitpid(pids[i], &status, 0);
    EXPECT_TRUE(WIFEXITED(status))
        << "Peer " << i << " did not exit cleanly";
  }

  // Check receiver results.
  int received = 0;
  for (int i = 0; i < kPairs; i++) {
    char r = 0;
    ASSERT_EQ(read(result_pipe[0], &r, 1), 1);
    if (r == 'P') received++;
  }
  close(result_pipe[0]);

  EXPECT_EQ(received, kPairs)
      << "Only " << received << "/" << kPairs
      << " receivers got their packet";
}

TEST_F(E2ETest, SenderDisconnectMidBurst) {
  // Verify the relay handles a sender disconnecting
  // while sends are in-flight without crashing.
  uint8_t pub_a[kKeySize], priv_a[kKeySize];
  uint8_t pub_b[kKeySize], priv_b[kKeySize];
  crypto_box_keypair(pub_a, priv_a);
  crypto_box_keypair(pub_b, priv_b);

  int b_ready[2], result_pipe[2];
  ASSERT_EQ(pipe(b_ready), 0);
  ASSERT_EQ(pipe(result_pipe), 0);

  // Fork receiver B.
  pid_t pid_b = fork();
  ASSERT_GE(pid_b, 0);

  if (pid_b == 0) {
    close(b_ready[0]);
    close(result_pipe[0]);

    DerpClient client;
    ClientInitWithKeys(&client, pub_b, priv_b);

    bool ok = true;
    ok = ok && ClientConnect(
        &client, "127.0.0.1", port_).has_value();
    ok = ok && ClientUpgrade(&client).has_value();
    ok = ok && ClientHandshake(&client).has_value();

    write(b_ready[1], "R", 1);
    close(b_ready[1]);

    if (!ok) {
      write(result_pipe[1], "F", 1);
      close(result_pipe[1]);
      _exit(1);
    }

    (void)ClientSetTimeout(&client, 5000);

    // Receive whatever arrives. Some packets expected
    // before sender disconnects.
    uint8_t buf[kMaxFramePayload];
    int buf_len = 0;
    FrameType ftype;
    int received = 0;

    for (int a = 0; a < 200; a++) {
      if (!ClientRecvFrame(&client, &ftype, buf,
                           &buf_len, sizeof(buf))) {
        break;
      }
      if (ftype == FrameType::kRecvPacket) {
        received++;
      }
    }

    ClientClose(&client);
    // Pass if we got at least one packet and didn't hang.
    bool pass = (received >= 1);
    write(result_pipe[1], pass ? "P" : "F", 1);
    close(result_pipe[1]);
    _exit(pass ? 0 : 1);
  }

  close(b_ready[1]);
  close(result_pipe[1]);

  char sig = 0;
  ASSERT_EQ(read(b_ready[0], &sig, 1), 1);
  ASSERT_EQ(sig, 'R');
  close(b_ready[0]);

  usleep(100000);

  // Fork sender A: sends 50 packets then abruptly closes.
  pid_t pid_a = fork();
  ASSERT_GE(pid_a, 0);

  if (pid_a == 0) {
    close(result_pipe[0]);

    DerpClient client;
    ClientInitWithKeys(&client, pub_a, priv_a);

    bool ok = true;
    ok = ok && ClientConnect(
        &client, "127.0.0.1", port_).has_value();
    ok = ok && ClientUpgrade(&client).has_value();
    ok = ok && ClientHandshake(&client).has_value();

    if (!ok) _exit(1);

    usleep(100000);

    uint8_t payload[256];
    memset(payload, 0xAB, sizeof(payload));

    for (int i = 0; i < 50; i++) {
      payload[0] = static_cast<uint8_t>(i);
      if (!ClientSendPacket(&client, ToKey(pub_b),
                            payload, sizeof(payload))) {
        break;
      }
    }

    // Abrupt close — no graceful shutdown.
    close(client.fd);
    _exit(0);
  }

  int status_a;
  waitpid(pid_a, &status_a, 0);
  EXPECT_TRUE(WIFEXITED(status_a));

  int status_b;
  waitpid(pid_b, &status_b, 0);
  EXPECT_TRUE(WIFEXITED(status_b));
  EXPECT_EQ(WEXITSTATUS(status_b), 0)
      << "Receiver B failed or hung";

  char result = 0;
  ASSERT_EQ(read(result_pipe[0], &result, 1), 1);
  EXPECT_EQ(result, 'P')
      << "Receiver did not get any packets before "
      << "sender disconnect";
  close(result_pipe[0]);
}

class E2EKtlsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(sodium_init() >= 0);

    auto probe = ProbeKtls();
    if (!probe) {
      GTEST_SKIP() << "kTLS not available: "
                   << probe.error().message;
    }

    char tmpl[] = "/tmp/e2e_ktls_XXXXXX";
    char* dir = mkdtemp(tmpl);
    ASSERT_NE(dir, nullptr);
    tmpdir_ = dir;
    cert_ = tmpdir_ + "/cert.pem";
    key_ = tmpdir_ + "/key.pem";

    std::string cmd =
        "openssl req -x509 -newkey ec "
        "-pkeyopt ec_paramgen_curve:prime256v1 "
        "-keyout " + key_ +
        " -out " + cert_ +
        " -days 1 -nodes -subj '/CN=localhost' "
        "2>/dev/null";
    ASSERT_EQ(system(cmd.c_str()), 0);

    port_ = test::FindFreePort();
    ASSERT_NE(port_, 0);
    relay_pid_ = test::StartRelay(
        port_, 1, cert_.c_str(), key_.c_str());
    ASSERT_GT(relay_pid_, 0);
    ASSERT_EQ(test::WaitRelayReady(port_, 5000), 0);
  }

  void TearDown() override {
    if (relay_pid_ > 0) {
      test::StopRelay(relay_pid_);
    }
    if (!tmpdir_.empty()) {
      std::filesystem::remove_all(tmpdir_);
    }
  }

  std::string tmpdir_;
  std::string cert_;
  std::string key_;
  uint16_t port_ = 0;
  pid_t relay_pid_ = -1;
};

TEST_F(E2EKtlsTest, SinglePacketOverTls) {
  uint8_t pub_a[kKeySize], priv_a[kKeySize];
  uint8_t pub_b[kKeySize], priv_b[kKeySize];
  crypto_box_keypair(pub_a, priv_a);
  crypto_box_keypair(pub_b, priv_b);

  int b_ready[2];
  int result_pipe[2];
  ASSERT_EQ(pipe(b_ready), 0);
  ASSERT_EQ(pipe(result_pipe), 0);

  const char* test_data = "hello-over-ktls";
  int test_data_len = static_cast<int>(strlen(test_data));

  pid_t pid_b = fork();
  ASSERT_GE(pid_b, 0);

  if (pid_b == 0) {
    close(b_ready[0]);
    close(result_pipe[0]);

    DerpClient client;
    ClientInitWithKeys(&client, pub_b, priv_b);

    bool ok = true;
    ok = ok && ClientConnect(
        &client, "127.0.0.1", port_).has_value();
    ok = ok && ClientTlsConnect(&client).has_value();
    ok = ok && ClientUpgrade(&client).has_value();
    ok = ok && ClientHandshake(&client).has_value();

    write(b_ready[1], "R", 1);
    close(b_ready[1]);

    if (!ok) {
      write(result_pipe[1], "F", 1);
      close(result_pipe[1]);
      _exit(1);
    }

    (void)ClientSetTimeout(&client, 5000);

    uint8_t buf[kMaxFramePayload];
    int buf_len = 0;
    FrameType ftype;
    bool got_packet = false;

    for (int i = 0; i < 20; i++) {
      if (!ClientRecvFrame(&client, &ftype, buf,
                           &buf_len, sizeof(buf))) {
        break;
      }
      if (ftype == FrameType::kRecvPacket) {
        if (buf_len >= kKeySize + test_data_len &&
            memcmp(buf, pub_a, kKeySize) == 0 &&
            memcmp(buf + kKeySize, test_data,
                   test_data_len) == 0) {
          got_packet = true;
        }
        break;
      }
    }

    ClientClose(&client);
    write(result_pipe[1], got_packet ? "P" : "F", 1);
    close(result_pipe[1]);
    _exit(got_packet ? 0 : 1);
  }

  close(b_ready[1]);
  close(result_pipe[1]);

  char sig = 0;
  ASSERT_EQ(read(b_ready[0], &sig, 1), 1);
  ASSERT_EQ(sig, 'R');
  close(b_ready[0]);

  usleep(100000);

  pid_t pid_a = fork();
  ASSERT_GE(pid_a, 0);

  if (pid_a == 0) {
    close(result_pipe[0]);

    DerpClient client;
    ClientInitWithKeys(&client, pub_a, priv_a);

    bool ok = true;
    ok = ok && ClientConnect(
        &client, "127.0.0.1", port_).has_value();
    ok = ok && ClientTlsConnect(&client).has_value();
    ok = ok && ClientUpgrade(&client).has_value();
    ok = ok && ClientHandshake(&client).has_value();

    if (!ok) _exit(1);

    usleep(100000);

    auto rc = ClientSendPacket(
        &client, ToKey(pub_b),
        reinterpret_cast<const uint8_t*>(test_data),
        test_data_len);

    ClientClose(&client);
    _exit(rc.has_value() ? 0 : 1);
  }

  int status_a;
  waitpid(pid_a, &status_a, 0);
  EXPECT_TRUE(WIFEXITED(status_a));
  EXPECT_EQ(WEXITSTATUS(status_a), 0)
      << "Client A (sender) failed";

  int status_b;
  waitpid(pid_b, &status_b, 0);
  EXPECT_TRUE(WIFEXITED(status_b));
  EXPECT_EQ(WEXITSTATUS(status_b), 0)
      << "Client B (receiver) failed";

  char result = 0;
  ASSERT_EQ(read(result_pipe[0], &result, 1), 1);
  EXPECT_EQ(result, 'P')
      << "Client B did not receive expected packet";
  close(result_pipe[0]);
}

}  // namespace
}  // namespace hyper_derp
