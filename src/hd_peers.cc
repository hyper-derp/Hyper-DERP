/// @file hd_peers.cc
/// @brief HD peer registry implementation.

#include "hyper_derp/hd_peers.h"

#include <cstring>

#include <sodium.h>

namespace hyper_derp {

void HdPeersInit(HdPeerRegistry* reg,
                 const Key& relay_key,
                 HdEnrollMode mode) {
  std::lock_guard<std::recursive_mutex> lock(reg->mutex);
  reg->relay_key = relay_key;
  reg->enroll_mode = mode;
  reg->peer_count = 0;
  for (int i = 0; i < kHdMaxPeers; ++i) {
    reg->peers[i] = HdPeer{};
  }
}

HdPeer* HdPeersLookup(HdPeerRegistry* reg,
                       const uint8_t* key) {
  for (int i = 0; i < kHdMaxPeers; ++i) {
    HdPeer& p = reg->peers[i];
    if (p.occupied != 1) continue;
    if (sodium_memcmp(p.key.data(), key, kKeySize) == 0) {
      return &p;
    }
  }
  return nullptr;
}

HdPeer* HdPeersLookupById(HdPeerRegistry* reg,
                           uint16_t peer_id) {
  for (int i = 0; i < kHdMaxPeers; ++i) {
    HdPeer& p = reg->peers[i];
    if (p.occupied != 1) continue;
    if (p.peer_id == peer_id) return &p;
  }
  return nullptr;
}

HdPeer* HdPeersInsert(HdPeerRegistry* reg,
                       const Key& key,
                       int fd) {
  std::lock_guard<std::recursive_mutex> lock(reg->mutex);
  // Check for duplicate.
  for (int i = 0; i < kHdMaxPeers; ++i) {
    HdPeer& p = reg->peers[i];
    if (p.occupied != 1) continue;
    if (sodium_memcmp(p.key.data(), key.data(),
                      kKeySize) == 0) {
      return &p;
    }
  }
  // Find an empty slot (occupied == 0 or tombstone == 2).
  for (int i = 0; i < kHdMaxPeers; ++i) {
    HdPeer& p = reg->peers[i];
    if (p.occupied == 1) continue;
    p.key = key;
    p.fd = fd;
    p.peer_id = reg->next_peer_id++;
    if (reg->next_peer_id == 0) reg->next_peer_id = 1;
    p.state = (reg->enroll_mode == HdEnrollMode::kAutoApprove)
                  ? HdPeerState::kApproved
                  : HdPeerState::kPending;
    p.occupied = 1;
    p.rule_count = 0;
    p.enrolled_at = 0;
    for (int j = 0; j < kHdMaxForwardRules; ++j) {
      p.rules[j] = HdForwardRule{};
    }
    ++reg->peer_count;
    return &p;
  }
  return nullptr;  // Registry full.
}

bool HdPeersApprove(HdPeerRegistry* reg,
                    const uint8_t* key) {
  std::lock_guard<std::recursive_mutex> lock(reg->mutex);
  HdPeer* p = HdPeersLookup(reg, key);
  if (!p) return false;
  p->state = HdPeerState::kApproved;
  return true;
}

bool HdPeersDeny(HdPeerRegistry* reg,
                 const uint8_t* key) {
  std::lock_guard<std::recursive_mutex> lock(reg->mutex);
  HdPeer* p = HdPeersLookup(reg, key);
  if (!p) return false;
  p->state = HdPeerState::kDenied;
  return true;
}

void HdPeersRemove(HdPeerRegistry* reg,
                   const uint8_t* key) {
  std::lock_guard<std::recursive_mutex> lock(reg->mutex);
  HdPeer* p = HdPeersLookup(reg, key);
  if (!p) return;
  p->occupied = 2;  // Tombstone.
  --reg->peer_count;
}

bool HdPeersAddRule(HdPeerRegistry* reg,
                    const uint8_t* peer_key,
                    const Key& dst_key) {
  std::lock_guard<std::recursive_mutex> lock(reg->mutex);
  HdPeer* p = HdPeersLookup(reg, peer_key);
  if (!p) return false;
  if (p->rule_count >= kHdMaxForwardRules) return false;
  // Find an empty rule slot.
  for (int i = 0; i < kHdMaxForwardRules; ++i) {
    if (p->rules[i].occupied == 0) {
      p->rules[i].dst_key = dst_key;
      p->rules[i].occupied = 1;
      ++p->rule_count;
      return true;
    }
  }
  return false;
}

bool HdPeersRemoveRule(HdPeerRegistry* reg,
                       const uint8_t* peer_key,
                       const uint8_t* dst_key) {
  std::lock_guard<std::recursive_mutex> lock(reg->mutex);
  HdPeer* p = HdPeersLookup(reg, peer_key);
  if (!p) return false;
  for (int i = 0; i < kHdMaxForwardRules; ++i) {
    if (p->rules[i].occupied != 1) continue;
    if (sodium_memcmp(p->rules[i].dst_key.data(),
                      dst_key, kKeySize) == 0) {
      p->rules[i] = HdForwardRule{};
      --p->rule_count;
      return true;
    }
  }
  return false;
}

bool HdVerifyEnrollment(const HdPeerRegistry* reg,
                        const Key& client_key,
                        const uint8_t* hmac) {
  return crypto_auth_verify(
             hmac, client_key.data(), kKeySize,
             reg->relay_key.data()) == 0;
}

int HdPeersList(const HdPeerRegistry* reg,
                Key* out_keys,
                HdPeerState* out_states,
                int max_out) {
  int n = 0;
  for (int i = 0; i < kHdMaxPeers && n < max_out; ++i) {
    const HdPeer& p = reg->peers[i];
    if (p.occupied != 1) continue;
    out_keys[n] = p.key;
    out_states[n] = p.state;
    ++n;
  }
  return n;
}

}  // namespace hyper_derp
