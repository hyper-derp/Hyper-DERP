/// @file turn.h
/// @brief TURN allocation manager for Level 2 NAT
///   traversal (RFC 5766). Manages relay allocations,
///   permissions, and channel bindings. BPF map
///   integration is stubbed for now.

#ifndef INCLUDE_HYPER_DERP_TURN_H_
#define INCLUDE_HYPER_DERP_TURN_H_

#include <cstdint>
#include <mutex>
#include <string>

namespace hyper_derp {

// -- Limits -------------------------------------------------

inline constexpr int kTurnDefaultLifetime = 600;
inline constexpr int kTurnMaxLifetime = 3600;
inline constexpr int kTurnMaxAllocations = 10000;
inline constexpr int kTurnMaxPermissions = 64;
inline constexpr int kTurnMaxChannels = 64;
inline constexpr uint16_t kTurnChannelMin = 0x4000;
inline constexpr uint16_t kTurnChannelMax = 0x7FFF;
inline constexpr int kTurnDefaultPort = 3478;

// -- TURN message types (method + class) --------------------

inline constexpr uint16_t kTurnAllocate = 0x0003;
inline constexpr uint16_t kTurnAllocateResponse = 0x0103;
inline constexpr uint16_t kTurnAllocateError = 0x0113;
inline constexpr uint16_t kTurnRefresh = 0x0004;
inline constexpr uint16_t kTurnRefreshResponse = 0x0104;
inline constexpr uint16_t kTurnCreatePermission = 0x0008;
inline constexpr uint16_t kTurnCreatePermissionResponse =
    0x0108;
inline constexpr uint16_t kTurnChannelBind = 0x0009;
inline constexpr uint16_t kTurnChannelBindResponse = 0x0109;
inline constexpr uint16_t kTurnSendIndication = 0x0016;
inline constexpr uint16_t kTurnDataIndication = 0x0017;

// -- TURN-specific attribute types --------------------------

inline constexpr uint16_t kTurnAttrChannelNumber = 0x000C;
inline constexpr uint16_t kTurnAttrLifetime = 0x000D;
inline constexpr uint16_t kTurnAttrXorPeerAddress = 0x0012;
inline constexpr uint16_t kTurnAttrData = 0x0013;
inline constexpr uint16_t kTurnAttrXorRelayedAddress =
    0x0016;
inline constexpr uint16_t kTurnAttrRequestedTransport =
    0x0019;

// -- Data structures ----------------------------------------

/// Per-allocation permission entry.
struct TurnPermission {
  uint32_t peer_ip = 0;
  uint64_t expires_at = 0;
  uint8_t occupied = 0;
};

/// Per-allocation channel binding.
struct TurnChannel {
  uint16_t number = 0;
  uint32_t peer_ip = 0;
  uint16_t peer_port = 0;
  uint64_t expires_at = 0;
  uint8_t occupied = 0;
};

/// Single TURN allocation.
struct TurnAllocation {
  uint32_t id = 0;
  uint32_t client_ip = 0;
  uint16_t client_port = 0;
  uint32_t relay_ip = 0;
  uint16_t relay_port = 0;
  uint64_t expires_at = 0;
  uint8_t occupied = 0;
  TurnPermission permissions[kTurnMaxPermissions]{};
  TurnChannel channels[kTurnMaxChannels]{};
};

/// TURN allocation manager.
struct TurnManager {
  TurnAllocation allocs[kTurnMaxAllocations]{};
  int alloc_count = 0;
  uint32_t next_id = 1;
  uint16_t next_port = 49152;
  uint32_t relay_ip = 0;
  std::string realm;
  std::mutex mutex;
};

/// Initialize the TURN manager.
void TurnInit(TurnManager* mgr, uint32_t relay_ip,
              const char* realm);

/// Create a new allocation.
/// Returns the allocation, or nullptr if limit reached.
TurnAllocation* TurnAllocate(TurnManager* mgr,
                             uint32_t client_ip,
                             uint16_t client_port,
                             int lifetime);

/// Refresh an existing allocation's lifetime.
bool TurnRefresh(TurnManager* mgr, uint32_t alloc_id,
                 int lifetime);

/// Delete an allocation.
void TurnDeallocate(TurnManager* mgr, uint32_t alloc_id);

/// Find an allocation by client 5-tuple.
TurnAllocation* TurnFindAlloc(TurnManager* mgr,
                              uint32_t client_ip,
                              uint16_t client_port);

/// Create a permission on an allocation.
bool TurnCreatePermission(TurnManager* mgr,
                          uint32_t alloc_id,
                          uint32_t peer_ip);

/// Check if a permission exists for a peer IP.
bool TurnCheckPermission(const TurnAllocation* alloc,
                         uint32_t peer_ip);

/// Bind a channel number to a peer address.
bool TurnChannelBind(TurnManager* mgr,
                     uint32_t alloc_id,
                     uint16_t channel,
                     uint32_t peer_ip,
                     uint16_t peer_port);

/// Look up a channel binding by number.
const TurnChannel* TurnFindChannel(
    const TurnAllocation* alloc,
    uint16_t channel);

/// Expire stale allocations, permissions, and channels.
/// Call periodically (e.g. every 30s).
int TurnExpire(TurnManager* mgr);

/// Get allocation count.
int TurnAllocCount(const TurnManager* mgr);

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_TURN_H_
