/// @file wg_peer.h
/// @brief Per-peer state machine for WireGuard tunnel setup.

#ifndef CLIENT_WG_PEER_H_
#define CLIENT_WG_PEER_H_

#include <cstdint>

#include "hyper_derp/ice.h"
#include "hyper_derp/protocol.h"

namespace hyper_derp {

enum class WgPeerState : uint8_t {
  kNew,
  kWgexSent,
  kWgexDone,
  kIceGathering,
  kIceChecking,
  kDirect,
  kRelayed,
};

inline constexpr int kWgMaxPeers = 256;

struct WgPeer {
  Key hd_key{};
  uint16_t hd_peer_id = 0;
  uint8_t wg_pubkey[32]{};
  uint32_t tunnel_ip = 0;
  WgPeerState state = WgPeerState::kNew;

  // ICE state.
  uint64_t ice_start_ns = 0;
  /// Last-handshake-time seen when direct path was tried.
  /// If wg.ko reports a newer one later, direct works.
  uint64_t ice_baseline_hs = 0;
  uint32_t direct_ip = 0;
  uint16_t direct_port = 0;

  uint8_t active = 0;
};

/// Peer table for the daemon.
struct WgPeerTable {
  WgPeer peers[kWgMaxPeers]{};
  int count = 0;
};

WgPeer* WgPeerAdd(WgPeerTable* t, const Key& hd_key,
                  uint16_t hd_peer_id);
WgPeer* WgPeerFind(WgPeerTable* t, const Key& hd_key);
WgPeer* WgPeerFindById(WgPeerTable* t,
                       uint16_t hd_peer_id);
void WgPeerRemove(WgPeerTable* t, const Key& hd_key);

}  // namespace hyper_derp

#endif  // CLIENT_WG_PEER_H_
