/// @file wg_config.h
/// @brief hd-wg daemon configuration.

#ifndef CLIENT_WG_CONFIG_H_
#define CLIENT_WG_CONFIG_H_

#include <cstdint>
#include <string>

namespace hyper_derp {

struct WgDaemonConfig {
  std::string relay_host;
  uint16_t relay_port = 3341;
  std::string relay_key_hex;

  std::string wg_private_key_hex;
  std::string wg_interface = "wg0";
  uint16_t wg_listen_port = 51820;

  std::string tunnel_cidr;  // "10.99.0.1/24"

  uint16_t proxy_port = 51821;

  std::string stun_server;  // "stun.l.google.com:19302"
  int stun_timeout_ms = 5000;

  int keepalive_secs = 25;
  std::string log_level = "info";
};

/// Load config from YAML file.
/// Returns true on success.
bool WgLoadConfig(const char* path,
                  WgDaemonConfig* config);

}  // namespace hyper_derp

#endif  // CLIENT_WG_CONFIG_H_
