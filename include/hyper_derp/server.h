/// @file server.h
/// @brief DERP relay server: TCP listener, HTTP upgrade,
///   handshake, and data plane integration.

#ifndef INCLUDE_HYPER_DERP_SERVER_H_
#define INCLUDE_HYPER_DERP_SERVER_H_

#include <array>
#include <atomic>
#include <cstdint>
#include <expected>
#include <string_view>
#include <thread>

#include "hyper_derp/control_plane.h"
#include "hyper_derp/data_plane.h"
#include "hyper_derp/error.h"
#include "hyper_derp/handshake.h"
#include "hyper_derp/metrics.h"

namespace hyper_derp {

/// Error codes for ServerInit / ServerRun.
enum class ServerError {
  /// Key generation failed.
  KeyGenFailed,
  /// Data plane initialization failed.
  DataPlaneInitFailed,
  /// socket() call failed.
  SocketFailed,
  /// bind() call failed.
  BindFailed,
  /// listen() call failed.
  ListenFailed,
  /// pthread_create failed.
  ThreadCreateFailed,
  /// Data plane run loop returned an error.
  DataPlaneRunFailed,
};

/// Human-readable name for a ServerError code.
constexpr auto ServerErrorName(ServerError e)
    -> std::string_view {
  switch (e) {
    case ServerError::KeyGenFailed:
      return "KeyGenFailed";
    case ServerError::DataPlaneInitFailed:
      return "DataPlaneInitFailed";
    case ServerError::SocketFailed:
      return "SocketFailed";
    case ServerError::BindFailed:
      return "BindFailed";
    case ServerError::ListenFailed:
      return "ListenFailed";
    case ServerError::ThreadCreateFailed:
      return "ThreadCreateFailed";
    case ServerError::DataPlaneRunFailed:
      return "DataPlaneRunFailed";
  }
  return "Unknown";
}

/// Server configuration.
struct ServerConfig {
  uint16_t port = 3340;
  int num_workers = 0;  // 0 = auto (hardware_concurrency)
  /// Per-socket send/recv buffer size in bytes. 0 = use
  /// the OS default. Capped by net.core.wmem_max/rmem_max.
  int sockbuf_size = 0;
  /// Metrics HTTP server configuration.
  MetricsConfig metrics;
  std::array<int, kMaxWorkers> pin_cores{};

  ServerConfig() { pin_cores.fill(-1); }
};

/// Top-level server state.
struct Server {
  ServerConfig config;
  ServerKeys keys;
  Ctx data_plane{};
  ControlPlane control_plane{};
  int listen_fd = -1;
  std::atomic<int> running{0};
  std::thread accept_thread;
  std::thread control_thread;
  MetricsServer* metrics_server = nullptr;
};

/// @brief Initializes the server.
/// @param server Pointer to uninitialized Server.
/// @param config Server configuration.
/// @returns void on success, or ServerError.
auto ServerInit(Server* server,
                const ServerConfig* config)
    -> std::expected<void, Error<ServerError>>;

/// @brief Starts the server (blocks until stopped).
/// @param server Initialized server.
/// @returns void on success, or ServerError.
auto ServerRun(Server* server)
    -> std::expected<void, Error<ServerError>>;

/// @brief Signals the server to stop.
/// @param server Running server.
void ServerStop(Server* server);

/// @brief Tears down the server, freeing all resources.
/// @param server Server to destroy.
void ServerDestroy(Server* server);

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_SERVER_H_
