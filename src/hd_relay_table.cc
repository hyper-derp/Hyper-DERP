/// @file hd_relay_table.cc
/// @brief Relay-to-relay routing table implementation.

#include "hyper_derp/hd_relay_table.h"

#include <cstring>

namespace hyper_derp {

void RelayTableInit(RelayTable* rt, uint16_t self_id) {
  std::lock_guard lock(rt->mutex);
  for (int i = 0; i < kMaxRelays; i++) {
    rt->entries[i] = RelayEntry{};
  }
  rt->self_id = self_id;
  rt->relay_count = 0;
}

RelayEntry* RelayTableAdd(RelayTable* rt,
                          uint16_t relay_id,
                          int fd,
                          const Key& key,
                          const char* name) {
  std::lock_guard lock(rt->mutex);
  if (relay_id == 0) return nullptr;

  // Check if already exists.
  for (int i = 0; i < kMaxRelays; i++) {
    if (rt->entries[i].occupied &&
        rt->entries[i].relay_id == relay_id) {
      rt->entries[i].fd = fd;
      rt->entries[i].key = key;
      rt->entries[i].hops = 1;
      rt->entries[i].next_hop = relay_id;
      if (name) {
        std::strncpy(rt->entries[i].name, name, 63);
        rt->entries[i].name[63] = '\0';
      }
      return &rt->entries[i];
    }
  }

  // Find an empty slot.
  for (int i = 0; i < kMaxRelays; i++) {
    if (!rt->entries[i].occupied) {
      rt->entries[i].relay_id = relay_id;
      rt->entries[i].fd = fd;
      rt->entries[i].key = key;
      rt->entries[i].hops = 1;
      rt->entries[i].next_hop = relay_id;
      rt->entries[i].occupied = 1;
      if (name) {
        std::strncpy(rt->entries[i].name, name, 63);
        rt->entries[i].name[63] = '\0';
      } else {
        rt->entries[i].name[0] = '\0';
      }
      rt->relay_count++;
      return &rt->entries[i];
    }
  }
  return nullptr;
}

void RelayTableRemove(RelayTable* rt,
                      uint16_t relay_id) {
  std::lock_guard lock(rt->mutex);
  for (int i = 0; i < kMaxRelays; i++) {
    if (rt->entries[i].occupied &&
        rt->entries[i].relay_id == relay_id) {
      rt->entries[i] = RelayEntry{};
      rt->relay_count--;
      return;
    }
  }
}

RelayEntry* RelayTableLookup(RelayTable* rt,
                             uint16_t relay_id) {
  std::lock_guard lock(rt->mutex);
  for (int i = 0; i < kMaxRelays; i++) {
    if (rt->entries[i].occupied &&
        rt->entries[i].relay_id == relay_id) {
      return &rt->entries[i];
    }
  }
  return nullptr;
}

void RelayTableUpdateRoute(RelayTable* rt,
                           uint16_t relay_id,
                           uint8_t hops,
                           uint16_t next_hop,
                           int next_hop_fd) {
  std::lock_guard lock(rt->mutex);
  if (relay_id == 0 || relay_id == rt->self_id) return;

  // Find existing entry.
  for (int i = 0; i < kMaxRelays; i++) {
    if (rt->entries[i].occupied &&
        rt->entries[i].relay_id == relay_id) {
      // Only update if hop count is strictly lower.
      if (hops < rt->entries[i].hops) {
        rt->entries[i].hops = hops;
        rt->entries[i].next_hop = next_hop;
        rt->entries[i].fd = next_hop_fd;
      }
      return;
    }
  }

  // New relay discovered via route announcement.
  for (int i = 0; i < kMaxRelays; i++) {
    if (!rt->entries[i].occupied) {
      rt->entries[i].relay_id = relay_id;
      rt->entries[i].fd = next_hop_fd;
      rt->entries[i].hops = hops;
      rt->entries[i].next_hop = next_hop;
      rt->entries[i].occupied = 1;
      rt->entries[i].name[0] = '\0';
      rt->relay_count++;
      return;
    }
  }
}

int RelayTableList(const RelayTable* rt,
                   uint16_t* out_ids,
                   uint8_t* out_hops,
                   int max_out) {
  // Caller must hold the mutex if needed for thread
  // safety. For listing, we cast away const to lock.
  auto* mutable_rt = const_cast<RelayTable*>(rt);
  std::lock_guard lock(mutable_rt->mutex);
  int count = 0;
  for (int i = 0; i < kMaxRelays && count < max_out;
       i++) {
    if (rt->entries[i].occupied) {
      out_ids[count] = rt->entries[i].relay_id;
      out_hops[count] = rt->entries[i].hops;
      count++;
    }
  }
  return count;
}

}  // namespace hyper_derp
