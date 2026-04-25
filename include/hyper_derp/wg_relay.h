/// @file wg_relay.h
/// @brief Transparent UDP relay for stock WireGuard.
///
/// The relay is operator-driven: the operator registers
/// named peers (with their pinned ip:port endpoints) and
/// declares links between pairs. Forwarding is pure
/// 4-tuple: a packet arriving from a registered peer's
/// endpoint goes to that peer's link partner.
///
/// The relay observes nothing about WireGuard. No MAC1,
/// no session tracking, no crypto. WireGuard endpoints on
/// each side terminate the tunnel between themselves; the
/// relay just shuffles UDP.
///
/// Iteration 1 enforces "each peer is in at most one
/// link" so a packet's destination is unambiguous from
/// the source endpoint alone. Multi-link mesh routing is
/// future work — it requires either per-link UDP ports
/// or some form of in-packet introspection.

#ifndef INCLUDE_HYPER_DERP_WG_RELAY_H_
#define INCLUDE_HYPER_DERP_WG_RELAY_H_

#include <atomic>
#include <cstdint>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <thread>
#include <vector>

#include "hyper_derp/server.h"

namespace hyper_derp {

/// One configured peer in the WG roster.
struct WgRelayPeer {
  /// Operator-assigned unique name, used by `wg link`
  /// and surfaced in `wg peer list`.
  std::string name;
  /// Pinned source endpoint: source 4-tuple of incoming
  /// UDP must match this for the packet to be considered
  /// "from this peer."
  struct sockaddr_storage endpoint{};
  socklen_t endpoint_len = 0;
  /// Stringified `host:port`, kept for display.
  std::string endpoint_str;
  /// Optional WG public key (base64, as `wg genkey`
  /// emits). Pure metadata — used only by
  /// `wg show config` to render [Peer] blocks for the
  /// operator. The forwarder ignores it.
  std::string pubkey_b64;
  /// Operator-supplied label. Optional.
  std::string label;
  /// Last time we successfully forwarded a packet from
  /// this peer (steady-clock ns; 0 if never).
  uint64_t last_seen_ns = 0;
  /// Cumulative byte counters.
  uint64_t rx_bytes = 0;
  uint64_t fwd_bytes = 0;
};

/// One operator-declared forwarding link between two
/// peers. Iteration 1 invariant: every peer name appears
/// in at most one link.
struct WgRelayLink {
  std::string a;
  std::string b;
};

/// Aggregate counters surfaced via the einheit channel.
struct WgRelayStats {
  std::atomic<uint64_t> rx_packets{0};
  std::atomic<uint64_t> fwd_packets{0};
  std::atomic<uint64_t> drop_unknown_src{0};
  std::atomic<uint64_t> drop_no_link{0};
};

struct WgRelay {
  int sock_fd = -1;
  uint16_t port = 0;
  std::vector<WgRelayPeer> peers;
  std::vector<WgRelayLink> links;
  /// peers_mu guards peers + links + roster_path writes.
  /// All operator-side mutations and the recv loop's
  /// per-packet lookups serialize on it; the lookup is
  /// O(N) but N is small (operator-supplied peer roster).
  mutable std::mutex peers_mu;
  WgRelayStats stats;
  std::atomic<bool> running{false};
  std::thread loop_thread;
  std::string roster_path;
};

/// Bring up the relay: load roster, bind UDP, spawn
/// recv loop. Returns nullptr on failure.
WgRelay* WgRelayStart(const WgRelayConfig& cfg);

/// Stop the recv loop, close the socket, free state.
void WgRelayStop(WgRelay* r);

/// Operator-facing mutations. All return false on
/// duplicate / invalid / link-invariant violations; on
/// success the on-disk roster is rewritten atomically.
bool WgRelayPeerAdd(WgRelay* r, const std::string& name,
                     const std::string& endpoint,
                     const std::string& label);
bool WgRelayPeerKey(WgRelay* r, const std::string& name,
                     const std::string& pubkey_b64);
bool WgRelayPeerRemove(WgRelay* r,
                        const std::string& name);
bool WgRelayLinkAdd(WgRelay* r, const std::string& a,
                     const std::string& b);
bool WgRelayLinkRemove(WgRelay* r, const std::string& a,
                        const std::string& b);

/// Read-only snapshots for `wg peer list`,
/// `wg link list`, `wg show`, `wg show config`.
struct WgRelayPeerInfo {
  std::string name;
  std::string endpoint;
  std::string pubkey_b64;
  std::string label;
  uint64_t last_seen_ns;
  uint64_t rx_bytes;
  uint64_t fwd_bytes;
  std::string linked_to;  // name of peer this is linked to, or empty
};
std::vector<WgRelayPeerInfo> WgRelayListPeers(
    const WgRelay* r);

struct WgRelayLinkInfo {
  std::string a;
  std::string b;
};
std::vector<WgRelayLinkInfo> WgRelayListLinks(
    const WgRelay* r);

/// Build the wg-quick [Peer] block(s) for `name` based
/// on the peer's links. Returns the rendered config text
/// or empty if the peer is unknown.
std::string WgRelayShowConfig(const WgRelay* r,
                               const std::string& name,
                               const std::string& relay_advertised_endpoint);

struct WgRelayStatsSnapshot {
  uint64_t rx_packets;
  uint64_t fwd_packets;
  uint64_t drop_unknown_src;
  uint64_t drop_no_link;
  size_t peer_count;
  size_t link_count;
};
WgRelayStatsSnapshot WgRelayGetStats(const WgRelay* r);

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_WG_RELAY_H_
