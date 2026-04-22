/// @file hd_peers.h
/// @brief HD peer registry: enrollment state and forwarding
///   rules for HD Protocol peers.

#ifndef INCLUDE_HYPER_DERP_HD_PEERS_H_
#define INCLUDE_HYPER_DERP_HD_PEERS_H_

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "hyper_derp/hd_protocol.h"
#include "hyper_derp/protocol.h"

namespace hyper_derp {

/// Enrollment state for an HD peer.
enum class HdPeerState : uint8_t {
  kPending = 0,
  kApproved = 1,
  kDenied = 2,
};

/// How new peers are enrolled.
enum class HdEnrollMode : uint8_t {
  kManual = 0,
  kAutoApprove = 1,
};

/// Maximum peers in the registry.
inline constexpr int kHdMaxPeers = 4096;

/// Maximum forwarding rules per peer.
inline constexpr int kHdMaxForwardRules = 16;

/// HMAC-SHA-512-256 output size (crypto_auth).
inline constexpr int kHdHmacSize = 32;

/// A single forwarding rule (destination key).
struct HdForwardRule {
  Key dst_key{};
  uint8_t occupied = 0;
};

/// Per-peer routing policy (cold path — read only at
/// OpenConnection time). Kept in a parallel array so the
/// hot HdPeer struct stays cache-friendly.
struct HdPeerPolicy {
  /// If true, `pinned_intent` replaces whatever the
  /// client asked for; if false, the pin acts as a
  /// narrowing constraint within the allowed set.
  bool override_client = false;
  /// True when `pinned_intent` is meaningful.
  bool has_pin = false;
  /// Intent pinned by this peer (ignored unless has_pin).
  HdIntent pinned_intent = HdIntent::kPreferDirect;
  /// Admin tag, surfaced in audit logs (e.g.
  /// "compliance-zone-A").
  std::string audit_tag;
  /// Human-readable rationale ("IP is sensitive").
  std::string reason;
};

/// Per-peer enrollment and forwarding state.
struct HdPeer {
  Key key{};
  int fd = -1;
  uint16_t peer_id = 0;  // Local peer ID (1-65535).
  HdPeerState state = HdPeerState::kPending;
  // 0=empty, 1=live, 2=tombstone
  uint8_t occupied = 0;
  HdForwardRule rules[kHdMaxForwardRules]{};
  int rule_count = 0;
  uint64_t enrolled_at = 0;
};

/// Policy constraints applied to auto-approval.
struct HdEnrollPolicy {
  /// Hard cap on approved peer count (0 = unlimited).
  int max_peers = 0;
  /// Glob patterns matched against the client-key string
  /// (raw 64-char hex). A single trailing '*' acts as a
  /// wildcard. Empty list = allow any.
  std::vector<std::string> allowed_keys;
  /// IPv4 CIDR the peer's source address must be in. Empty
  /// string = no restriction.
  std::string require_ip_range;
};

/// Top-level HD peer registry.
struct HdPeerRegistry {
  HdPeer peers[kHdMaxPeers]{};
  int peer_count = 0;
  HdEnrollMode enroll_mode = HdEnrollMode::kManual;
  HdEnrollPolicy policy;
  Key relay_key{};
  uint16_t next_peer_id = 1;  // Monotonic peer ID counter.
  uint16_t relay_id = 0;      // This relay's ID.
  /// Persistent list of revoked client keys. A key appears
  /// here when an admin calls HdPeersRevoke() (or the REST
  /// DELETE endpoint). Subsequent enrollment attempts by
  /// the same key are rejected even after the slot is
  /// freed. Persisted to disk via HdDenylist{Load,Save} if
  /// `denylist_path` is non-empty.
  std::vector<Key> denylist;
  std::string denylist_path;
  /// Per-peer routing policies, indexed by the same slot
  /// as `peers[]`. Cold path; never touched on packet
  /// forwarding.
  HdPeerPolicy policies[kHdMaxPeers]{};
  std::string peer_policy_path;
  std::recursive_mutex mutex;
};

/// @brief Looks up the routing policy for a peer by key.
/// @param reg Registry to search.
/// @param key Pointer to 32-byte peer key.
/// @returns Pointer to the peer's HdPeerPolicy slot, or
///   nullptr if the peer is not registered.
HdPeerPolicy* HdPeersLookupPolicy(HdPeerRegistry* reg,
                                   const uint8_t* key);

/// @brief Sets the routing policy for a peer.
/// @param reg Registry containing the peer.
/// @param key Peer's 32-byte public key.
/// @param policy New policy (copied).
/// @returns True if the peer exists and the policy was
///   updated.
bool HdPeersSetPolicy(HdPeerRegistry* reg,
                      const uint8_t* key,
                      const HdPeerPolicy& policy);

/// @brief Clears the routing policy for a peer to
///   defaults.
/// @returns True if the peer exists.
bool HdPeersClearPolicy(HdPeerRegistry* reg,
                        const uint8_t* key);

/// @brief Loads peer policies from disk.
///
/// File format: `[4B count LE]` then `count` records:
///   `[32B peer_key][1B has_pin][1B pinned_intent]
///    [1B override_client][2B audit_tag_len LE][N bytes]
///    [2B reason_len LE][M bytes]`.
/// Records whose key is not present in the registry are
/// silently skipped (enrollment will assign a policy
/// slot, but the admin config is only relevant once the
/// peer enrolls). Missing file is treated as empty.
/// @returns True on success (or missing file).
bool HdPeerPolicyLoad(HdPeerRegistry* reg);

/// @brief Atomically persists peer policies to disk.
/// @returns True on success.
bool HdPeerPolicySave(const HdPeerRegistry* reg);

/// @brief Checks a candidate enrollment against the policy.
/// @param reg Registry with policy to apply.
/// @param client_key Candidate client key.
/// @param peer_ipv4_be Peer's source IPv4 address in
///   network byte order, or 0 to skip the ip-range check.
/// @param out_reason If rejected, set to a short reason
///   string (owned by the caller-visible string literal).
/// @returns True if the candidate passes policy.
bool HdPolicyAllows(const HdPeerRegistry* reg,
                    const Key& client_key,
                    uint32_t peer_ipv4_be,
                    const char** out_reason);

/// Initialize the registry with a relay key and mode.
/// @param reg Registry to initialize.
/// @param relay_key Relay's public key (used for HMAC).
/// @param mode Enrollment mode (manual or auto-approve).
void HdPeersInit(HdPeerRegistry* reg,
                 const Key& relay_key,
                 HdEnrollMode mode);

/// Look up a peer by its 32-byte public key.
/// @param reg Registry to search.
/// @param key Pointer to 32-byte public key.
/// @returns Pointer to the peer, or nullptr if not found.
HdPeer* HdPeersLookup(HdPeerRegistry* reg,
                       const uint8_t* key);

/// Find an HD peer by peer ID (O(n) scan, not hot path).
/// @param reg Registry to search.
/// @param peer_id Local peer ID to look up.
/// @returns Pointer to the peer, or nullptr if not found.
HdPeer* HdPeersLookupById(HdPeerRegistry* reg,
                           uint16_t peer_id);

/// Insert a new peer in pending state.
/// @param reg Registry to insert into.
/// @param key Peer's public key.
/// @param fd Socket file descriptor for the peer.
/// @returns Pointer to the inserted (or existing) peer,
///   or nullptr if the registry is full.
HdPeer* HdPeersInsert(HdPeerRegistry* reg,
                       const Key& key,
                       int fd);

/// Approve a pending peer.
/// @param reg Registry containing the peer.
/// @param key Pointer to 32-byte public key.
/// @returns True if the peer was found and approved.
bool HdPeersApprove(HdPeerRegistry* reg,
                    const uint8_t* key);

/// Deny a pending peer.
/// @param reg Registry containing the peer.
/// @param key Pointer to 32-byte public key.
/// @returns True if the peer was found and denied.
bool HdPeersDeny(HdPeerRegistry* reg,
                 const uint8_t* key);

/// Remove (tombstone) a peer.
/// @param reg Registry containing the peer.
/// @param key Pointer to 32-byte public key.
void HdPeersRemove(HdPeerRegistry* reg,
                   const uint8_t* key);

/// Permanently revoke a client key: remove the peer (if
/// present) and add the key to the persistent denylist.
/// Enrollment attempts by this key are rejected until the
/// admin clears the denylist.
/// @param reg Registry containing the peer.
/// @param key Pointer to 32-byte public key.
void HdPeersRevoke(HdPeerRegistry* reg,
                   const uint8_t* key);

/// Returns true if the key is present in the denylist.
/// @param reg Registry with denylist.
/// @param key Pointer to 32-byte public key.
bool HdPeersIsDenied(const HdPeerRegistry* reg,
                     const uint8_t* key);

/// Load denylist from disk. File format is raw concatenated
/// 32-byte keys; a missing file is treated as empty.
/// @param reg Registry to populate. `reg->denylist_path`
///   must already be set.
/// @returns True on success (or when the file is absent).
bool HdDenylistLoad(HdPeerRegistry* reg);

/// Save the denylist to disk atomically.
/// @param reg Registry whose denylist to persist.
/// @returns True on success.
bool HdDenylistSave(const HdPeerRegistry* reg);

/// Add a forwarding rule for a peer.
/// @param reg Registry containing the peer.
/// @param peer_key Pointer to the peer's 32-byte key.
/// @param dst_key Destination key for the rule.
/// @returns True if the rule was added, false if the peer
///   was not found or the rule limit was reached.
bool HdPeersAddRule(HdPeerRegistry* reg,
                    const uint8_t* peer_key,
                    const Key& dst_key);

/// Remove a forwarding rule from a peer.
/// @param reg Registry containing the peer.
/// @param peer_key Pointer to the peer's 32-byte key.
/// @param dst_key Pointer to the 32-byte destination key.
/// @returns True if the rule was found and removed.
bool HdPeersRemoveRule(HdPeerRegistry* reg,
                       const uint8_t* peer_key,
                       const uint8_t* dst_key);

/// Verify enrollment HMAC: crypto_auth_verify(hmac,
///   client_key, relay_key).
/// @param reg Registry (provides relay_key).
/// @param client_key Client's public key.
/// @param hmac Pointer to 32-byte HMAC to verify.
/// @returns True if the HMAC is valid.
bool HdVerifyEnrollment(const HdPeerRegistry* reg,
                        const Key& client_key,
                        const uint8_t* hmac);

/// List enrolled peers (all states).
/// @param reg Registry to list.
/// @param out_keys Output array for peer keys.
/// @param out_states Output array for peer states.
/// @param max_out Maximum entries to write.
/// @returns Number of peers written.
int HdPeersList(const HdPeerRegistry* reg,
                Key* out_keys,
                HdPeerState* out_states,
                int max_out);

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_HD_PEERS_H_
