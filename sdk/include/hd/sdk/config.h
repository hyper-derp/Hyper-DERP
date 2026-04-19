/// @file config.h
/// @brief Client and tunnel configuration.

#ifndef HD_SDK_CONFIG_H_
#define HD_SDK_CONFIG_H_

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace hd::sdk {

/// Routing intent for a tunnel.
enum class Intent {
  PreferDirect,
  RequireDirect,
  PreferRelay,
  RequireRelay,
};

/// Connection mode of a tunnel.
enum class Mode {
  Pending,
  Direct,
  Relayed,
  Closed,
};

/// Event thread model.
enum class EventThread {
  Own,       // SDK manages its own I/O thread.
  External,  // Caller drives via Poll().
};

/// Client configuration.
struct ClientConfig {
  /// Relay URL (e.g. "hd://relay.example.com:443" or
  /// just "host:port").
  std::string relay_url;

  /// Enrollment key (hex string).
  std::string relay_key;

  /// Path to persist/load client keypair.
  std::string key_path;

  /// Default routing intent for new tunnels.
  Intent default_routing = Intent::PreferDirect;
  bool allow_upgrade = true;
  bool allow_downgrade = true;

  /// Keepalive interval.
  std::chrono::seconds keepalive{25};

  /// Zero-copy buffer pool size.
  uint32_t frame_pool_size = 4096;

  /// Threading model.
  EventThread event_thread = EventThread::Own;

  /// Auto-reconnect on disconnect.
  bool auto_reconnect = true;
  int reconnect_initial_ms = 1000;
  int reconnect_max_ms = 30000;

  /// Enable TLS (default true).
  bool tls = true;
};

/// Per-tunnel options (override client defaults).
struct TunnelOptions {
  std::optional<Intent> routing;
  std::optional<bool> allow_upgrade;
  std::optional<bool> allow_downgrade;
  std::string reason;
};

}  // namespace hd::sdk

#endif  // HD_SDK_CONFIG_H_
