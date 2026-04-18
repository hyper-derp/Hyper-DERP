/// @file server.h
/// @brief DERP relay server: TCP listener, HTTP upgrade,
///   handshake, and data plane integration.

#ifndef INCLUDE_HYPER_DERP_SERVER_H_
#define INCLUDE_HYPER_DERP_SERVER_H_

#include <array>
#include <atomic>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "hyper_derp/control_plane.h"
#include "hyper_derp/data_plane.h"
#include "hyper_derp/error.h"
#include "hyper_derp/handshake.h"
#include "hyper_derp/hd_peers.h"
#include "hyper_derp/hd_relay_table.h"
#include "hyper_derp/ice.h"
#include "hyper_derp/ktls.h"
#include "hyper_derp/metrics.h"
#include "hyper_derp/turn.h"
#include "hyper_derp/xdp_loader.h"

namespace hyper_derp {

/// Connection level for a peer pair. Level 0 (DERP) is
/// managed by the standard DERP data plane. Level 1 (HD) and
/// Level 2 (Direct) are HD protocol extensions.
enum class ConnectionLevel : uint8_t {
  kDerp = 0,    // Level 0: standard DERP relay
  kHd = 1,      // Level 1: HD protocol relay
  kDirect = 2,  // Level 2: direct path (ICE)
};

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
  /// kTLS initialization failed.
  KtlsInitFailed,
  /// Level 2 initialization failed.
  Level2InitFailed,
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
    case ServerError::KtlsInitFailed:
      return "KtlsInitFailed";
    case ServerError::Level2InitFailed:
      return "Level2InitFailed";
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
  /// Maximum accepted connections per second.
  /// 0 = unlimited (no rate limiting).
  int max_accept_per_sec = 0;
  /// TLS certificate + key for data plane connections.
  /// If both are set, kTLS is enabled.
  std::string tls_cert;
  std::string tls_key;
  /// Use SQPOLL mode (kernel thread polls SQ).
  bool sqpoll = false;
  /// Metrics HTTP server configuration.
  MetricsConfig metrics;
  /// HD Protocol relay key (32-byte shared secret).
  /// Empty string = HD protocol disabled.
  std::string hd_relay_key;
  /// HD enrollment mode.
  HdEnrollMode hd_enroll_mode = HdEnrollMode::kManual;
  /// This relay's fleet ID (0 = standalone, no fleet).
  uint16_t hd_relay_id = 0;
  /// Seed relays for fleet bootstrapping ("host:port").
  std::vector<std::string> seed_relays;
  std::array<int, kMaxWorkers> pin_cores{};

  /// Level 2 (direct path) configuration.
  struct Level2Config {
    bool enabled = false;
    uint16_t stun_port = 3478;
    /// NIC name for XDP attachment (e.g. "eth0").
    std::string xdp_interface;
    std::string turn_realm;
    int turn_max_allocations = 10000;
    int turn_default_lifetime = 600;
  } level2;

  ServerConfig() { pin_cores.fill(-1); }
};

/// Top-level server state.
struct Server {
  ServerConfig config;
  ServerKeys keys;
  Ctx data_plane{};
  ControlPlane control_plane{};
  KtlsCtx ktls_ctx{};
  bool ktls_enabled = false;
  HdPeerRegistry hd_peers{};
  RelayTable relay_table{};
  bool hd_enabled = false;
  int listen_fd = -1;
  std::atomic<int> running{0};
  std::thread accept_thread;
  std::thread control_thread;
  std::thread seed_thread;
  MetricsServer* metrics_server = nullptr;

  // Level 2 (direct path) subsystems.
  IceAgent ice_agent{};
  TurnManager* turn_manager = nullptr;
  XdpContext xdp_ctx{};
  bool level2_enabled = false;
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
/// @param stop_flag Optional atomic flag set by signal
///   handler. ServerRun polls this and calls ServerStop
///   from a safe context. May be nullptr.
/// @returns void on success, or ServerError.
auto ServerRun(Server* server,
               std::atomic<int>* stop_flag = nullptr)
    -> std::expected<void, Error<ServerError>>;

/// @brief Signals the server to stop.
/// @param server Running server.
void ServerStop(Server* server);

/// @brief Tears down the server, freeing all resources.
/// @param server Server to destroy.
void ServerDestroy(Server* server);

/// @brief Attempt Level 2 ICE upgrade for a new HD peer.
///
/// Called after a new HD peer connects. Checks the HD peer
/// registry for other approved peers with matching forwarding
/// rules and starts ICE sessions for each eligible pair.
/// @param server Server with Level 2 enabled.
/// @param new_peer_key The newly connected HD peer's key.
void TryIceUpgrade(Server* server,
                   const Key& new_peer_key);

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_SERVER_H_
