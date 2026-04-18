/// @file wg_peer.cc
/// @brief Per-peer state machine for WireGuard tunnels.

#include "wg_peer.h"

#include <cstring>

#include <sodium.h>

namespace hyper_derp {

WgPeer* WgPeerAdd(WgPeerTable* t, const Key& hd_key,
                  uint16_t hd_peer_id) {
  // Check existing.
  for (int i = 0; i < kWgMaxPeers; i++) {
    if (t->peers[i].active &&
        t->peers[i].hd_key == hd_key) {
      return &t->peers[i];
    }
  }
  // Find empty slot.
  for (int i = 0; i < kWgMaxPeers; i++) {
    if (!t->peers[i].active) {
      auto* p = &t->peers[i];
      p->hd_key = hd_key;
      p->hd_peer_id = hd_peer_id;
      p->state = WgPeerState::kNew;
      p->active = 1;
      t->count++;
      return p;
    }
  }
  return nullptr;
}

WgPeer* WgPeerFind(WgPeerTable* t, const Key& hd_key) {
  for (int i = 0; i < kWgMaxPeers; i++) {
    if (t->peers[i].active &&
        t->peers[i].hd_key == hd_key) {
      return &t->peers[i];
    }
  }
  return nullptr;
}

WgPeer* WgPeerFindById(WgPeerTable* t,
                       uint16_t hd_peer_id) {
  for (int i = 0; i < kWgMaxPeers; i++) {
    if (t->peers[i].active &&
        t->peers[i].hd_peer_id == hd_peer_id) {
      return &t->peers[i];
    }
  }
  return nullptr;
}

void WgPeerRemove(WgPeerTable* t, const Key& hd_key) {
  for (int i = 0; i < kWgMaxPeers; i++) {
    if (t->peers[i].active &&
        t->peers[i].hd_key == hd_key) {
      t->peers[i].active = 0;
      t->count--;
      return;
    }
  }
}

}  // namespace hyper_derp
