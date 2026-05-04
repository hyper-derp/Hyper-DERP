/// @file test_wg_relay_trace.cc
/// @brief Smoke tests for the wg-relay diagnostic surface
///        added in the 0.2.2 daemon brief: trace-forward-
///        hashes flag (P0.1) and per-peer drop counters
///        (P0.1).
///
/// These exercise the wg-relay forwarder in-process so the
/// new logging path and per-peer counters are reachable.
/// They do not assert exact log strings — the brief only
/// requires that the path is reachable.

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>

#include "hyper_derp/server.h"
#include "hyper_derp/wg_relay.h"

namespace hyper_derp {
namespace {

// Bind a UDP socket on loopback and return (fd, port). Caller
// owns the fd; close it when done.
std::pair<int, uint16_t> BindUdpEphemeral() {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return {-1, 0};
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr),
            sizeof(addr)) < 0) {
    close(fd);
    return {-1, 0};
  }
  socklen_t len = sizeof(addr);
  getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
  return {fd, ntohs(addr.sin_port)};
}

// Build a 148-byte WireGuard-shaped handshake init. The
// payload is junk crypto — the relay forwarder doesn't decrypt
// it, only checks shape (length + first byte == 1).
std::array<uint8_t, 148> MakeHandshakeInit() {
  std::array<uint8_t, 148> p{};
  p[0] = 1;  // type = init
  for (size_t i = 4; i < p.size(); ++i) {
    p[i] = static_cast<uint8_t>(i & 0xff);
  }
  return p;
}

class WgRelayTraceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto [a_fd, a_port] = BindUdpEphemeral();
    auto [b_fd, b_port] = BindUdpEphemeral();
    ASSERT_GE(a_fd, 0);
    ASSERT_GE(b_fd, 0);
    alice_fd_ = a_fd;
    alice_port_ = a_port;
    bob_fd_ = b_fd;
    bob_port_ = b_port;
  }

  // Pick a UDP port for the relay by binding-then-closing.
  // Tests that need to run WgRelayStart call this each time
  // so the kernel hands back a fresh ephemeral port — the
  // historical "pick once at SetUp" pattern has a TOCTOU
  // window that flakes when another process grabs the port
  // between the test's close and the relay's bind.
  void StartRelay(WgRelayConfig cfg) {
    for (int attempt = 0; attempt < 5; ++attempt) {
      auto [fd, port] = BindUdpEphemeral();
      ASSERT_GE(fd, 0);
      relay_port_ = port;
      close(fd);
      cfg.port = port;
      relay_ = WgRelayStart(cfg);
      if (relay_) return;
    }
    FAIL() << "WgRelayStart failed 5 times in a row";
  }

  void TearDown() override {
    if (relay_) WgRelayStop(relay_);
    if (alice_fd_ >= 0) close(alice_fd_);
    if (bob_fd_ >= 0) close(bob_fd_);
  }

  WgRelayConfig MakeCfg() {
    WgRelayConfig cfg;
    // cfg.port is filled in by StartRelay(); leave 0 here.
    WgRelayConfig::PeerEntry alice;
    alice.name = "alice";
    alice.endpoint =
        std::format("127.0.0.1:{}", alice_port_);
    cfg.peers.push_back(std::move(alice));
    WgRelayConfig::PeerEntry bob;
    bob.name = "bob";
    bob.endpoint =
        std::format("127.0.0.1:{}", bob_port_);
    cfg.peers.push_back(std::move(bob));
    cfg.links.push_back({"alice", "bob"});
    return cfg;
  }

  ssize_t SendFromAlice(
      const std::array<uint8_t, 148>& pkt) {
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    to.sin_port = htons(relay_port_);
    return sendto(alice_fd_, pkt.data(), pkt.size(), 0,
                   reinterpret_cast<sockaddr*>(&to),
                   sizeof(to));
  }

  // Wait up to 500 ms for a packet on bob's socket. Returns
  // the number of bytes received, or -1 on timeout.
  ssize_t RecvOnBob(uint8_t* buf, size_t bufsz) {
    timeval tv{0, 500'000};
    setsockopt(bob_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv,
                sizeof(tv));
    return recv(bob_fd_, buf, bufsz, 0);
  }

  int alice_fd_ = -1;
  int bob_fd_ = -1;
  uint16_t alice_port_ = 0;
  uint16_t bob_port_ = 0;
  uint16_t relay_port_ = 0;
  WgRelay* relay_ = nullptr;
};

// Wait briefly for fwd_packets to reach `target`. The recv
// loop bumps the counter just AFTER sendto returns, so a
// sub-microsecond test that reads stats immediately after
// recv() returns can race the increment. Poll up to ~500 ms.
uint64_t WaitFwdPackets(WgRelay* r, uint64_t target) {
  for (int i = 0; i < 50; ++i) {
    auto s = WgRelayGetStats(r);
    if (s.fwd_packets >= target) return s.fwd_packets;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return WgRelayGetStats(r).fwd_packets;
}

// Trace flag off (default) — relay still forwards.
TEST_F(WgRelayTraceTest, ForwardsWithTraceOff) {
  WgRelayConfig cfg = MakeCfg();
  cfg.trace_forward_hashes = false;
  StartRelay(std::move(cfg));
  ASSERT_NE(relay_, nullptr);

  auto pkt = MakeHandshakeInit();
  ASSERT_EQ(SendFromAlice(pkt), 148);

  std::array<uint8_t, 256> buf{};
  ssize_t n = RecvOnBob(buf.data(), buf.size());
  ASSERT_EQ(n, 148);
  EXPECT_EQ(buf[0], 1);
  EXPECT_EQ(WaitFwdPackets(relay_, 1), 1u);
}

// Trace flag on — relay forwards and the trace path runs
// without crashing. We don't assert exact log strings (per the
// brief); reaching this assertion proves the new code path is
// live.
TEST_F(WgRelayTraceTest, ForwardsWithTraceOn) {
  WgRelayConfig cfg = MakeCfg();
  cfg.trace_forward_hashes = true;
  StartRelay(std::move(cfg));
  ASSERT_NE(relay_, nullptr);

  auto pkt = MakeHandshakeInit();
  ASSERT_EQ(SendFromAlice(pkt), 148);

  std::array<uint8_t, 256> buf{};
  ssize_t n = RecvOnBob(buf.data(), buf.size());
  ASSERT_EQ(n, 148);
  EXPECT_EQ(WaitFwdPackets(relay_, 1), 1u);
}

// P0.2 smoke: invalid --xdp-mode causes WgRelayStart to fail.
// Exercises the ParseXdpMode validation path; the daemon
// surfacing this means main.cc returns EXIT_FAILURE rather
// than silently running with a bogus mode.
TEST_F(WgRelayTraceTest, RejectsUnknownXdpMode) {
  WgRelayConfig cfg = MakeCfg();
  cfg.port = 0;  // not exercised; bind never reached.
  cfg.xdp_interface = "lo";
  cfg.xdp_mode = "extreme";
  WgRelay* r = WgRelayStart(cfg);
  EXPECT_EQ(r, nullptr);
}

// P0.2 smoke: --xdp-mode=off skips XDP attach entirely even
// when --xdp-interface is set. The relay still comes up; the
// userspace recv loop handles every packet. Operator opts in
// to "no XDP" explicitly, rather than getting it as a silent
// fallback.
TEST_F(WgRelayTraceTest, XdpModeOffSkipsAttach) {
  WgRelayConfig cfg = MakeCfg();
  cfg.xdp_interface = "lo";
  cfg.xdp_mode = "off";
  StartRelay(std::move(cfg));
  ASSERT_NE(relay_, nullptr);
  auto stats = WgRelayGetStats(relay_);
  EXPECT_FALSE(stats.xdp_attached);
}

// Per-source-IP histogram bumps drop_not_wg_shaped when an
// unrecognised packet hits the relay's port. The brief
// flagged this counter as needing more granularity than
// aggregate; surface it via WgRelayListDropSources.
TEST_F(WgRelayTraceTest, PerSrcIpDropHistogram) {
  WgRelayConfig cfg = MakeCfg();
  StartRelay(std::move(cfg));
  ASSERT_NE(relay_, nullptr);

  // Send a single byte 0x05 (not a valid WG type) — fails the
  // shape filter, increments drop_not_wg_shaped per-source.
  uint8_t junk = 0x05;
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  to.sin_port = htons(relay_port_);
  ASSERT_EQ(sendto(alice_fd_, &junk, 1, 0,
                    reinterpret_cast<sockaddr*>(&to),
                    sizeof(to)),
            1);
  // Recv loop runs; let it process.
  for (int i = 0; i < 50; ++i) {
    auto rows = WgRelayListDropSources(relay_);
    if (!rows.empty()) {
      EXPECT_GE(rows[0].drop_not_wg_shaped, 1u);
      EXPECT_EQ(rows[0].ip, "127.0.0.1");
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  FAIL() << "drop_by_src histogram never populated";
}

// Per-peer drop_no_link counter increments when a registered
// peer sends with no link configured for it. Counter is
// surfaced via WgRelayListPeers.
TEST_F(WgRelayTraceTest, PerPeerDropNoLinkCounter) {
  WgRelayConfig cfg = MakeCfg();
  // Drop the link so alice is registered but unlinked.
  cfg.links.clear();
  StartRelay(std::move(cfg));
  ASSERT_NE(relay_, nullptr);

  auto pkt = MakeHandshakeInit();
  ASSERT_EQ(SendFromAlice(pkt), 148);
  // Give the recv loop a moment to process.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  auto peers = WgRelayListPeers(relay_);
  uint64_t alice_drop_no_link = 0;
  for (const auto& p : peers) {
    if (p.name == "alice") alice_drop_no_link = p.drop_no_link;
  }
  EXPECT_EQ(alice_drop_no_link, 1u);
}

}  // namespace
}  // namespace hyper_derp
