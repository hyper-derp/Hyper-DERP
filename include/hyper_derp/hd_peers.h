/// @file hd_peers.h
/// @brief HD peer registry: enrollment state and forwarding
///   rules for HD Protocol peers.

#ifndef INCLUDE_HYPER_DERP_HD_PEERS_H_
#define INCLUDE_HYPER_DERP_HD_PEERS_H_

#include <cstdint>
#include <mutex>

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

/// Top-level HD peer registry.
struct HdPeerRegistry {
  HdPeer peers[kHdMaxPeers]{};
  int peer_count = 0;
  HdEnrollMode enroll_mode = HdEnrollMode::kManual;
  Key relay_key{};
  uint16_t next_peer_id = 1;  // Monotonic peer ID counter.
  uint16_t relay_id = 0;      // This relay's ID.
  std::recursive_mutex mutex;
};

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
