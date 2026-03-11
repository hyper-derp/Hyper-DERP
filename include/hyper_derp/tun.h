/// @file tun.h
/// @brief Linux TUN device: create, configure, read/write
///   raw IP packets.

#ifndef INCLUDE_HYPER_DERP_TUN_H_
#define INCLUDE_HYPER_DERP_TUN_H_

#include <cstdint>

namespace hyper_derp {

/// @brief TUN device state.
struct TunDevice {
  int fd;
  char name[16];  // IFNAMSIZ
};

/// @brief Open a TUN device.
///
/// Creates a kernel TUN interface (no PI header). Requires
/// CAP_NET_ADMIN or root.
/// @param tun Device state to initialize.
/// @param name_hint Interface name pattern (e.g. "derp%d").
///   Pass nullptr for "derp%d".
/// @returns 0 on success, -1 on failure.
int TunOpen(TunDevice* tun, const char* name_hint);

/// @brief Assign an IPv4 address to the TUN interface.
/// @param tun Opened TUN device.
/// @param addr IPv4 address in network byte order.
/// @param prefix_len Prefix length (e.g. 24 for /24).
/// @returns 0 on success, -1 on failure.
int TunSetAddr(TunDevice* tun, uint32_t addr,
               int prefix_len);

/// @brief Bring the TUN interface up.
/// @param tun Opened TUN device.
/// @returns 0 on success, -1 on failure.
int TunBringUp(TunDevice* tun);

/// @brief Close the TUN device.
/// @param tun Device to close.
void TunClose(TunDevice* tun);

/// @brief Parse "a.b.c.d/N" into address and prefix length.
/// @param cidr CIDR string (e.g. "10.99.0.1/24").
/// @param addr Output: IPv4 address in network byte order.
/// @param prefix_len Output: prefix length.
/// @returns 0 on success, -1 on parse failure.
int ParseCidr(const char* cidr, uint32_t* addr,
              int* prefix_len);

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_TUN_H_
