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
#include <map>
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
  /// Operator-assigned NIC name for this peer's L2
  /// segment. Empty = use the first xdp_interface (the
  /// default for backward-compatible single-NIC setups).
  /// Populates the BPF peer entry's ifindex so
  /// XDP_REDIRECT can pick the right egress NIC.
  std::string nic;
  /// Times this peer's `endpoint` was relearned via the
  /// MAC1-driven roaming flow. Persisted to the roster.
  uint64_t endpoint_relearn = 0;
  /// Pending relearn-candidate, populated when an unknown
  /// source presents a handshake with valid MAC1 against
  /// this peer's link partner. Cleared on confirm (transport
  /// data flowed from the candidate) or expiry (no transport
  /// data within 30 s). The committed `endpoint` above stays
  /// untouched until confirm.
  struct sockaddr_storage candidate_endpoint{};
  socklen_t candidate_endpoint_len = 0;
  uint64_t candidate_set_ns = 0;
  /// Steady-clock ns of the last completed relearn — gates
  /// new candidate registrations against rapid flapping.
  uint64_t last_relearn_ns = 0;
};

/// One operator-declared forwarding link between two
/// peers. Iteration 1 invariant: every peer name appears
/// in at most one link.
struct WgRelayLink {
  std::string a;
  std::string b;
};

/// Aggregate counters surfaced via the einheit channel.
/// These cover the userspace forwarding path. When XDP is
/// attached the bulk of forwarded packets bypass userspace
/// entirely and are counted in WgXdpStats below.
struct WgRelayStats {
  std::atomic<uint64_t> rx_packets{0};
  std::atomic<uint64_t> fwd_packets{0};
  std::atomic<uint64_t> drop_unknown_src{0};
  std::atomic<uint64_t> drop_no_link{0};
  /// First-byte / length sanity check — drops packets whose
  /// shape doesn't match a WireGuard message type. Mirrors the
  /// XDP STAT_DROP_NOT_WG_SHAPED counter for the userspace
  /// fallback path.
  std::atomic<uint64_t> drop_not_wg_shaped{0};
  /// Handshake init/response from a registered source whose
  /// MAC1 field doesn't verify against the link partner's
  /// stamped pubkey. Engages only when the operator has
  /// stamped pubkeys on both ends of a link. A non-zero count
  /// usually means a misconfigured client, a NAT collision,
  /// or someone pointed at the wrong relay.
  std::atomic<uint64_t> drop_handshake_pubkey_mismatch{0};
  /// Handshake init/response from an unknown source whose
  /// MAC1 didn't verify against any registered partner's
  /// pubkey — i.e. wasn't a roam attempt for any known peer.
  std::atomic<uint64_t> drop_handshake_no_pubkey_match{0};
  /// Candidate slot expired without transport data confirming
  /// it. Strong signal of a forged handshake — the source
  /// could produce a valid MAC1 but couldn't progress to
  /// transport data because they don't have the static
  /// private key.
  std::atomic<uint64_t> drop_relearn_unconfirmed{0};
};

/// One attached NIC. The same BPF program is attached to
/// each entry; the maps are shared across them so MAC
/// learning and peer lookups are global.
struct WgXdpAttachment {
  std::string iface;
  int ifindex = 0;
  uint8_t mac[6] = {0};
  /// Primary IPv4 address on this NIC, network byte order.
  /// Used as the egress source IP on cross-NIC redirect.
  uint32_t ipv4_be = 0;
};

/// XDP fast-path state. Populated when wg_relay.xdp_interface
/// is set in the config and the BPF program loads successfully.
/// All fds are closed in WgRelayStop.
struct WgXdpCtx {
  void* bpf_obj = nullptr;
  int prog_fd = -1;
  /// One or more NICs the BPF program is attached to.
  /// Iteration-1 single-NIC configs have one entry; the
  /// dual-NIC XDP_REDIRECT topology has one per NIC.
  std::vector<WgXdpAttachment> attachments;
  int peers_map_fd = -1;
  int macs_map_fd = -1;
  int stats_map_fd = -1;
  int port_map_fd = -1;
  /// Per-CPU byte counters keyed on source endpoint. Folded
  /// into per-peer rx_bytes/fwd_bytes in `wg peer list` so
  /// the operator sees XDP-path traffic alongside userspace.
  int peer_bytes_map_fd = -1;
  /// Source-IP blocklist (HASH key=u32 IPv4 NBO, value=
  /// blocklist_entry). Userspace writes; BPF reads + drops
  /// on every packet from a live blocklisted source.
  int blocklist_map_fd = -1;
  /// Devmap (key = ifindex, value = ifindex) used for
  /// cross-NIC redirect. Populated at attach with one
  /// entry per attachment's ifindex.
  int devmap_fd = -1;
  /// NIC source-MAC map (key = ifindex, value = 6-byte
  /// MAC). Populated at attach. The BPF program reads it
  /// for the egress source MAC on cross-NIC redirect.
  int nic_macs_map_fd = -1;
  /// NIC primary-IPv4 map (key = ifindex, value = u32 IP
  /// in network byte order). Used for the egress source
  /// IP on cross-NIC redirect, since WG peers silently
  /// drop packets whose src endpoint doesn't match.
  int nic_ips_map_fd = -1;
  bool attached = false;
};

/// Per-CPU stats summed across all cores. These count
/// packets that took the BPF/XDP_TX path (rx_xdp,
/// fwd_xdp) plus the two reasons XDP fell through to
/// userspace (pass_no_peer, pass_no_mac).
struct WgXdpStats {
  uint64_t rx_xdp = 0;
  uint64_t fwd_xdp = 0;
  uint64_t pass_no_peer = 0;
  uint64_t pass_no_mac = 0;
  uint64_t drop_not_wg_shaped = 0;
  uint64_t drop_blocklisted = 0;
};

/// Strike record per source IP — incremented when a candidate
/// endpoint that source registered fails to confirm via
/// transport-data. Escalates the source onto the blocklist
/// after a threshold is crossed.
struct WgRelayStrike {
  uint32_t count = 0;
  uint64_t first_strike_ns = 0;
  uint64_t total_strikes = 0;
};

struct WgRelay {
  int sock_fd = -1;
  uint16_t port = 0;
  std::vector<WgRelayPeer> peers;
  std::vector<WgRelayLink> links;
  /// peers_mu guards peers + links + roster_path writes
  /// AND the strike + blocklist tables below.
  mutable std::mutex peers_mu;
  /// Failed-confirm strikes by source IP (host-byte-order
  /// uint32_t). Cleared from a peer's record once a confirm
  /// succeeds; escalated to wg_blocklist once the threshold
  /// is crossed.
  std::map<uint32_t, WgRelayStrike> strikes;
  /// Blocked source IPs (host-byte-order uint32_t) → expiry
  /// timestamp (steady_clock ns). Mirrors the BPF
  /// wg_blocklist map for `wg blocklist list` / userspace
  /// drop in case XDP isn't attached.
  std::map<uint32_t, uint64_t> blocklist;
  WgRelayStats stats;
  std::atomic<bool> running{false};
  std::thread loop_thread;
  std::string roster_path;
  /// XDP fast path. attached == true iff the BPF program
  /// is live on a NIC. Map updates from `wg link add`
  /// land here; the userspace recv loop still runs as the
  /// fallback for cold-start packets (XDP_PASS).
  WgXdpCtx xdp;
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
/// Pin a peer to a specific NIC for XDP_REDIRECT. The NIC
/// must be one of the names in wg_relay.xdp_interface or
/// the call is rejected. Empty `nic` clears the binding
/// (peer falls back to the first attached NIC).
bool WgRelayPeerNic(WgRelay* r, const std::string& name,
                     const std::string& nic);
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
  uint64_t drop_not_wg_shaped;
  uint64_t drop_handshake_pubkey_mismatch;
  uint64_t drop_handshake_no_pubkey_match;
  uint64_t drop_relearn_unconfirmed;
  size_t peer_count;
  size_t link_count;
  /// XDP-path counters, zero when xdp.attached is false.
  WgXdpStats xdp;
  bool xdp_attached;
};
WgRelayStatsSnapshot WgRelayGetStats(const WgRelay* r);

struct WgBlocklistView {
  std::string ip;            // dotted-quad IPv4
  uint64_t seconds_left;     // until expiry
  uint64_t total_strikes;    // cumulative for this IP
};
std::vector<WgBlocklistView> WgRelayListBlocklist(
    const WgRelay* r);

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_WG_RELAY_H_
