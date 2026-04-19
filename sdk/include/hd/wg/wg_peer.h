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
  /// When WGEX was received; used to time out the "waiting
  /// for candidates" window before falling back to relay.
  uint64_t wgex_start_ns = 0;
  uint64_t ice_start_ns = 0;
  /// Last-handshake-time seen when direct path was tried.
  /// If wg.ko reports a newer one later, direct works.
  uint64_t ice_baseline_hs = 0;
  uint32_t direct_ip = 0;
  uint16_t direct_port = 0;

  // kDirect health tracking. We poll rx_bytes while in
  // kDirect; if tx is growing but rx has stalled for long
  // enough, the direct path broke and we switch to relay.
  uint64_t direct_last_check_ns = 0;
  uint64_t direct_last_rx_change_ns = 0;
  uint64_t direct_last_rx_bytes = 0;
  uint64_t direct_last_tx_bytes = 0;

  uint8_t active = 0;
};

/// Peer table for the daemon.
struct WgPeerTable {
  WgPeer peers[kWgMaxPeers]{};
  int count = 0;
};

/// Add a peer, or return the existing entry for this
/// public key. If `is_new` is non-null, it is set to true
/// when a fresh entry was created, false when an existing
/// entry was returned (and its hd_peer_id refreshed to the
/// passed value — relays sometimes reassign IDs across
/// reconnects).
WgPeer* WgPeerAdd(WgPeerTable* t, const Key& hd_key,
                  uint16_t hd_peer_id,
                  bool* is_new = nullptr);
WgPeer* WgPeerFind(WgPeerTable* t, const Key& hd_key);
WgPeer* WgPeerFindById(WgPeerTable* t,
                       uint16_t hd_peer_id);
void WgPeerRemove(WgPeerTable* t, const Key& hd_key);

}  // namespace hyper_derp

#endif  // CLIENT_WG_PEER_H_
