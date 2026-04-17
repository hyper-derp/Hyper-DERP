/// @file turn.cc
/// @brief TURN allocation manager implementation
///   (RFC 5766). Userspace state machine for relay
///   allocations, permissions, and channel bindings.

#include "hyper_derp/turn.h"

#include <chrono>
#include <cstring>
#include <mutex>

namespace hyper_derp {

// -- Time helpers -------------------------------------------

static uint64_t NowNs() {
  auto tp = std::chrono::steady_clock::now();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          tp.time_since_epoch())
          .count());
}

// -- Allocation helpers -------------------------------------

/// Clear all fields in an allocation slot.
static void ClearAllocation(TurnAllocation* a) {
  a->id = 0;
  a->client_ip = 0;
  a->client_port = 0;
  a->relay_ip = 0;
  a->relay_port = 0;
  a->expires_at = 0;
  a->occupied = 0;
  std::memset(a->permissions, 0, sizeof(a->permissions));
  std::memset(a->channels, 0, sizeof(a->channels));
}

// -- Public API ---------------------------------------------

void TurnInit(TurnManager* mgr, uint32_t relay_ip,
              const char* realm) {
  mgr->relay_ip = relay_ip;
  mgr->realm = realm;
  mgr->alloc_count = 0;
  mgr->next_id = 1;
  mgr->next_port = 49152;
  std::memset(mgr->allocs, 0, sizeof(mgr->allocs));
}

TurnAllocation* TurnAllocate(TurnManager* mgr,
                             uint32_t client_ip,
                             uint16_t client_port,
                             int lifetime) {
  if (lifetime <= 0) {
    lifetime = kTurnDefaultLifetime;
  }
  if (lifetime > kTurnMaxLifetime) {
    lifetime = kTurnMaxLifetime;
  }
  std::lock_guard<std::mutex> lock(mgr->mutex);
  if (mgr->alloc_count >= kTurnMaxAllocations) {
    return nullptr;
  }
  // Linear scan for empty slot.
  for (int i = 0; i < kTurnMaxAllocations; i++) {
    TurnAllocation* a = &mgr->allocs[i];
    if (a->occupied != 0) {
      continue;
    }
    a->id = mgr->next_id++;
    a->client_ip = client_ip;
    a->client_port = client_port;
    a->relay_ip = mgr->relay_ip;
    a->relay_port = mgr->next_port;
    a->expires_at =
        NowNs() + static_cast<uint64_t>(lifetime) *
                       1000000000ULL;
    a->occupied = 1;
    // Advance port, wrap into ephemeral range.
    mgr->next_port++;
    if (mgr->next_port == 0) {
      mgr->next_port = 49152;
    }
    mgr->alloc_count++;
    return a;
  }
  return nullptr;
}

bool TurnRefresh(TurnManager* mgr, uint32_t alloc_id,
                 int lifetime) {
  std::lock_guard<std::mutex> lock(mgr->mutex);
  for (int i = 0; i < kTurnMaxAllocations; i++) {
    TurnAllocation* a = &mgr->allocs[i];
    if (a->occupied != 1 || a->id != alloc_id) {
      continue;
    }
    if (lifetime == 0) {
      ClearAllocation(a);
      mgr->alloc_count--;
      return true;
    }
    if (lifetime > kTurnMaxLifetime) {
      lifetime = kTurnMaxLifetime;
    }
    a->expires_at =
        NowNs() + static_cast<uint64_t>(lifetime) *
                       1000000000ULL;
    return true;
  }
  return false;
}

void TurnDeallocate(TurnManager* mgr, uint32_t alloc_id) {
  std::lock_guard<std::mutex> lock(mgr->mutex);
  for (int i = 0; i < kTurnMaxAllocations; i++) {
    TurnAllocation* a = &mgr->allocs[i];
    if (a->occupied == 1 && a->id == alloc_id) {
      ClearAllocation(a);
      mgr->alloc_count--;
      return;
    }
  }
}

TurnAllocation* TurnFindAlloc(TurnManager* mgr,
                              uint32_t client_ip,
                              uint16_t client_port) {
  std::lock_guard<std::mutex> lock(mgr->mutex);
  for (int i = 0; i < kTurnMaxAllocations; i++) {
    TurnAllocation* a = &mgr->allocs[i];
    if (a->occupied == 1 &&
        a->client_ip == client_ip &&
        a->client_port == client_port) {
      return a;
    }
  }
  return nullptr;
}

bool TurnCreatePermission(TurnManager* mgr,
                           uint32_t alloc_id,
                           uint32_t peer_ip) {
  std::lock_guard<std::mutex> lock(mgr->mutex);
  // Find allocation.
  TurnAllocation* alloc = nullptr;
  for (int i = 0; i < kTurnMaxAllocations; i++) {
    if (mgr->allocs[i].occupied == 1 &&
        mgr->allocs[i].id == alloc_id) {
      alloc = &mgr->allocs[i];
      break;
    }
  }
  if (alloc == nullptr) {
    return false;
  }
  // Refresh existing permission if found.
  uint64_t expires =
      NowNs() + 300ULL * 1000000000ULL;
  for (int i = 0; i < kTurnMaxPermissions; i++) {
    TurnPermission* p = &alloc->permissions[i];
    if (p->occupied == 1 && p->peer_ip == peer_ip) {
      p->expires_at = expires;
      return true;
    }
  }
  // Find empty slot.
  for (int i = 0; i < kTurnMaxPermissions; i++) {
    TurnPermission* p = &alloc->permissions[i];
    if (p->occupied == 0) {
      p->peer_ip = peer_ip;
      p->expires_at = expires;
      p->occupied = 1;
      return true;
    }
  }
  return false;
}

bool TurnCheckPermission(const TurnAllocation* alloc,
                         uint32_t peer_ip) {
  uint64_t now = NowNs();
  for (int i = 0; i < kTurnMaxPermissions; i++) {
    const TurnPermission* p = &alloc->permissions[i];
    if (p->occupied == 1 &&
        p->peer_ip == peer_ip &&
        p->expires_at > now) {
      return true;
    }
  }
  return false;
}

bool TurnChannelBind(TurnManager* mgr,
                     uint32_t alloc_id,
                     uint16_t channel,
                     uint32_t peer_ip,
                     uint16_t peer_port) {
  if (channel < kTurnChannelMin ||
      channel > kTurnChannelMax) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mgr->mutex);
  // Find allocation.
  TurnAllocation* alloc = nullptr;
  for (int i = 0; i < kTurnMaxAllocations; i++) {
    if (mgr->allocs[i].occupied == 1 &&
        mgr->allocs[i].id == alloc_id) {
      alloc = &mgr->allocs[i];
      break;
    }
  }
  if (alloc == nullptr) {
    return false;
  }
  uint64_t expires =
      NowNs() + 600ULL * 1000000000ULL;
  // Check for existing binding with same channel number.
  for (int i = 0; i < kTurnMaxChannels; i++) {
    TurnChannel* c = &alloc->channels[i];
    if (c->occupied != 1) {
      continue;
    }
    if (c->number == channel) {
      // Same channel: must be same peer to refresh.
      if (c->peer_ip == peer_ip &&
          c->peer_port == peer_port) {
        c->expires_at = expires;
        return true;
      }
      // Different peer on same channel: reject.
      return false;
    }
  }
  // Find empty slot.
  for (int i = 0; i < kTurnMaxChannels; i++) {
    TurnChannel* c = &alloc->channels[i];
    if (c->occupied == 0) {
      c->number = channel;
      c->peer_ip = peer_ip;
      c->peer_port = peer_port;
      c->expires_at = expires;
      c->occupied = 1;
      return true;
    }
  }
  return false;
}

const TurnChannel* TurnFindChannel(
    const TurnAllocation* alloc,
    uint16_t channel) {
  uint64_t now = NowNs();
  for (int i = 0; i < kTurnMaxChannels; i++) {
    const TurnChannel* c = &alloc->channels[i];
    if (c->occupied == 1 &&
        c->number == channel &&
        c->expires_at > now) {
      return c;
    }
  }
  return nullptr;
}

int TurnExpire(TurnManager* mgr) {
  std::lock_guard<std::mutex> lock(mgr->mutex);
  uint64_t now = NowNs();
  int expired = 0;
  for (int i = 0; i < kTurnMaxAllocations; i++) {
    TurnAllocation* a = &mgr->allocs[i];
    if (a->occupied != 1) {
      continue;
    }
    // Expire the whole allocation.
    if (a->expires_at <= now) {
      ClearAllocation(a);
      mgr->alloc_count--;
      expired++;
      continue;
    }
    // Expire individual permissions.
    for (int j = 0; j < kTurnMaxPermissions; j++) {
      TurnPermission* p = &a->permissions[j];
      if (p->occupied == 1 && p->expires_at <= now) {
        p->peer_ip = 0;
        p->expires_at = 0;
        p->occupied = 0;
        expired++;
      }
    }
    // Expire individual channels.
    for (int j = 0; j < kTurnMaxChannels; j++) {
      TurnChannel* c = &a->channels[j];
      if (c->occupied == 1 && c->expires_at <= now) {
        c->number = 0;
        c->peer_ip = 0;
        c->peer_port = 0;
        c->expires_at = 0;
        c->occupied = 0;
        expired++;
      }
    }
  }
  return expired;
}

int TurnAllocCount(const TurnManager* mgr) {
  return mgr->alloc_count;
}

}  // namespace hyper_derp
