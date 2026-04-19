/// @file wg_netlink.h
/// @brief Configure wireguard.ko via generic netlink.

#ifndef CLIENT_WG_NETLINK_H_
#define CLIENT_WG_NETLINK_H_

#include <cstdint>
#include <expected>

#include "hyper_derp/error.h"

struct mnl_socket;

namespace hyper_derp {

inline constexpr int kWgKeySize = 32;

enum class WgNlError {
  SocketFailed,
  FamilyNotFound,
  SetDeviceFailed,
  SetPeerFailed,
  RemovePeerFailed,
  InterfaceFailed,
  CreateDeviceFailed,
};

/// Netlink context for WireGuard configuration.
struct WgNetlink {
  mnl_socket* nl = nullptr;
  uint16_t family_id = 0;
  uint32_t seq = 0;
  uint32_t portid = 0;
};

/// WireGuard peer configuration for WgNlSetPeer.
struct WgPeerConfig {
  uint8_t public_key[kWgKeySize]{};
  uint32_t endpoint_ip = 0;
  uint16_t endpoint_port = 0;
  uint32_t allowed_ip = 0;
  int allowed_prefix = 32;
  uint16_t keepalive_secs = 25;
};

/// Open netlink socket and resolve WireGuard family ID.
auto WgNlInit(WgNetlink* wg)
    -> std::expected<void, Error<WgNlError>>;

/// Create a WireGuard interface.
auto WgNlCreateDevice(const char* ifname)
    -> std::expected<void, Error<WgNlError>>;

/// Set device private key and listen port.
auto WgNlSetDevice(WgNetlink* wg, const char* ifname,
                   const uint8_t* private_key,
                   uint16_t listen_port)
    -> std::expected<void, Error<WgNlError>>;

/// Add or update a peer on the device.
auto WgNlSetPeer(WgNetlink* wg, const char* ifname,
                 const WgPeerConfig* peer)
    -> std::expected<void, Error<WgNlError>>;

/// Remove a peer from the device.
auto WgNlRemovePeer(WgNetlink* wg, const char* ifname,
                    const uint8_t* public_key)
    -> std::expected<void, Error<WgNlError>>;

/// Get the last-handshake timestamp (UNIX seconds) for a
/// peer, or 0 if no handshake has completed. Used to verify
/// that a direct-path endpoint actually works.
auto WgNlGetPeerHandshake(WgNetlink* wg, const char* ifname,
                          const uint8_t* public_key,
                          uint64_t* handshake_sec)
    -> std::expected<void, Error<WgNlError>>;

/// Assign IP address and bring interface up (ioctl).
auto WgNlConfigureAddr(const char* ifname,
                       uint32_t addr, int prefix_len)
    -> std::expected<void, Error<WgNlError>>;

/// Close netlink socket.
void WgNlClose(WgNetlink* wg);

}  // namespace hyper_derp

#endif  // CLIENT_WG_NETLINK_H_
