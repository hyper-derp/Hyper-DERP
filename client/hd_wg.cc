/// @file hd_wg.cc
/// @brief hd-wg daemon: WireGuard tunnels via HD Protocol.
///
/// Connects to an HD relay, discovers peers via PeerInfo,
/// exchanges WireGuard keys via MeshData (WGEX), attempts
/// STUN hole-punching for direct UDP, falls back to relay.
///
/// ICE port inheritance: STUN socket binds wg_listen_port
/// before wg.ko, hole-punches the NAT, then wg.ko inherits
/// the mapping. Direct works through port-preserving NATs.
///
/// Non-killing fallback: relayed at 5s, ICE keeps running.
/// If direct succeeds later, promotes to direct endpoint.

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>

#include <arpa/inet.h>
#include <net/if.h>
#include <netdb.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sodium.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "hyper_derp/hd_client.h"
#include "hyper_derp/hd_protocol.h"
#include "hyper_derp/stun.h"
#include "hyper_derp/tun.h"

#include "hd/wg/wg_config.h"
#include "hd/wg/wg_netlink.h"
#include "hd/wg/wg_peer.h"
#include "hd/wg/wg_proxy.h"

using namespace hyper_derp;
using namespace std::string_view_literals;

static volatile sig_atomic_t g_stop = 0;
static void SigHandler(int) { g_stop = 1; }

static uint64_t NowMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000 +
         static_cast<uint64_t>(ts.tv_nsec) / 1000000;
}

/// WG MTU: 1500 (ethernet) - 20 (IP) - 20 (TCP) - 29 (TLS)
///   - 4 (HD header) - 6 (MeshData header) - 32 (WG outer)
///   = 1389. Round down to 1380 for safety.
inline constexpr int kWgTunnelMtu = 1380;

// -- Relay connection helper -------------------------------------------------

/// Connect to HD relay: TCP → TLS → HTTP upgrade → enroll.
/// Returns true on success. On failure, hd is closed.
static bool ConnectRelay(HdClient* hd, const char* host,
                         uint16_t port,
                         const Key& relay_key) {
  HdClientClose(hd);
  auto init = HdClientInit(hd);
  if (!init) {
    spdlog::error("hd init: {}",
                  init.error().message);
    return false;
  }
  hd->relay_key = relay_key;

  auto conn = HdClientConnect(hd, host, port);
  if (!conn) {
    spdlog::error("hd connect: {}",
                  conn.error().message);
    return false;
  }

  auto tls = HdClientTlsConnect(hd);
  if (!tls) {
    spdlog::error("hd tls: {}",
                  tls.error().message);
    HdClientClose(hd);
    return false;
  }

  auto up = HdClientUpgrade(hd);
  if (!up) {
    spdlog::error("hd upgrade: {}",
                  up.error().message);
    HdClientClose(hd);
    return false;
  }

  auto enr = HdClientEnroll(hd);
  if (!enr) {
    spdlog::error("hd enroll: {}",
                  enr.error().message);
    HdClientClose(hd);
    return false;
  }

  HdClientSetTimeout(hd, 10);
  return true;
}

/// Re-exchange WGEX with all known peers after reconnect.
static void ReannounceToAllPeers(HdClient* hd,
                                 WgPeerTable* peers,
                                 const uint8_t* wg_pubkey,
                                 uint32_t tunnel_ip,
                                 uint32_t host_ip,
                                 uint16_t host_port,
                                 uint32_t srflx_ip,
                                 uint16_t srflx_port) {
  for (int i = 0; i < kWgMaxPeers; i++) {
    auto* p = &peers->peers[i];
    if (!p->active) continue;
    // Reset state so WGEX is re-sent on next PeerInfo.
    p->state = WgPeerState::kNew;
  }
}

// -- STUN reflexive address discovery ----------------------------------------

/// Bind a UDP socket on wg_listen_port with SO_REUSEPORT,
/// send STUN binding request, parse response.
/// Returns the socket fd (caller must close before wg.ko
/// binds the same port). Sets reflexive_ip/port on success.
static int StunDiscover(uint16_t local_port,
                        const char* stun_server,
                        uint32_t* reflexive_ip,
                        uint16_t* reflexive_port) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return -1;

  int reuse = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEPORT,
             &reuse, sizeof(reuse));

  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = INADDR_ANY;
  local.sin_port = htons(local_port);
  if (bind(fd, reinterpret_cast<sockaddr*>(&local),
           sizeof(local)) < 0) {
    spdlog::warn("stun bind :{}: {}", local_port,
                 strerror(errno));
    close(fd);
    return -1;
  }

  // Resolve STUN server.
  // Format: "host:port" or just "host" (default 3478).
  std::string host_str = stun_server;
  uint16_t stun_port = 3478;
  auto colon = host_str.rfind(':');
  if (colon != std::string::npos) {
    stun_port = static_cast<uint16_t>(
        atoi(host_str.c_str() + colon + 1));
    host_str = host_str.substr(0, colon);
  }

  sockaddr_in stun_addr{};
  stun_addr.sin_family = AF_INET;
  stun_addr.sin_port = htons(stun_port);
  if (inet_pton(AF_INET, host_str.c_str(),
                &stun_addr.sin_addr) != 1) {
    // Try DNS resolution.
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host_str.c_str(), nullptr,
                    &hints, &res) != 0 || !res) {
      spdlog::warn("stun resolve failed: {}",
                   host_str);
      close(fd);
      return -1;
    }
    stun_addr.sin_addr =
        reinterpret_cast<sockaddr_in*>(
            res->ai_addr)->sin_addr;
    freeaddrinfo(res);
  }

  // Send STUN Binding Request.
  uint8_t req[64];
  int req_len = StunBuildBindingRequest(req, sizeof(req),
                                        nullptr);
  if (req_len <= 0) {
    close(fd);
    return -1;
  }

  sendto(fd, req, req_len, 0,
         reinterpret_cast<sockaddr*>(&stun_addr),
         sizeof(stun_addr));

  // Wait for response (1s timeout).
  struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
             &tv, sizeof(tv));

  uint8_t resp[256];
  int n = recv(fd, resp, sizeof(resp), 0);
  if (n <= 0) {
    spdlog::warn("stun timeout from {}", stun_server);
    close(fd);
    return fd;  // Return fd even on timeout for port inheritance.
  }

  StunMessage msg{};
  if (StunParse(resp, n, &msg) && msg.has_xor_mapped) {
    StunDecodeXorAddress(msg.xor_mapped_port,
                         msg.xor_mapped_ip,
                         reflexive_port, reflexive_ip);
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, reflexive_ip, ip_str,
              sizeof(ip_str));
    spdlog::info("stun reflexive: {}:{}",
                 ip_str, ntohs(*reflexive_port));
  }

  return fd;
}

// -- WGEX + candidate signaling ----------------------------------------------

static void SendWgex(HdClient* hd, uint16_t dst_peer_id,
                     const uint8_t* wg_pubkey,
                     uint32_t tunnel_ip) {
  uint8_t buf[kWgexSize];
  int len = WgSerializeWgex(wg_pubkey, tunnel_ip,
                            buf, sizeof(buf));
  if (len < 0) return;
  auto result = HdClientSendMeshData(
      hd, dst_peer_id, buf, len);
  if (!result) {
    spdlog::warn("failed to send WGEX to peer {}",
                 dst_peer_id);
  }
}

/// Send ICE candidates to a peer via MeshData.
/// Format: [4B "CAND"][4B ip][2B port] per candidate.
static void SendCandidates(HdClient* hd,
                           uint16_t dst_peer_id,
                           uint32_t host_ip,
                           uint16_t host_port,
                           uint32_t srflx_ip,
                           uint16_t srflx_port) {
  uint8_t buf[32];
  int off = 0;
  memcpy(buf + off, kCandMagic, 4); off += 4;
  // Host candidate.
  memcpy(buf + off, &host_ip, 4); off += 4;
  memcpy(buf + off, &host_port, 2); off += 2;
  // Server-reflexive candidate (if known).
  if (srflx_ip != 0) {
    memcpy(buf + off, &srflx_ip, 4); off += 4;
    memcpy(buf + off, &srflx_port, 2); off += 2;
  }
  HdClientSendMeshData(hd, dst_peer_id, buf, off);
}

static void HandlePeerInfo(HdClient* hd,
                           WgPeerTable* peers,
                           const uint8_t* payload,
                           int len,
                           const uint8_t* wg_pubkey,
                           uint32_t tunnel_ip,
                           uint32_t host_ip,
                           uint16_t host_port,
                           uint32_t srflx_ip,
                           uint16_t srflx_port,
                           bool force_relay) {
  if (len < 34) return;
  Key peer_key;
  memcpy(peer_key.data(), payload, 32);
  if (peer_key == hd->public_key) return;

  uint16_t peer_id =
      static_cast<uint16_t>(payload[32]) << 8 |
      payload[33];

  auto* peer = WgPeerAdd(peers, peer_key, peer_id);
  if (!peer) return;

  spdlog::info("peer {} appeared (id={})",
               peer_id, peer_id);

  if (peer->state == WgPeerState::kNew ||
      peer->state == WgPeerState::kWgexSent) {
    SendWgex(hd, peer_id, wg_pubkey, tunnel_ip);
    if (!force_relay) {
      SendCandidates(hd, peer_id, host_ip, host_port,
                     srflx_ip, srflx_port);
    }
    if (peer->state == WgPeerState::kNew)
      peer->state = WgPeerState::kWgexSent;
  }
}

static void HandleWgex(WgPeer* peer, WgNetlink* wg,
                       WgProxy* proxy,
                       const char* ifname,
                       uint16_t proxy_port,
                       uint16_t keepalive,
                       const uint8_t* data, int len,
                       HdClient* hd,
                       const uint8_t* our_wg_pubkey,
                       uint32_t our_tunnel_ip,
                       uint32_t host_ip, uint16_t host_port,
                       uint32_t srflx_ip, uint16_t srflx_port,
                       bool force_relay) {
  uint8_t wg_pubkey[32];
  uint32_t tunnel_ip;
  if (WgParseWgex(data, len, wg_pubkey, &tunnel_ip) < 0)
    return;

  // Idempotent: skip if already configured.
  if (peer->state == WgPeerState::kDirect ||
      peer->state == WgPeerState::kRelayed ||
      peer->state == WgPeerState::kIceChecking)
    return;

  memcpy(peer->wg_pubkey, wg_pubkey, 32);
  peer->tunnel_ip = tunnel_ip;

  // Send WGEX back if we haven't yet.
  if (peer->state == WgPeerState::kNew) {
    SendWgex(hd, peer->hd_peer_id, our_wg_pubkey,
             our_tunnel_ip);
    if (!force_relay) {
      SendCandidates(hd, peer->hd_peer_id,
                     host_ip, host_port,
                     srflx_ip, srflx_port);
    }
  }

  // Don't configure wg.ko yet. Wait for candidates so the
  // first endpoint is direct (if we get any) — that avoids
  // WG roaming latching onto the proxy before ICE runs.
  // If force_relay or no candidates arrive in 500ms, the
  // periodic check falls back to the proxy endpoint.
  peer->state = WgPeerState::kWgexDone;
  peer->wgex_start_ns = NowMs();

  char ip_str[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &tunnel_ip, ip_str, sizeof(ip_str));
  spdlog::info("peer {} wg tunnel {} (wgex done, "
               "waiting for candidates)",
               peer->hd_peer_id, ip_str);
}

/// Configure wg.ko with the proxy endpoint and add the peer
/// to the proxy table. Used as the relay fallback.
static void StartRelay(WgPeer* peer, WgNetlink* wg,
                       WgProxy* proxy, const char* ifname,
                       uint16_t proxy_port,
                       uint16_t keepalive) {
  WgPeerConfig wpc{};
  memcpy(wpc.public_key, peer->wg_pubkey, 32);
  wpc.endpoint_ip = htonl(INADDR_LOOPBACK);
  wpc.endpoint_port = htons(proxy_port);
  wpc.allowed_ip = peer->tunnel_ip;
  wpc.allowed_prefix = 32;
  wpc.keepalive_secs = keepalive;
  WgNlSetPeer(wg, ifname, &wpc);
  WgProxyAddPeer(proxy, peer->hd_key, peer->hd_peer_id,
                 peer->wg_pubkey, peer->tunnel_ip);
  peer->state = WgPeerState::kRelayed;
}

/// Handle received ICE candidates from a peer.
static void HandleCandidates(WgPeer* peer,
                             WgNetlink* wg,
                             const char* ifname,
                             uint16_t keepalive,
                             const uint8_t* data, int len,
                             bool force_relay) {
  if (force_relay) return;
  if (len < 4 || memcmp(data, kCandMagic, 4) != 0) return;
  data += 4; len -= 4;

  // Parse candidates: [4B ip][2B port] each.
  while (len >= 6) {
    uint32_t ip;
    uint16_t port;
    memcpy(&ip, data, 4);
    memcpy(&port, data + 2 + 2, 2);
    // Actually: [4B ip][2B port]
    memcpy(&ip, data, 4);
    memcpy(&port, data + 4, 2);
    data += 6; len -= 6;

    // Configure WG with the direct endpoint. Only honored
    // when we already have the peer's WG info (kWgexDone)
    // or we previously went to relay but haven't yet seen
    // any direct attempt.
    bool eligible =
        peer->state == WgPeerState::kWgexDone ||
        peer->state == WgPeerState::kRelayed;
    if (eligible && ip != 0 && port != 0) {
      WgPeerConfig wpc{};
      memcpy(wpc.public_key, peer->wg_pubkey, 32);
      wpc.endpoint_ip = ip;
      wpc.endpoint_port = port;
      wpc.allowed_ip = peer->tunnel_ip;
      wpc.allowed_prefix = 32;
      wpc.keepalive_secs = keepalive;

      // Baseline the handshake time before switching so a
      // fresh one is proof the direct endpoint works.
      uint64_t baseline = 0;
      WgNlGetPeerHandshake(wg, ifname, peer->wg_pubkey,
                           &baseline);

      auto result = WgNlSetPeer(wg, ifname, &wpc);
      if (result) {
        peer->direct_ip = ip;
        peer->direct_port = ntohs(port);
        peer->ice_baseline_hs = baseline;
        peer->ice_start_ns = NowMs();
        peer->state = WgPeerState::kIceChecking;

        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &ip, ip_str, sizeof(ip_str));
        spdlog::info("peer {} trying direct {}:{}",
                     peer->hd_peer_id, ip_str,
                     ntohs(port));
      }
      return;  // Try one candidate at a time.
    }
  }
}

static void HandlePeerGone(WgPeerTable* peers,
                           WgNetlink* wg, WgProxy* proxy,
                           const char* ifname,
                           const uint8_t* payload,
                           int len) {
  if (len < 32) return;
  Key peer_key;
  memcpy(peer_key.data(), payload, 32);
  auto* peer = WgPeerFind(peers, peer_key);
  if (!peer) return;

  WgNlRemovePeer(wg, ifname, peer->wg_pubkey);
  WgProxyRemovePeer(proxy, peer->wg_pubkey);
  WgPeerRemove(peers, peer_key);
  spdlog::info("peer removed");
}

/// Check if a peer's WG handshake succeeded after the
/// endpoint was switched to a direct candidate. Queries
/// wg.ko for last-handshake-time: if non-zero since the
/// switch, the direct path works and we promote. Otherwise
/// after 5s we revert the endpoint to the proxy so WG
/// traffic keeps flowing through the relay.
static void CheckDirectPromotion(WgPeer* peer,
                                 WgNetlink* wg,
                                 WgProxy* proxy,
                                 const char* ifname,
                                 uint16_t proxy_port,
                                 uint16_t keepalive) {
  // No candidates arrived within the window — configure
  // the relay path so traffic can start flowing.
  if (peer->state == WgPeerState::kWgexDone &&
      NowMs() - peer->wgex_start_ns > 500) {
    StartRelay(peer, wg, proxy, ifname, proxy_port,
               keepalive);
    spdlog::info("peer {} no candidates, using relay",
                 peer->hd_peer_id);
    return;
  }

  // Health-check an established direct path. If we're
  // sending but rx_bytes hasn't changed for ~15s, the
  // direct endpoint has gone away — switch to relay.
  if (peer->state == WgPeerState::kDirect) {
    uint64_t now = NowMs();
    if (now - peer->direct_last_check_ns < 2000) return;
    peer->direct_last_check_ns = now;

    WgPeerStats stats;
    WgNlGetPeerStats(wg, ifname, peer->wg_pubkey, &stats);

    // Initial sample: seed the baseline and return.
    if (peer->direct_last_rx_change_ns == 0) {
      peer->direct_last_rx_bytes = stats.rx_bytes;
      peer->direct_last_tx_bytes = stats.tx_bytes;
      peer->direct_last_rx_change_ns = now;
      return;
    }

    if (stats.rx_bytes != peer->direct_last_rx_bytes) {
      peer->direct_last_rx_bytes = stats.rx_bytes;
      peer->direct_last_rx_change_ns = now;
    }
    bool tx_growing =
        stats.tx_bytes > peer->direct_last_tx_bytes;
    peer->direct_last_tx_bytes = stats.tx_bytes;

    if (tx_growing &&
        now - peer->direct_last_rx_change_ns > 15000) {
      // Wipe the stale direct-path session so WG will
      // initiate a fresh handshake on the new endpoint.
      WgNlRemovePeer(wg, ifname, peer->wg_pubkey);
      StartRelay(peer, wg, proxy, ifname, proxy_port,
                 keepalive);
      peer->direct_last_rx_change_ns = 0;
      // Tell the peer so it also resets its direct
      // session — otherwise it keeps trying to reply to
      // the dead direct endpoint.
      HdClientSendMeshData(proxy->hd, peer->hd_peer_id,
                           kFallMagic, 4);
      spdlog::info("peer {} direct path stalled, "
                   "falling back to relay",
                   peer->hd_peer_id);
    }
    return;
  }

  if (peer->state != WgPeerState::kIceChecking) return;

  uint64_t elapsed = NowMs() - peer->ice_start_ns;
  uint64_t hs_sec = 0;
  WgNlGetPeerHandshake(wg, ifname, peer->wg_pubkey, &hs_sec);

  if (hs_sec > peer->ice_baseline_hs) {
    peer->state = WgPeerState::kDirect;
    peer->direct_last_check_ns = 0;
    peer->direct_last_rx_change_ns = 0;
    char ip_str[INET_ADDRSTRLEN];
    struct in_addr a = {peer->direct_ip};
    inet_ntop(AF_INET, &a, ip_str, sizeof(ip_str));
    spdlog::info("peer {} promoted to direct ({}:{})",
                 peer->hd_peer_id, ip_str,
                 peer->direct_port);
    return;
  }

  if (elapsed > 5000) {
    StartRelay(peer, wg, proxy, ifname, proxy_port,
               keepalive);
    spdlog::info("peer {} direct failed, fell back to relay",
                 peer->hd_peer_id);
  }
}

// -- CLI args ----------------------------------------------------------------

static void PrintUsage() {
  fprintf(stderr,
      "Usage: hd-wg [options]\n"
      "  --config PATH        YAML config file\n"
      "  --relay-host HOST    Relay IP\n"
      "  --relay-port PORT    Relay port (3341)\n"
      "  --relay-key HEX      Relay shared secret\n"
      "  --wg-key HEX         WireGuard private key\n"
      "  --wg-interface NAME  WG interface (wg0)\n"
      "  --wg-port PORT       WG listen port (51820)\n"
      "  --tunnel CIDR        Tunnel address\n"
      "  --proxy-port PORT    UDP proxy port (51821)\n"
      "  --stun SERVER        STUN server (host:port)\n"
      "  --keepalive SECS     WG keepalive (25)\n"
      "  --force-relay        Always relay WG via HD (no "
      "ICE direct path)\n"
      "  --help\n");
}

// -- Main --------------------------------------------------------------------

int main(int argc, char** argv) {
  WgDaemonConfig cfg;

  // Load config file first, then override with CLI args.
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
      if (!WgLoadConfig(argv[i + 1], &cfg)) {
        return 1;
      }
      spdlog::info("loaded config from {}", argv[i + 1]);
      break;
    }
  }

  for (int i = 1; i < argc; i++) {
    auto arg = std::string_view(argv[i]);
    if (arg == "--config"sv && i + 1 < argc) {
      i++;  // Already loaded.
    } else if (arg == "--relay-host"sv && i + 1 < argc) {
      cfg.relay_host = argv[++i];
    } else if (arg == "--relay-port"sv && i + 1 < argc) {
      cfg.relay_port =
          static_cast<uint16_t>(atoi(argv[++i]));
    } else if (arg == "--relay-key"sv && i + 1 < argc) {
      cfg.relay_key_hex = argv[++i];
    } else if (arg == "--wg-key"sv && i + 1 < argc) {
      cfg.wg_private_key_hex = argv[++i];
    } else if (arg == "--wg-interface"sv && i + 1 < argc) {
      cfg.wg_interface = argv[++i];
    } else if (arg == "--wg-port"sv && i + 1 < argc) {
      cfg.wg_listen_port =
          static_cast<uint16_t>(atoi(argv[++i]));
    } else if (arg == "--tunnel"sv && i + 1 < argc) {
      cfg.tunnel_cidr = argv[++i];
    } else if (arg == "--proxy-port"sv && i + 1 < argc) {
      cfg.proxy_port =
          static_cast<uint16_t>(atoi(argv[++i]));
    } else if (arg == "--stun"sv && i + 1 < argc) {
      cfg.stun_server = argv[++i];
    } else if (arg == "--keepalive"sv && i + 1 < argc) {
      cfg.keepalive_secs = atoi(argv[++i]);
    } else if (arg == "--force-relay"sv) {
      cfg.force_relay = true;
    } else if (arg == "--help"sv) {
      PrintUsage();
      return 0;
    } else {
      fprintf(stderr, "unknown: %s\n", argv[i]);
      PrintUsage();
      return 1;
    }
  }

  if (cfg.relay_host.empty() ||
      cfg.relay_key_hex.empty() ||
      cfg.wg_private_key_hex.empty() ||
      cfg.tunnel_cidr.empty()) {
    fprintf(stderr, "error: --relay-host, --relay-key, "
            "--wg-key, --tunnel are required\n");
    PrintUsage();
    return 1;
  }

  Key relay_key{};
  if (cfg.relay_key_hex.size() != 64 ||
      sodium_hex2bin(relay_key.data(), 32,
                     cfg.relay_key_hex.c_str(), 64,
                     nullptr, nullptr, nullptr) != 0) {
    fprintf(stderr, "error: invalid relay key\n");
    return 1;
  }

  uint8_t wg_private[32];
  if (cfg.wg_private_key_hex.size() != 64 ||
      sodium_hex2bin(wg_private, 32,
                     cfg.wg_private_key_hex.c_str(), 64,
                     nullptr, nullptr, nullptr) != 0) {
    fprintf(stderr, "error: invalid WG private key\n");
    return 1;
  }

  uint8_t wg_public[32];
  crypto_scalarmult_base(wg_public, wg_private);

  uint32_t tunnel_ip;
  int prefix_len;
  if (ParseCidr(cfg.tunnel_cidr.c_str(),
                &tunnel_ip, &prefix_len) != 0) {
    fprintf(stderr, "error: invalid tunnel CIDR\n");
    return 1;
  }

  signal(SIGINT, SigHandler);
  signal(SIGTERM, SigHandler);
  signal(SIGPIPE, SIG_IGN);

  spdlog::info("hd-wg starting{}",
               cfg.force_relay ? " (force-relay)" : "");

  // -------------------------------------------------------
  // Phase 1: STUN discovery (port inheritance trick).
  // Bind UDP on wg_listen_port BEFORE wg.ko, discover
  // our reflexive address, then close the socket so
  // wg.ko can inherit the NAT mapping.
  // -------------------------------------------------------

  uint32_t srflx_ip = 0;
  uint16_t srflx_port = 0;
  int stun_fd = -1;

  if (!cfg.stun_server.empty()) {
    stun_fd = StunDiscover(cfg.wg_listen_port,
                           cfg.stun_server.c_str(),
                           &srflx_ip, &srflx_port);
  }

  // Get our host IP for candidate exchange.
  uint32_t host_ip = 0;
  {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s >= 0) {
      sockaddr_in dst{};
      dst.sin_family = AF_INET;
      dst.sin_addr.s_addr = inet_addr("8.8.8.8");
      dst.sin_port = htons(53);
      if (connect(s, reinterpret_cast<sockaddr*>(&dst),
                  sizeof(dst)) == 0) {
        sockaddr_in local{};
        socklen_t len = sizeof(local);
        getsockname(s,
            reinterpret_cast<sockaddr*>(&local), &len);
        host_ip = local.sin_addr.s_addr;
      }
      close(s);
    }
  }
  uint16_t host_port = htons(cfg.wg_listen_port);

  if (host_ip != 0) {
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &host_ip, ip_str, sizeof(ip_str));
    spdlog::info("host candidate: {}:{}",
                 ip_str, cfg.wg_listen_port);
  }

  // Close STUN socket — wg.ko will bind the same port.
  // NAT mapping persists briefly.
  if (stun_fd >= 0) {
    close(stun_fd);
    stun_fd = -1;
  }

  // -------------------------------------------------------
  // Phase 2: Create WireGuard interface.
  // -------------------------------------------------------

  WgNetlink wg{};
  auto wg_init = WgNlInit(&wg);
  if (!wg_init) {
    spdlog::error("netlink init: {}",
                  wg_init.error().message);
    return 1;
  }

  auto wg_dev = WgNlCreateDevice(
      cfg.wg_interface.c_str());
  if (!wg_dev) {
    spdlog::error("create device: {}",
                  wg_dev.error().message);
    WgNlClose(&wg);
    return 1;
  }

  auto wg_set = WgNlSetDevice(
      &wg, cfg.wg_interface.c_str(),
      wg_private, cfg.wg_listen_port);
  if (!wg_set) {
    spdlog::error("set device: {}",
                  wg_set.error().message);
    WgNlClose(&wg);
    return 1;
  }

  auto wg_addr = WgNlConfigureAddr(
      cfg.wg_interface.c_str(), tunnel_ip, prefix_len);
  if (!wg_addr) {
    spdlog::error("configure addr: {}",
                  wg_addr.error().message);
    WgNlClose(&wg);
    return 1;
  }

  // Set WG interface MTU to account for MeshData + TLS.
  {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock >= 0) {
      struct ifreq ifr{};
      strncpy(ifr.ifr_name, cfg.wg_interface.c_str(),
              IFNAMSIZ - 1);
      ifr.ifr_mtu = kWgTunnelMtu;
      if (ioctl(sock, SIOCSIFMTU, &ifr) == 0) {
        spdlog::info("wg0 mtu set to {}", kWgTunnelMtu);
      }
      close(sock);
    }
  }

  // -------------------------------------------------------
  // Phase 3: Connect to HD relay.
  // -------------------------------------------------------

  HdClient hd{};
  if (!ConnectRelay(&hd, cfg.relay_host.c_str(),
                    cfg.relay_port, relay_key)) {
    WgNlClose(&wg);
    return 1;
  }
  spdlog::info("enrolled with relay (peer_id={})",
               hd.peer_id);

  // -------------------------------------------------------
  // Phase 4: UDP proxy + main loop.
  // -------------------------------------------------------

  WgProxy proxy{};
  auto px = WgProxyInit(&proxy, cfg.proxy_port,
                        cfg.wg_listen_port, &hd);
  if (!px) {
    spdlog::error("proxy init: {}",
                  px.error().message);
    HdClientClose(&hd);
    WgNlClose(&wg);
    return 1;
  }

  WgPeerTable peers{};
  bool connected = true;
  int reconnect_backoff = 1;
  uint64_t last_ping = NowMs();

  spdlog::info("hd-wg running (tunnel {}, proxy "
               "127.0.0.1:{}, mtu {})",
               cfg.tunnel_cidr, cfg.proxy_port,
               kWgTunnelMtu);

  uint8_t frame_buf[kHdMaxFramePayload];
  int frame_len;
  HdFrameType frame_type;

  while (!g_stop) {
    // Reconnect if disconnected.
    if (!connected) {
      spdlog::info("reconnecting in {}s...",
                   reconnect_backoff);
      for (int i = 0; i < reconnect_backoff * 5 &&
           !g_stop; i++) {
        usleep(200000);
      }
      if (g_stop) break;

      if (ConnectRelay(&hd, cfg.relay_host.c_str(),
                       cfg.relay_port, relay_key)) {
        spdlog::info("reconnected (peer_id={})",
                     hd.peer_id);
        proxy.hd = &hd;
        connected = true;
        reconnect_backoff = 1;
        last_ping = NowMs();
        // Reset peer states so WGEX re-exchanges.
        ReannounceToAllPeers(&hd, &peers, wg_public,
            tunnel_ip, host_ip, host_port,
            srflx_ip, srflx_port);
      } else {
        reconnect_backoff =
            reconnect_backoff < 30
                ? reconnect_backoff * 2 : 30;
      }
      continue;
    }

    pollfd fds[2];
    fds[0].fd = hd.ssl ? SSL_get_fd(hd.ssl) : hd.fd;
    fds[0].events = POLLIN;
    fds[1].fd = proxy.udp_fd;
    fds[1].events = POLLIN;

    int ret = poll(fds, 2, 5);
    if (ret < 0) {
      if (errno == EINTR) continue;
      break;
    }

    // Periodic ping for keepalive / liveness detection.
    if (NowMs() - last_ping > 10000) {
      auto pr = HdClientSendPing(&hd);
      if (!pr) {
        spdlog::warn("ping failed, relay connection lost");
        connected = false;
        continue;
      }
      last_ping = NowMs();
    }

    // HD frames.
    if (fds[0].revents & POLLIN) {
      // Mark whether this was the first RecvFrame call
      // (where we expect data from the kernel). Subsequent
      // calls should only process frames that are already
      // decoded in HdClient's buffer — otherwise we'd block
      // inside SSL_read for SO_RCVTIMEO.
      bool first = true;
      while (!g_stop) {
        if (!first && hd.recv_len == hd.recv_pos) {
          break;
        }
        first = false;
        auto rv = HdClientRecvFrame(
            &hd, &frame_type, frame_buf,
            &frame_len, sizeof(frame_buf));
        if (!rv) {
          // Check if it's a real disconnect vs timeout.
          if (!hd.connected) {
            spdlog::warn("relay disconnected");
            connected = false;
          }
          break;
        }

        if (frame_type == HdFrameType::kPeerInfo) {
          HandlePeerInfo(&hd, &peers, frame_buf,
                         frame_len, wg_public, tunnel_ip,
                         host_ip, host_port,
                         srflx_ip, srflx_port,
                         cfg.force_relay);
        } else if (frame_type == HdFrameType::kMeshData) {
          if (frame_len < 2) continue;
          uint16_t src_id =
              static_cast<uint16_t>(frame_buf[0]) << 8 |
              frame_buf[1];
          const uint8_t* payload = frame_buf + 2;
          int payload_len = frame_len - 2;

          if (payload_len >= 4 &&
              memcmp(payload, kWgexMagic, 4) == 0) {
            auto* peer = WgPeerFindById(&peers, src_id);
            if (peer) {
              HandleWgex(peer, &wg, &proxy,
                         cfg.wg_interface.c_str(),
                         cfg.proxy_port,
                         cfg.keepalive_secs,
                         payload, payload_len,
                         &hd, wg_public, tunnel_ip,
                         host_ip, host_port,
                         srflx_ip, srflx_port,
                         cfg.force_relay);
            }
          } else if (payload_len >= 4 &&
                     memcmp(payload, kCandMagic, 4) == 0) {
            auto* peer = WgPeerFindById(&peers, src_id);
            if (peer) {
              HandleCandidates(peer, &wg,
                               cfg.wg_interface.c_str(),
                               cfg.keepalive_secs,
                               payload, payload_len,
                               cfg.force_relay);
            }
          } else if (payload_len >= 4 &&
                     memcmp(payload, kFallMagic, 4) == 0) {
            auto* peer = WgPeerFindById(&peers, src_id);
            if (peer &&
                peer->state == WgPeerState::kDirect) {
              WgNlRemovePeer(&wg,
                             cfg.wg_interface.c_str(),
                             peer->wg_pubkey);
              StartRelay(peer, &wg, &proxy,
                         cfg.wg_interface.c_str(),
                         cfg.proxy_port,
                         cfg.keepalive_secs);
              peer->direct_last_rx_change_ns = 0;
              spdlog::info("peer {} peer-requested relay "
                           "fallback",
                           peer->hd_peer_id);
            }
          } else {
            WgProxyHandleHd(&proxy, src_id,
                            payload, payload_len);
          }
        } else if (frame_type == HdFrameType::kPeerGone) {
          HandlePeerGone(&peers, &wg, &proxy,
                         cfg.wg_interface.c_str(),
                         frame_buf, frame_len);
        }
      }
    }

    // UDP from wireguard.ko — drain all queued packets.
    if (fds[1].revents & POLLIN) {
      while (WgProxyHandleUdp(&proxy) == 0) {}
    }

    // Periodic: check ICE promotions.
    for (int i = 0; i < kWgMaxPeers; i++) {
      if (!peers.peers[i].active) continue;
      CheckDirectPromotion(&peers.peers[i], &wg, &proxy,
                           cfg.wg_interface.c_str(),
                           cfg.proxy_port,
                           cfg.keepalive_secs);
    }
  }

  spdlog::info("shutting down");
  WgProxyClose(&proxy);
  HdClientClose(&hd);
  WgNlClose(&wg);
  return 0;
}
