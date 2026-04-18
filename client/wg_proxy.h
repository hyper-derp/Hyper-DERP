/// @file wg_proxy.h
/// @brief UDP proxy: bridge wireguard.ko ↔ HD MeshData.
///
/// wireguard.ko sends encrypted UDP to 127.0.0.1:proxy_port.
/// The proxy wraps it in HD MeshData and sends to the relay.
/// Incoming MeshData is unwrapped and sent to WG's listen port.

#ifndef CLIENT_WG_PROXY_H_
#define CLIENT_WG_PROXY_H_

#include <cstdint>
#include <expected>

#include "hyper_derp/error.h"
#include "hyper_derp/hd_client.h"
#include "hyper_derp/protocol.h"

namespace hyper_derp {

inline constexpr int kWgMaxProxyPeers = 256;

/// WGEX key exchange magic.
inline constexpr uint8_t kWgexMagic[4] = {'W', 'G', 'E', 'X'};
inline constexpr int kWgexSize = 40;  // 4B magic + 32B key + 4B IP

/// ICE candidate exchange magic.
inline constexpr uint8_t kCandMagic[4] = {'C', 'A', 'N', 'D'};

enum class WgProxyError {
  SocketFailed,
  BindFailed,
};

/// Per-peer proxy state.
struct WgProxyPeer {
  Key hd_key{};
  uint16_t hd_peer_id = 0;
  uint8_t wg_pubkey[32]{};
  uint32_t tunnel_ip = 0;
  /// Source port wireguard.ko uses for this peer.
  /// Learned from the first UDP packet.
  uint16_t wg_src_port = 0;
  uint8_t active = 0;
};

/// UDP proxy state.
struct WgProxy {
  int udp_fd = -1;
  uint16_t listen_port = 0;
  uint16_t wg_listen_port = 0;
  HdClient* hd = nullptr;
  WgProxyPeer peers[kWgMaxProxyPeers]{};
  int peer_count = 0;
};

/// Bind the UDP proxy socket on 127.0.0.1:port.
auto WgProxyInit(WgProxy* proxy, uint16_t port,
                 uint16_t wg_listen_port, HdClient* hd)
    -> std::expected<void, Error<WgProxyError>>;

/// Register a peer for proxying.
WgProxyPeer* WgProxyAddPeer(WgProxy* proxy,
                            const Key& hd_key,
                            uint16_t hd_peer_id,
                            const uint8_t* wg_pubkey,
                            uint32_t tunnel_ip);

/// Remove a peer.
void WgProxyRemovePeer(WgProxy* proxy,
                       const uint8_t* wg_pubkey);

/// Find peer by HD peer ID.
WgProxyPeer* WgProxyFindByHdId(WgProxy* proxy,
                               uint16_t peer_id);

/// Handle one UDP datagram from wireguard.ko.
/// Reads from udp_fd, wraps in MeshData, sends to relay.
/// Returns 0 on success, -1 on error or no data.
int WgProxyHandleUdp(WgProxy* proxy);

/// Handle one HD MeshData payload from relay.
/// Sends as UDP to wireguard.ko's listen port.
/// Returns 0 on success, -1 on error.
int WgProxyHandleHd(WgProxy* proxy,
                    uint16_t src_peer_id,
                    const uint8_t* data, int len);

/// Serialize WGEX key exchange message.
int WgSerializeWgex(const uint8_t* wg_pubkey,
                    uint32_t tunnel_ip,
                    uint8_t* buf, int buf_size);

/// Parse WGEX key exchange message.
/// Returns 0 on success, -1 if not WGEX.
int WgParseWgex(const uint8_t* data, int len,
                uint8_t* wg_pubkey,
                uint32_t* tunnel_ip);

/// Close proxy socket.
void WgProxyClose(WgProxy* proxy);

}  // namespace hyper_derp

#endif  // CLIENT_WG_PROXY_H_
