/// @file hd_relay_table.h
/// @brief Relay-to-relay routing table for FleetData
///   forwarding.

#ifndef INCLUDE_HYPER_DERP_HD_RELAY_TABLE_H_
#define INCLUDE_HYPER_DERP_HD_RELAY_TABLE_H_

#include <cstdint>
#include <mutex>

#include "hyper_derp/protocol.h"

namespace hyper_derp {

/// Maximum number of relay entries in the table.
inline constexpr int kMaxRelays = 256;

/// A neighbor relay entry.
struct RelayEntry {
  uint16_t relay_id = 0;
  int fd = -1;
  Key key{};
  char name[64]{};
  uint8_t hops = 0;
  uint16_t next_hop = 0;
  uint8_t occupied = 0;
};

/// Relay routing table.
struct RelayTable {
  RelayEntry entries[kMaxRelays]{};
  uint16_t self_id = 0;
  int relay_count = 0;
  std::recursive_mutex mutex;
};

/// Initialize the relay table.
/// @param rt Table to initialize.
/// @param self_id This relay's ID in the fleet.
void RelayTableInit(RelayTable* rt, uint16_t self_id);

/// Add or update a direct neighbor relay.
/// @param rt Relay table.
/// @param relay_id ID of the neighbor relay.
/// @param fd HD connection fd to this relay.
/// @param key Relay's 32-byte public key.
/// @param name Human-readable relay name (max 63 chars).
/// @returns Pointer to the inserted/updated entry, or
///   nullptr if the table is full.
RelayEntry* RelayTableAdd(RelayTable* rt,
                          uint16_t relay_id,
                          int fd,
                          const Key& key,
                          const char* name);

/// Remove a relay (disconnected).
/// @param rt Relay table.
/// @param relay_id ID of the relay to remove.
void RelayTableRemove(RelayTable* rt,
                      uint16_t relay_id);

/// Look up a relay by ID.
/// @param rt Relay table.
/// @param relay_id ID of the relay to look up.
/// @returns Pointer to the entry, or nullptr if not
///   found.
RelayEntry* RelayTableLookup(RelayTable* rt,
                             uint16_t relay_id);

/// Update a route learned from a neighbor's announcement.
/// Only updates if the new hop count is lower than the
/// existing route.
/// @param rt Relay table.
/// @param relay_id ID of the relay being routed to.
/// @param hops Hop count to that relay.
/// @param next_hop Relay ID of the next hop.
/// @param next_hop_fd FD of the next-hop relay connection.
void RelayTableUpdateRoute(RelayTable* rt,
                           uint16_t relay_id,
                           uint8_t hops,
                           uint16_t next_hop,
                           int next_hop_fd);

/// List all known relays (for route announcements).
/// @param rt Relay table.
/// @param out_ids Output array for relay IDs.
/// @param out_hops Output array for hop counts.
/// @param max_out Maximum entries to write.
/// @returns Number of entries written.
int RelayTableList(const RelayTable* rt,
                   uint16_t* out_ids,
                   uint8_t* out_hops,
                   int max_out);

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_HD_RELAY_TABLE_H_
