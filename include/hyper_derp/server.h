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
#include "hyper_derp/einheit_channel.h"
#include "hyper_derp/fleet_controller.h"
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

// Forward decl — full def lives in wg_relay.h. Holding a
// pointer here keeps server.h dependency-light.
struct WgRelay;

/// Daemon operating mode. Selected at startup; not
/// live-mutable. `kDerp` runs the full DERP/HD/kTLS
/// stack; `kWireguard` runs a transparent UDP forwarder
/// for stock WireGuard clients.
enum class DaemonMode : uint8_t {
  kDerp = 0,
  kWireguard = 1,
};

/// WireGuard relay mode configuration. Only consulted
/// when DaemonMode::kWireguard is selected. The relay
/// observes nothing about WireGuard semantics — peer
/// table and link table are operator-set; forwarding is
/// pure 4-tuple based on source endpoint.
struct WgRelayConfig {
  /// UDP port the relay binds for WG traffic.
  uint16_t port = 51820;
  /// Roster persistence path. Each `wg peer add` /
  /// `wg link add` rewrites this atomically; startup
  /// loads it before binding the UDP socket.
  std::string roster_path;
  /// Static peers, loaded from yaml at startup before
  /// the persisted roster (yaml entries take
  /// precedence on name collisions).
  struct PeerEntry {
    std::string name;
    std::string endpoint;       // "ip:port"
    std::string pubkey_b64;     // optional, metadata only
    std::string label;
    std::string nic;            // optional NIC binding
  };
  std::vector<PeerEntry> peers;
  /// Static links, loaded from yaml.
  struct LinkEntry {
    std::string a;
    std::string b;
  };
  std::vector<LinkEntry> links;
  /// NIC name(s) for XDP attachment. A single name keeps
  /// the iteration-1 same-NIC XDP_TX behaviour (e.g.
  /// "enp1s0"). A comma-separated list (e.g.
  /// "ens4f0np0,ens4f1np1") attaches the BPF program to
  /// each NIC and uses XDP_REDIRECT through the devmap to
  /// cross between them, with each peer pinned to its
  /// own NIC via PeerEntry::nic. Empty disables the XDP
  /// fast path entirely; userspace forwarder handles
  /// every packet.
  std::string xdp_interface;
  /// Path to the compiled BPF object. Defaults to a
  /// CMake-installed location; rarely set explicitly.
  std::string xdp_bpf_obj_path;
};

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
  /// Configuration invariant violated (e.g. HD mode
  /// enabled without TLS).
  ConfigInvalid,
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
    case ServerError::ConfigInvalid:
      return "ConfigInvalid";
  }
  return "Unknown";
}

/// Server configuration.
struct ServerConfig {
  /// Operating mode. Default keeps backward-compatible
  /// DERP behaviour. `kWireguard` runs the WG relay
  /// forwarder on its own UDP port and ignores the
  /// DERP/HD plumbing.
  DaemonMode mode = DaemonMode::kDerp;
  /// WireGuard relay configuration; only consulted in
  /// kWireguard mode.
  WgRelayConfig wg;
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
  /// Policy constraints applied to auto-approval.
  HdEnrollPolicy hd_enroll_policy;
  /// Path to persistent denylist file (empty = in-memory
  /// only, no persistence across restarts).
  std::string hd_denylist_path;
  /// Path to per-peer policy file (empty = in-memory
  /// only, admin changes do not survive restart).
  std::string hd_peer_policy_path;
  /// Relay-operator routing policy (static, read at
  /// startup; Phase 6 replaces with signed bundle pull).
  HdRelayPolicy hd_relay_policy;
  /// Fleet-wide policy loaded from disk (Phase 4 reads a
  /// local YAML path; Phase 6 replaces with bundle pull).
  HdFleetPolicy hd_fleet_policy;
  /// Path to a fleet-policy YAML file. Empty = no fleet
  /// policy configured.
  std::string hd_fleet_policy_path;
  /// Federation policy (applied at the gateway on
  /// inbound FleetOpenConnection frames).
  HdFederationPolicy hd_federation_policy;
  /// Path to the routing-policy audit log. Empty = no
  /// file sink (ring-only, still queryable via REST).
  std::string hd_audit_log_path;
  /// Rotate audit log when it reaches this size, keeping
  /// `hd_audit_log_keep` rotated files.
  uint64_t hd_audit_log_max_bytes = 100 * 1024 * 1024;
  /// Number of rotated files to keep (.1..N).
  int hd_audit_log_keep = 10;
  /// Fleet controller (signed-bundle puller). Empty URL
  /// disables.
  struct FleetControllerOptions {
    std::string url;
    std::string signing_pubkey_b64;
    std::string client_cert;
    std::string client_key;
    std::string ca_bundle;
    int poll_interval_secs = 60;
    std::string bundle_cache_path;
  } hd_fleet_controller;
  /// einheit control-plane ZMQ endpoint (ipc:// ...). Empty
  /// disables the einheit channel (falls back to legacy
  /// ctl_channel only).
  std::string einheit_ctl_endpoint;
  /// einheit event PUB endpoint. Empty disables events.
  std::string einheit_pub_endpoint;
  /// Append-only commit log for live candidate-config
  /// changes. Each successful `commit` appends one
  /// tab-separated record; on startup the channel replays
  /// the log so committed changes survive a restart.
  /// Empty disables persistence.
  std::string einheit_commit_log_path;
  /// This relay's fleet ID (0 = standalone, no fleet).
  uint16_t hd_relay_id = 0;
  /// Seed relays for fleet bootstrapping ("host:port").
  std::vector<std::string> seed_relays;
  std::array<int, kMaxWorkers> pin_cores{};
  /// Per-peer receive rate limit in bytes/sec. 0 = unlimited.
  uint64_t peer_rate_limit = 0;

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
  FleetController fleet_controller{};
  bool fleet_controller_started = false;
  EinheitChannel* einheit_channel = nullptr;

  /// WireGuard relay subsystem. Non-null iff the daemon
  /// was started in DaemonMode::kWireguard. Owned by the
  /// entrypoint; einheit `wg_*` handlers reach it via
  /// this pointer.
  WgRelay* wg_relay = nullptr;

  // Level 2 (direct path) subsystems.
  IceAgent ice_agent{};
  TurnManager* turn_manager = nullptr;
  XdpContext xdp_ctx{};
  bool level2_enabled = false;

  // Server-level HD counters (incremented from accept
  // thread, read with relaxed atomics).
  std::atomic<uint64_t> hd_enrollments{0};
  std::atomic<uint64_t> hd_auth_failures{0};
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

/// @brief Drains in-flight connections before shutdown.
///
/// Closes the listen socket to stop new connections, then
/// waits up to 5 seconds for in-flight sends to complete.
/// @param server Running server.
void ServerDrain(Server* server);

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
