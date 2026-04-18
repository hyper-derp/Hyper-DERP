/// @file hd_sdk.h
/// @brief High-level HD Protocol client SDK.
///
/// Event-driven API with background recv thread, automatic
/// reconnect, and thread-safe callback registration.
///
/// Usage:
///   HdSdkConfig config{};
///   config.relay_host = "10.50.0.2";
///   config.relay_port = 3341;
///   config.relay_key = relay_key;
///
///   auto result = HdSdk::Create(config);
///   if (!result) { handle error }
///   auto sdk = std::move(*result);
///
///   sdk.OnPeerInfo([](const PeerEvent& e) {
///     printf("peer %d appeared\n", e.peer_id);
///   });
///   sdk.OnData([](uint16_t src, const uint8_t* d, int l) {
///     printf("data from %d: %d bytes\n", src, l);
///   });
///
///   sdk.Start();  // Begins recv loop + keepalive.
///   // ... sdk delivers events via callbacks ...
///   sdk.SendMeshData(peer_id, data, len);
///   sdk.Stop();

#ifndef SDK_HD_SDK_H_
#define SDK_HD_SDK_H_

#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "hyper_derp/error.h"
#include "hyper_derp/hd_client.h"
#include "hyper_derp/hd_protocol.h"
#include "hyper_derp/protocol.h"

namespace hyper_derp {

// -- Events ------------------------------------------------------------------

/// Peer discovery/departure event.
struct PeerEvent {
  Key key;
  uint16_t peer_id;
};

/// Connection state change.
enum class ConnectionState {
  kConnected,
  kDisconnected,
  kReconnecting,
};

// -- Callbacks ---------------------------------------------------------------

using DataCallback =
    std::function<void(const uint8_t* data, int len)>;
using MeshDataCallback =
    std::function<void(uint16_t src_peer_id,
                       const uint8_t* data, int len)>;
using PeerInfoCallback =
    std::function<void(const PeerEvent& event)>;
using PeerGoneCallback =
    std::function<void(const PeerEvent& event)>;
using ConnectionCallback =
    std::function<void(ConnectionState state)>;

// -- Configuration -----------------------------------------------------------

struct HdSdkConfig {
  std::string relay_host;
  uint16_t relay_port = 3341;
  Key relay_key{};

  /// Enable TLS (always true for production).
  bool tls = true;

  /// Automatic reconnect on disconnect.
  bool auto_reconnect = true;
  int reconnect_initial_delay_ms = 1000;
  int reconnect_max_delay_ms = 30000;

  /// Keepalive ping interval (ms). 0 = disabled.
  int keepalive_ms = 10000;
};

// -- SDK error ---------------------------------------------------------------

enum class HdSdkError {
  InitFailed,
  ConnectFailed,
  AlreadyRunning,
  NotRunning,
  NotConnected,
  SendFailed,
};

// -- SDK class ---------------------------------------------------------------

/// High-level HD Protocol client.
/// Move-only. Background thread handles recv + reconnect.
class HdSdk {
 public:
  /// Create and connect to relay.
  static auto Create(const HdSdkConfig& config)
      -> std::expected<HdSdk, Error<HdSdkError>>;

  ~HdSdk();

  // Move-only.
  HdSdk(HdSdk&& other) noexcept;
  HdSdk& operator=(HdSdk&& other) noexcept;
  HdSdk(const HdSdk&) = delete;
  HdSdk& operator=(const HdSdk&) = delete;

  // -- Lifecycle -------------------------------------------------------------

  /// Start the background recv loop.
  auto Start() -> std::expected<void, Error<HdSdkError>>;

  /// Stop the background recv loop.
  void Stop();

  /// Check if the recv loop is running.
  bool IsRunning() const;

  /// Check if connected to the relay.
  bool IsConnected() const;

  /// Our public key (Curve25519).
  const Key& PublicKey() const;

  /// Our peer ID assigned by the relay.
  uint16_t PeerId() const;

  // -- Callbacks (thread-safe registration) ----------------------------------

  void OnData(DataCallback cb);
  void OnMeshData(MeshDataCallback cb);
  void OnPeerInfo(PeerInfoCallback cb);
  void OnPeerGone(PeerGoneCallback cb);
  void OnConnection(ConnectionCallback cb);

  // -- Send (thread-safe) ----------------------------------------------------

  /// Send HD Data frame (broadcast to forwarding rule dests).
  auto SendData(const uint8_t* data, int len)
      -> std::expected<void, Error<HdSdkError>>;

  /// Send MeshData to a specific peer by peer_id.
  auto SendMeshData(uint16_t dst_peer_id,
                    const uint8_t* data, int len)
      -> std::expected<void, Error<HdSdkError>>;

  /// Send Ping (pong handled internally).
  auto SendPing()
      -> std::expected<void, Error<HdSdkError>>;

 private:
  HdSdk();
  struct Impl;
  static bool DoConnect(Impl* impl);
  static void RecvLoop(Impl* impl);
  Impl* impl_ = nullptr;
};

}  // namespace hyper_derp

#endif  // SDK_HD_SDK_H_
