/// @file peer_info.h
/// @brief Peer metadata.

#ifndef HD_SDK_PEER_INFO_H_
#define HD_SDK_PEER_INFO_H_

#include <array>
#include <cstdint>
#include <string>

namespace hd::sdk {

struct PeerInfo {
  /// Human-readable peer name (from enrollment).
  std::string name;
  /// HD peer ID within the peer's home relay.
  uint16_t peer_id = 0;
  /// HD relay ID (0 = same relay as this client).
  uint16_t relay_id = 0;
  /// Whether the peer is currently connected.
  bool connected = false;
  /// Peer's 32-byte public key.
  std::array<uint8_t, 32> public_key{};
};

}  // namespace hd::sdk

#endif  // HD_SDK_PEER_INFO_H_
