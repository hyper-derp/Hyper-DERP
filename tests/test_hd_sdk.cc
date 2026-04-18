/// @file test_hd_sdk.cc
/// @brief Tests for the high-level HD SDK.

#include "harness.h"

#include <atomic>
#include <cstring>
#include <thread>

#include <csignal>

#include <gtest/gtest.h>
#include <sodium.h>

#include "hd_sdk.h"

namespace hyper_derp {
namespace {

class HdSdkTest : public ::testing::Test {
 protected:
  void SetUp() override {
    signal(SIGPIPE, SIG_IGN);
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

  HdSdkConfig MakeConfig() {
    HdSdkConfig cfg{};
    cfg.relay_host = "127.0.0.1";
    cfg.relay_port = port_;
    cfg.relay_key = relay_key_;
    cfg.tls = false;
    cfg.keepalive_ms = 0;
    cfg.auto_reconnect = false;
    return cfg;
  }

  Key relay_key_{};
  uint16_t port_ = 0;
  pid_t relay_pid_ = -1;
};

TEST_F(HdSdkTest, CreateAndConnect) {
  auto result = HdSdk::Create(MakeConfig());
  ASSERT_TRUE(result.has_value())
      << result.error().message;
  auto sdk = std::move(*result);
  EXPECT_TRUE(sdk.IsConnected());
}

TEST_F(HdSdkTest, StartStop) {
  auto sdk = std::move(*HdSdk::Create(MakeConfig()));
  ASSERT_TRUE(sdk.Start().has_value());
  EXPECT_TRUE(sdk.IsRunning());

  // Double start fails.
  auto r2 = sdk.Start();
  EXPECT_FALSE(r2.has_value());

  sdk.Stop();
  EXPECT_FALSE(sdk.IsRunning());
}

TEST_F(HdSdkTest, PingPong) {
  auto sdk = std::move(*HdSdk::Create(MakeConfig()));
  auto r = sdk.SendPing();
  EXPECT_TRUE(r.has_value());
}

TEST_F(HdSdkTest, PeerInfoCallback) {
  auto cfg = MakeConfig();

  // SDK A.
  auto a = std::move(*HdSdk::Create(cfg));
  std::atomic<bool> a_got_peer{false};
  uint16_t seen_peer_id = 0;
  a.OnPeerInfo([&](const PeerEvent& e) {
    seen_peer_id = e.peer_id;
    a_got_peer.store(true);
  });
  ASSERT_TRUE(a.Start().has_value());

  // SDK B connects after A is running.
  auto b = std::move(*HdSdk::Create(cfg));
  ASSERT_TRUE(b.Start().has_value());

  // Wait for A to see B.
  for (int i = 0; i < 50 && !a_got_peer.load(); i++) {
    usleep(100000);
  }
  EXPECT_TRUE(a_got_peer.load())
      << "A did not get PeerInfo for B";
  EXPECT_GT(seen_peer_id, 0);

  b.Stop();
  a.Stop();
}

TEST_F(HdSdkTest, MeshDataExchange) {
  auto cfg = MakeConfig();

  auto a = std::move(*HdSdk::Create(cfg));
  auto b = std::move(*HdSdk::Create(cfg));

  // Track peer IDs.
  std::atomic<uint16_t> b_id{0};
  std::atomic<uint16_t> a_id{0};
  a.OnPeerInfo([&](const PeerEvent& e) {
    b_id.store(e.peer_id);
  });
  b.OnPeerInfo([&](const PeerEvent& e) {
    a_id.store(e.peer_id);
  });

  // Track received MeshData.
  std::atomic<bool> b_got_msg{false};
  std::string received_msg;
  std::mutex msg_mutex;
  b.OnMeshData([&](uint16_t src, const uint8_t* data,
                   int len) {
    std::lock_guard lock(msg_mutex);
    received_msg = std::string(
        reinterpret_cast<const char*>(data), len);
    b_got_msg.store(true);
  });

  ASSERT_TRUE(a.Start().has_value());
  ASSERT_TRUE(b.Start().has_value());

  // Wait for peer discovery.
  for (int i = 0; i < 50 && b_id.load() == 0; i++) {
    usleep(100000);
  }
  ASSERT_GT(b_id.load(), 0)
      << "A did not discover B";

  // A sends MeshData to B.
  const char* msg = "hello-sdk";
  auto sr = a.SendMeshData(
      b_id.load(),
      reinterpret_cast<const uint8_t*>(msg),
      static_cast<int>(strlen(msg)));
  EXPECT_TRUE(sr.has_value());

  // Wait for B to receive.
  for (int i = 0; i < 50 && !b_got_msg.load(); i++) {
    usleep(100000);
  }
  EXPECT_TRUE(b_got_msg.load())
      << "B did not receive MeshData";
  {
    std::lock_guard lock(msg_mutex);
    EXPECT_EQ(received_msg, "hello-sdk");
  }

  b.Stop();
  a.Stop();
}

TEST_F(HdSdkTest, ConnectionCallback) {
  auto cfg = MakeConfig();
  cfg.auto_reconnect = false;
  cfg.keepalive_ms = 500;

  auto sdk = std::move(*HdSdk::Create(cfg));

  std::atomic<int> disconnects{0};
  sdk.OnConnection([&](ConnectionState state) {
    if (state == ConnectionState::kDisconnected) {
      disconnects.fetch_add(1);
    }
  });

  ASSERT_TRUE(sdk.Start().has_value());

  // Kill relay.
  test::StopRelay(relay_pid_);
  relay_pid_ = -1;

  // Wait for disconnect detection via keepalive ping fail.
  for (int i = 0; i < 30 && disconnects.load() == 0; i++) {
    usleep(200000);
  }
  EXPECT_GT(disconnects.load(), 0)
      << "No disconnect callback";

  sdk.Stop();
}

TEST_F(HdSdkTest, MoveSemantics) {
  auto sdk = std::move(*HdSdk::Create(MakeConfig()));
  EXPECT_TRUE(sdk.IsConnected());

  // Move to new variable.
  HdSdk sdk2 = std::move(sdk);
  EXPECT_TRUE(sdk2.IsConnected());
}

TEST_F(HdSdkTest, SendWhileDisconnected) {
  auto cfg = MakeConfig();
  cfg.auto_reconnect = false;
  cfg.keepalive_ms = 500;

  auto sdk = std::move(*HdSdk::Create(cfg));
  ASSERT_TRUE(sdk.Start().has_value());

  // Kill relay.
  test::StopRelay(relay_pid_);
  relay_pid_ = -1;

  // Wait for disconnect via keepalive.
  for (int i = 0; i < 30; i++) {
    usleep(200000);
    if (!sdk.IsConnected()) break;
  }

  // Send should fail cleanly.
  uint8_t data[] = {1, 2, 3};
  auto r = sdk.SendData(data, 3);
  EXPECT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, HdSdkError::NotConnected);

  sdk.Stop();
}

}  // namespace
}  // namespace hyper_derp
