/// @file hd_wg.cc
/// @brief hd-wg daemon: WireGuard tunnels via HD Protocol.
///
/// Connects to an HD relay, discovers peers via PeerInfo,
/// exchanges WireGuard keys via MeshData (WGEX), attempts
/// STUN hole-punching for direct UDP, falls back to relay.
///
/// Usage:
///   hd-wg --relay-host 10.50.0.2 --relay-port 3341 \
///     --relay-key <hex> --wg-key <hex> \
///     --tunnel 10.99.0.1/24

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>

#include <arpa/inet.h>
#include <poll.h>
#include <sodium.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "hyper_derp/hd_client.h"
#include "hyper_derp/hd_protocol.h"
#include "hyper_derp/tun.h"

#include "wg_config.h"
#include "wg_netlink.h"
#include "wg_peer.h"
#include "wg_proxy.h"

using namespace hyper_derp;
using namespace std::string_view_literals;

static volatile sig_atomic_t g_stop = 0;
static void SigHandler(int) { g_stop = 1; }

// -- WGEX signaling ----------------------------------------------------------

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

static void HandlePeerInfo(HdClient* hd,
                           WgPeerTable* peers,
                           const uint8_t* payload,
                           int len,
                           const uint8_t* wg_pubkey,
                           uint32_t tunnel_ip) {
  if (len < 32) return;
  Key peer_key;
  memcpy(peer_key.data(), payload, 32);

  // Ignore self.
  if (peer_key == hd->public_key) return;

  // Read peer_id from the 2 bytes after the key.
  uint16_t peer_id = 0;
  if (len >= 34) {
    peer_id = static_cast<uint16_t>(payload[32]) << 8 |
              payload[33];
  }

  auto* peer = WgPeerAdd(peers, peer_key, peer_id);
  if (!peer) {
    spdlog::warn("peer table full");
    return;
  }

  spdlog::info("peer {} appeared (id={})",
               peer_id, peer_id);

  // Send our WG key exchange.
  if (peer->state == WgPeerState::kNew) {
    SendWgex(hd, peer_id, wg_pubkey, tunnel_ip);
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
                       uint32_t our_tunnel_ip) {
  uint8_t wg_pubkey[32];
  uint32_t tunnel_ip;
  if (WgParseWgex(data, len, wg_pubkey, &tunnel_ip) < 0) {
    return;
  }

  memcpy(peer->wg_pubkey, wg_pubkey, 32);
  peer->tunnel_ip = tunnel_ip;

  // If we haven't sent WGEX yet, send it now.
  if (peer->state == WgPeerState::kNew) {
    SendWgex(hd, peer->hd_peer_id, our_wg_pubkey,
             our_tunnel_ip);
  }

  // Configure wireguard.ko with this peer.
  // Initially route through the local UDP proxy (relay).
  // Direct path via ICE is phase 2.
  WgPeerConfig wpc{};
  memcpy(wpc.public_key, wg_pubkey, 32);
  wpc.endpoint_ip = htonl(INADDR_LOOPBACK);
  wpc.endpoint_port = htons(proxy_port);
  wpc.allowed_ip = tunnel_ip;
  wpc.allowed_prefix = 32;
  wpc.keepalive_secs = keepalive;

  auto result = WgNlSetPeer(wg, ifname, &wpc);
  if (!result) {
    spdlog::error("failed to configure WG peer: {}",
                  result.error().message);
    return;
  }

  // Register in proxy for relay forwarding.
  WgProxyAddPeer(proxy, peer->hd_key, peer->hd_peer_id,
                 wg_pubkey, tunnel_ip);

  peer->state = WgPeerState::kRelayed;

  char ip_str[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &tunnel_ip, ip_str, sizeof(ip_str));
  spdlog::info("peer {} configured: wg tunnel {} "
               "(relayed)", peer->hd_peer_id, ip_str);
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

// -- CLI args ----------------------------------------------------------------

static void PrintUsage() {
  fprintf(stderr,
      "Usage: hd-wg [options]\n"
      "  --relay-host HOST    Relay IP\n"
      "  --relay-port PORT    Relay port (3341)\n"
      "  --relay-key HEX      Relay shared secret\n"
      "  --wg-key HEX         WireGuard private key\n"
      "  --wg-interface NAME  WG interface (wg0)\n"
      "  --wg-port PORT       WG listen port (51820)\n"
      "  --tunnel CIDR        Tunnel address (10.99.0.1/24)\n"
      "  --proxy-port PORT    UDP proxy port (51821)\n"
      "  --keepalive SECS     WG keepalive (25)\n"
      "  --help\n");
}

// -- Main loop ---------------------------------------------------------------

int main(int argc, char** argv) {
  WgDaemonConfig cfg;

  // Parse CLI args.
  for (int i = 1; i < argc; i++) {
    auto arg = std::string_view(argv[i]);
    if (arg == "--relay-host"sv && i + 1 < argc) {
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
    } else if (arg == "--keepalive"sv && i + 1 < argc) {
      cfg.keepalive_secs = atoi(argv[++i]);
    } else if (arg == "--help"sv) {
      PrintUsage();
      return 0;
    } else {
      fprintf(stderr, "unknown: %s\n", argv[i]);
      PrintUsage();
      return 1;
    }
  }

  // Validate required args.
  if (cfg.relay_host.empty() ||
      cfg.relay_key_hex.empty() ||
      cfg.wg_private_key_hex.empty() ||
      cfg.tunnel_cidr.empty()) {
    fprintf(stderr, "error: --relay-host, --relay-key, "
            "--wg-key, --tunnel are required\n");
    PrintUsage();
    return 1;
  }

  // Parse relay key.
  Key relay_key{};
  if (cfg.relay_key_hex.size() != 64 ||
      sodium_hex2bin(relay_key.data(), 32,
                     cfg.relay_key_hex.c_str(), 64,
                     nullptr, nullptr, nullptr) != 0) {
    fprintf(stderr, "error: invalid relay key\n");
    return 1;
  }

  // Parse WG private key.
  uint8_t wg_private[32];
  if (cfg.wg_private_key_hex.size() != 64 ||
      sodium_hex2bin(wg_private, 32,
                     cfg.wg_private_key_hex.c_str(), 64,
                     nullptr, nullptr, nullptr) != 0) {
    fprintf(stderr, "error: invalid WG private key\n");
    return 1;
  }

  // Derive WG public key.
  uint8_t wg_public[32];
  crypto_scalarmult_base(wg_public, wg_private);

  // Parse tunnel CIDR.
  uint32_t tunnel_ip;
  int prefix_len;
  if (ParseCidr(cfg.tunnel_cidr.c_str(),
                &tunnel_ip, &prefix_len) != 0) {
    fprintf(stderr, "error: invalid tunnel CIDR\n");
    return 1;
  }

  // Signal handling.
  signal(SIGINT, SigHandler);
  signal(SIGTERM, SigHandler);
  signal(SIGPIPE, SIG_IGN);

  spdlog::info("hd-wg starting");

  // 1. Create WireGuard interface.
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

  // 2. Connect to HD relay.
  HdClient hd{};
  auto hd_init = HdClientInit(&hd);
  if (!hd_init) {
    spdlog::error("hd init: {}",
                  hd_init.error().message);
    WgNlClose(&wg);
    return 1;
  }
  hd.relay_key = relay_key;

  auto hd_conn = HdClientConnect(
      &hd, cfg.relay_host.c_str(), cfg.relay_port);
  if (!hd_conn) {
    spdlog::error("hd connect: {}",
                  hd_conn.error().message);
    WgNlClose(&wg);
    return 1;
  }

  auto hd_tls = HdClientTlsConnect(&hd);
  if (!hd_tls) {
    spdlog::error("hd tls: {}",
                  hd_tls.error().message);
    HdClientClose(&hd);
    WgNlClose(&wg);
    return 1;
  }

  auto hd_up = HdClientUpgrade(&hd);
  if (!hd_up) {
    spdlog::error("hd upgrade: {}",
                  hd_up.error().message);
    HdClientClose(&hd);
    WgNlClose(&wg);
    return 1;
  }

  auto hd_enr = HdClientEnroll(&hd);
  if (!hd_enr) {
    spdlog::error("hd enroll: {}",
                  hd_enr.error().message);
    HdClientClose(&hd);
    WgNlClose(&wg);
    return 1;
  }

  spdlog::info("enrolled with relay (peer_id={})",
               hd.peer_id);

  // 3. Start UDP proxy.
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

  // Set non-blocking recv timeout on HD client.
  HdClientSetTimeout(&hd, 100);

  WgPeerTable peers{};

  spdlog::info("hd-wg running (tunnel {}, proxy "
               "127.0.0.1:{})",
               cfg.tunnel_cidr, cfg.proxy_port);

  // 4. Main loop.
  pollfd fds[2];
  fds[0].fd = hd.ssl ? SSL_get_fd(hd.ssl) : hd.fd;
  fds[0].events = POLLIN;
  fds[1].fd = proxy.udp_fd;
  fds[1].events = POLLIN;

  uint8_t frame_buf[kHdMaxFramePayload];
  int frame_len;
  HdFrameType frame_type;

  while (!g_stop) {
    int ret = poll(fds, 2, 200);
    if (ret < 0) {
      if (errno == EINTR) continue;
      break;
    }

    // HD frames.
    if (fds[0].revents & POLLIN) {
      while (!g_stop) {
        auto rv = HdClientRecvFrame(
            &hd, &frame_type, frame_buf,
            &frame_len, sizeof(frame_buf));
        if (!rv) break;

        if (frame_type == HdFrameType::kPeerInfo) {
          HandlePeerInfo(&hd, &peers, frame_buf,
                         frame_len, wg_public, tunnel_ip);
        } else if (frame_type == HdFrameType::kMeshData) {
          // MeshData: [2B src_peer_id][payload]
          if (frame_len < 2) continue;
          uint16_t src_id =
              static_cast<uint16_t>(frame_buf[0]) << 8 |
              frame_buf[1];
          const uint8_t* payload = frame_buf + 2;
          int payload_len = frame_len - 2;

          // Check for WGEX magic.
          if (payload_len >= 4 &&
              memcmp(payload, kWgexMagic, 4) == 0) {
            auto* peer = WgPeerFindById(&peers, src_id);
            if (peer) {
              HandleWgex(peer, &wg, &proxy,
                         cfg.wg_interface.c_str(),
                         cfg.proxy_port,
                         cfg.keepalive_secs,
                         payload, payload_len,
                         &hd, wg_public, tunnel_ip);
            }
          } else {
            // Relay traffic — forward to WG.
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

    // UDP from wireguard.ko.
    if (fds[1].revents & POLLIN) {
      WgProxyHandleUdp(&proxy);
    }
  }

  spdlog::info("shutting down");
  WgProxyClose(&proxy);
  HdClientClose(&hd);
  WgNlClose(&wg);
  return 0;
}
