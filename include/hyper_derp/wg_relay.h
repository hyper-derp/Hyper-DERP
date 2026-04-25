/// @file wg_relay.h
/// @brief Transparent WireGuard packet relay.
///
/// Forwards vanilla WireGuard UDP between configured peers
/// without participating in the WireGuard cryptographic
/// handshake or holding any peer private keys. The relay
/// observes only public framing fields:
///
///   * type-1/2 handshake packets — identify destination
///     pubkey by computing MAC1 against each roster entry's
///     pubkey (BLAKE2s-keyed; the input is public).
///   * type-3/4 data packets — look up `receiver_index` in
///     a session table populated from prior handshakes.
///
/// Stock `wg-quick` clients work unmodified — they just point
/// `Endpoint` at the relay's UDP port. The relay never sees
/// plaintext payloads.

#ifndef INCLUDE_HYPER_DERP_WG_RELAY_H_
#define INCLUDE_HYPER_DERP_WG_RELAY_H_

#include <atomic>
#include <cstdint>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "hyper_derp/server.h"

namespace hyper_derp {

/// One configured peer in the WG roster.
struct WgRelayPeer {
  /// 32-byte WireGuard static public key.
  uint8_t pubkey[32];
  /// Operator-supplied label, surfaced in `wg peer list`.
  std::string label;
  /// Pinned destination endpoint. The relay sends to this
  /// 4-tuple whenever traffic is destined to this peer.
  /// Required in the v0 design; endpoint learning is a
  /// follow-up.
  struct sockaddr_storage endpoint;
  socklen_t endpoint_len = 0;
  /// Stringified `host:port` form, for display.
  std::string endpoint_str;
  /// Last time we observed traffic from this peer's
  /// configured endpoint (monotonic ns; 0 if never).
  /// Always read/written under WgRelay::peers_mu, so a
  /// plain integer is sufficient.
  uint64_t last_seen_ns = 0;
};

/// One active WireGuard session, learned by observing a
/// handshake. The relay routes data packets by the
/// receiver-side `session_id`; this table maps it to the
/// owning peer's destination endpoint so type-3/4 packets
/// can be forwarded with a single hash lookup.
struct WgRelaySession {
  uint32_t session_id = 0;
  struct sockaddr_storage endpoint{};
  socklen_t endpoint_len = 0;
  uint64_t last_seen_ns = 0;
};

/// Relay-wide aggregate counters. Read concurrently by the
/// einheit channel; written by the recv loop.
struct WgRelayStats {
  std::atomic<uint64_t> rx_packets{0};
  std::atomic<uint64_t> fwd_packets{0};
  std::atomic<uint64_t> drop_no_dst{0};
  std::atomic<uint64_t> drop_bad_form{0};
  std::atomic<uint64_t> drop_no_session{0};
};

struct WgRelay {
  int sock_fd = -1;
  uint16_t port = 0;
  std::vector<WgRelayPeer> peers;
  mutable std::mutex peers_mu;
  std::unordered_map<uint32_t, WgRelaySession> sessions;
  mutable std::mutex sessions_mu;
  WgRelayStats stats;
  std::atomic<bool> running{false};
  std::thread loop_thread;
  /// On-disk roster path. Empty disables persistence;
  /// otherwise add/remove rewrite this atomically and
  /// startup loads from it before binding.
  std::string roster_path;
};

/// Bring up the relay: bind UDP, prime the roster from cfg,
/// spawn the recv loop. Returns nullptr on failure.
WgRelay* WgRelayStart(const WgRelayConfig& cfg);

/// Stop the recv loop, close the socket, free state.
void WgRelayStop(WgRelay* r);

/// Add or replace a peer in the roster. Returns false on
/// invalid pubkey or unparseable endpoint.
bool WgRelayAddPeer(WgRelay* r, const uint8_t pubkey[32],
                    const std::string& endpoint,
                    const std::string& label);

/// Remove a peer (and any sessions that referenced them).
/// Returns true if a peer was present.
bool WgRelayRemovePeer(WgRelay* r, const uint8_t pubkey[32]);

/// Snapshot of the roster for read-only display.
struct WgRelayPeerInfo {
  uint8_t pubkey[32];
  std::string label;
  std::string endpoint;
  uint64_t last_seen_ns;
};
std::vector<WgRelayPeerInfo> WgRelayListPeers(const WgRelay* r);

/// Snapshot of stats — used by the einheit `wg show` verb.
struct WgRelayStatsSnapshot {
  uint64_t rx_packets;
  uint64_t fwd_packets;
  uint64_t drop_no_dst;
  uint64_t drop_bad_form;
  uint64_t drop_no_session;
  size_t peer_count;
  size_t session_count;
};
WgRelayStatsSnapshot WgRelayGetStats(const WgRelay* r);

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_WG_RELAY_H_
