/// @file wg_proxy.cc
/// @brief UDP proxy bridging wireguard.ko to HD MeshData.

#include "wg_proxy.h"

#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

namespace hyper_derp {

auto WgProxyInit(WgProxy* proxy, uint16_t port,
                 uint16_t wg_listen_port, HdClient* hd)
    -> std::expected<void, Error<WgProxyError>> {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return std::unexpected(MakeError(
        WgProxyError::SocketFailed,
        "socket: " + std::string(strerror(errno))));
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);

  if (bind(fd, reinterpret_cast<sockaddr*>(&addr),
           sizeof(addr)) < 0) {
    close(fd);
    return std::unexpected(MakeError(
        WgProxyError::BindFailed,
        "bind 127.0.0.1:" + std::to_string(port) +
            ": " + std::string(strerror(errno))));
  }

  proxy->udp_fd = fd;
  proxy->listen_port = port;
  proxy->wg_listen_port = wg_listen_port;
  proxy->hd = hd;
  proxy->peer_count = 0;

  spdlog::info("wg proxy listening on 127.0.0.1:{}",
               port);
  return {};
}

WgProxyPeer* WgProxyAddPeer(WgProxy* proxy,
                            const Key& hd_key,
                            uint16_t hd_peer_id,
                            const uint8_t* wg_pubkey,
                            uint32_t tunnel_ip) {
  // Check for existing peer.
  for (int i = 0; i < kWgMaxProxyPeers; i++) {
    if (proxy->peers[i].active &&
        proxy->peers[i].hd_peer_id == hd_peer_id) {
      return &proxy->peers[i];
    }
  }

  // Find empty slot.
  for (int i = 0; i < kWgMaxProxyPeers; i++) {
    if (!proxy->peers[i].active) {
      auto* p = &proxy->peers[i];
      p->hd_key = hd_key;
      p->hd_peer_id = hd_peer_id;
      memcpy(p->wg_pubkey, wg_pubkey, 32);
      p->tunnel_ip = tunnel_ip;
      p->wg_src_port = 0;
      p->active = 1;
      proxy->peer_count++;
      return p;
    }
  }
  return nullptr;
}

void WgProxyRemovePeer(WgProxy* proxy,
                       const uint8_t* wg_pubkey) {
  for (int i = 0; i < kWgMaxProxyPeers; i++) {
    if (proxy->peers[i].active &&
        memcmp(proxy->peers[i].wg_pubkey,
               wg_pubkey, 32) == 0) {
      proxy->peers[i].active = 0;
      proxy->peer_count--;
      return;
    }
  }
}

WgProxyPeer* WgProxyFindByHdId(WgProxy* proxy,
                               uint16_t peer_id) {
  for (int i = 0; i < kWgMaxProxyPeers; i++) {
    if (proxy->peers[i].active &&
        proxy->peers[i].hd_peer_id == peer_id) {
      return &proxy->peers[i];
    }
  }
  return nullptr;
}

static WgProxyPeer* FindBySrcPort(WgProxy* proxy,
                                  uint16_t port) {
  for (int i = 0; i < kWgMaxProxyPeers; i++) {
    if (proxy->peers[i].active &&
        proxy->peers[i].wg_src_port == port) {
      return &proxy->peers[i];
    }
  }
  return nullptr;
}

int WgProxyHandleUdp(WgProxy* proxy) {
  uint8_t buf[2048];
  sockaddr_in src{};
  socklen_t slen = sizeof(src);

  int n = recvfrom(proxy->udp_fd, buf, sizeof(buf), 0,
                   reinterpret_cast<sockaddr*>(&src),
                   &slen);
  if (n <= 0) return -1;

  uint16_t src_port = ntohs(src.sin_port);

  // Look up peer by source port.
  WgProxyPeer* peer = FindBySrcPort(proxy, src_port);
  if (!peer) {
    // First packet from WG for some peer. WG uses one
    // ephemeral port per peer endpoint. Try to match by
    // finding a peer with no learned src_port yet.
    for (int i = 0; i < kWgMaxProxyPeers; i++) {
      if (proxy->peers[i].active &&
          proxy->peers[i].wg_src_port == 0) {
        proxy->peers[i].wg_src_port = src_port;
        peer = &proxy->peers[i];
        spdlog::debug("learned wg src port {} for peer {}",
                      src_port, peer->hd_peer_id);
        break;
      }
    }
  }
  if (!peer) return -1;

  // Wrap in MeshData and send via HD relay.
  auto result = HdClientSendMeshData(
      proxy->hd, peer->hd_peer_id, buf, n);
  if (!result) return -1;
  return 0;
}

int WgProxyHandleHd(WgProxy* proxy,
                    uint16_t src_peer_id,
                    const uint8_t* data, int len) {
  // Send UDP to wireguard.ko's listen port.
  sockaddr_in dst{};
  dst.sin_family = AF_INET;
  dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  dst.sin_port = htons(proxy->wg_listen_port);

  int sent = sendto(proxy->udp_fd, data, len, 0,
                    reinterpret_cast<sockaddr*>(&dst),
                    sizeof(dst));
  if (sent < 0) return -1;
  return 0;
}

// -- WGEX serialization ------------------------------------------------------

int WgSerializeWgex(const uint8_t* wg_pubkey,
                    uint32_t tunnel_ip,
                    uint8_t* buf, int buf_size) {
  if (buf_size < kWgexSize) return -1;
  memcpy(buf, kWgexMagic, 4);
  memcpy(buf + 4, wg_pubkey, 32);
  memcpy(buf + 36, &tunnel_ip, 4);
  return kWgexSize;
}

int WgParseWgex(const uint8_t* data, int len,
                uint8_t* wg_pubkey,
                uint32_t* tunnel_ip) {
  if (len < kWgexSize) return -1;
  if (memcmp(data, kWgexMagic, 4) != 0) return -1;
  memcpy(wg_pubkey, data + 4, 32);
  memcpy(tunnel_ip, data + 36, 4);
  return 0;
}

void WgProxyClose(WgProxy* proxy) {
  if (proxy->udp_fd >= 0) {
    close(proxy->udp_fd);
    proxy->udp_fd = -1;
  }
}

}  // namespace hyper_derp
