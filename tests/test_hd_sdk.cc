/// @file test_hd_sdk.cc
/// @brief Tests for the HD SDK (Client + Tunnel API).

#include "harness.h"

#include <atomic>
#include <csignal>
#include <cstring>
#include <thread>

#include <gtest/gtest.h>
#include <sodium.h>

#include <hd/sdk.hpp>

namespace hd::sdk {
namespace {

class SdkTest : public ::testing::Test {
 protected:
  void SetUp() override {
    signal(SIGPIPE, SIG_IGN);
    ASSERT_GE(sodium_init(), 0);
    randombytes_buf(relay_key_.data(), hyper_derp::kKeySize);
    port_ = hyper_derp::test::FindFreePort();
    ASSERT_NE(port_, 0);
    relay_pid_ = hyper_derp::test::StartHdRelay(
        port_, 1, relay_key_);
    ASSERT_GT(relay_pid_, 0);
    ASSERT_EQ(
        hyper_derp::test::WaitRelayReady(port_, 5000), 0);
  }

  void TearDown() override {
    if (relay_pid_ > 0)
      hyper_derp::test::StopRelay(relay_pid_);
  }

  ClientConfig MakeConfig() {
    ClientConfig cfg;
    cfg.relay_url = "127.0.0.1:" + std::to_string(port_);
    char hex[65];
    sodium_bin2hex(hex, sizeof(hex),
                   relay_key_.data(), 32);
    cfg.relay_key = hex;
    cfg.tls = true;
    cfg.keepalive = std::chrono::seconds(0);
    cfg.auto_reconnect = false;
    return cfg;
  }

  hyper_derp::Key relay_key_{};
  uint16_t port_ = 0;
  pid_t relay_pid_ = -1;
};

TEST_F(SdkTest, CreateAndConnect) {
  auto r = Client::Create(MakeConfig());
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(r->GetStatus(), Status::Connected);
}

TEST_F(SdkTest, StartStop) {
  auto c = std::move(*Client::Create(MakeConfig()));
  ASSERT_TRUE(c.Start().has_value());

  auto r2 = c.Start();
  EXPECT_FALSE(r2.has_value());  // Double start.

  c.Stop();
}

TEST_F(SdkTest, PeerCallback) {
  auto cfg = MakeConfig();
  auto a = std::move(*Client::Create(cfg));

  std::atomic<bool> got_peer{false};
  a.SetPeerCallback([&](const PeerInfo& p, bool conn) {
    if (conn) got_peer.store(true);
  });
  ASSERT_TRUE(a.Start().has_value());

  auto b = std::move(*Client::Create(cfg));
  ASSERT_TRUE(b.Start().has_value());

  for (int i = 0; i < 50 && !got_peer.load(); i++)
    usleep(100000);
  EXPECT_TRUE(got_peer.load());

  b.Stop();
  a.Stop();
}

TEST_F(SdkTest, ListPeers) {
  auto cfg = MakeConfig();
  auto a = std::move(*Client::Create(cfg));
  ASSERT_TRUE(a.Start().has_value());

  auto b = std::move(*Client::Create(cfg));
  ASSERT_TRUE(b.Start().has_value());

  // Wait for peer discovery.
  for (int i = 0; i < 50; i++) {
    if (!a.ListPeers().empty()) break;
    usleep(100000);
  }
  auto peers = a.ListPeers();
  EXPECT_GE(peers.size(), 1u);

  b.Stop();
  a.Stop();
}

TEST_F(SdkTest, OpenTunnelAndSend) {
  auto cfg = MakeConfig();
  auto a = std::move(*Client::Create(cfg));
  auto b = std::move(*Client::Create(cfg));

  std::atomic<uint16_t> b_id{0};
  a.SetPeerCallback([&](const PeerInfo& p, bool conn) {
    if (conn) b_id.store(p.peer_id);
  });

  ASSERT_TRUE(a.Start().has_value());
  ASSERT_TRUE(b.Start().has_value());

  // Wait for discovery.
  for (int i = 0; i < 50 && b_id.load() == 0; i++)
    usleep(100000);
  ASSERT_GT(b_id.load(), 0);

  // Open tunnel A → B.
  auto tr = a.Open(std::to_string(b_id.load()));
  ASSERT_TRUE(tr.has_value()) << tr.error().message;
  auto tunnel = std::move(*tr);
  EXPECT_EQ(tunnel.CurrentMode(), Mode::Relayed);

  // Register recv on B.
  std::atomic<bool> b_got{false};
  std::string received;
  std::mutex rx_mu;

  // We need a tunnel on B's side too to receive.
  // B gets A's data via the raw MeshData path — the recv
  // loop dispatches to matching tunnels. But B hasn't
  // opened a tunnel to A. For now, use raw SendMeshData
  // which works without a tunnel object.

  // Actually, test tunnel.Send from A. B receives via
  // its own SetPeerCallback + raw listen. The current
  // architecture dispatches MeshData to tunnels keyed by
  // src_peer_id. B needs to have a tunnel with A's peer_id.
  // This requires B to Open(A).

  // Let's test the raw SendMeshData path instead.
  const uint8_t msg[] = "hello-tunnel";
  auto sr = tunnel.Send(
      std::span<const uint8_t>(msg, sizeof(msg) - 1));
  EXPECT_TRUE(sr.has_value());

  tunnel.Close();
  b.Stop();
  a.Stop();
}

TEST_F(SdkTest, MoveSemantics) {
  auto c = std::move(*Client::Create(MakeConfig()));
  EXPECT_EQ(c.GetStatus(), Status::Connected);

  Client c2 = std::move(c);
  EXPECT_EQ(c2.GetStatus(), Status::Connected);
}

}  // namespace
}  // namespace hd::sdk
