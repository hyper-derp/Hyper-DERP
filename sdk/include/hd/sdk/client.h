/// @file client.h
/// @brief Main SDK entry point.

#ifndef HD_SDK_CLIENT_H_
#define HD_SDK_CLIENT_H_

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "hd/sdk/config.h"
#include "hd/sdk/error.h"
#include "hd/sdk/frame_pool.h"
#include "hd/sdk/peer_info.h"
#include "hd/sdk/tunnel.h"

#include "hyper_derp/hd_protocol.h"

namespace hd::sdk {

/// Connection state of the client.
enum class Status {
  Disconnected,
  Connecting,
  Connected,
  Error,
};

using PeerCallback =
    std::function<void(const PeerInfo& peer,
                       bool connected)>;
using ErrorCallback =
    std::function<void(const Error& error)>;

/// Fired when the relay sends a Redirect (0x22). Gives
/// the application a chance to veto or log. If the
/// callback returns true (the default), the SDK auto-
/// reconnects to `target_url`.
using RedirectCallback =
    std::function<bool(hyper_derp::HdRedirectReason reason,
                       const std::string& target_url)>;

/// HD Protocol client.
/// Move-only. Manages relay connection, peer discovery,
/// and tunnel lifecycle.
class Client {
 public:
  /// Create a client with the given config.
  /// Connects to the relay, performs TLS + enrollment.
  static Result<Client> Create(const ClientConfig& config);

  ~Client();
  Client(Client&& other) noexcept;
  Client& operator=(Client&& other) noexcept;
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  // -- Lifecycle -------------------------------------------------------------

  /// Start the I/O thread (Own mode) or prepare for
  /// Poll (External mode).
  Result<> Start();

  /// Stop: disconnect, close all tunnels, join thread.
  void Stop();

  /// Blocking event loop. Returns when Stop() is called
  /// from a callback or signal handler.
  void Run();

  /// Non-blocking: process pending events and return.
  /// Use with EventThread::External.
  void Poll();

  /// File descriptor that becomes readable when events
  /// are pending. Use with your own event loop.
  int EventFd() const;

  /// Current client status.
  Status GetStatus() const;

  // -- Tunnels ---------------------------------------------------------------

  /// Open a tunnel to a named peer.
  Result<Tunnel> Open(const std::string& peer_name);

  /// Open with per-tunnel options.
  Result<Tunnel> Open(const std::string& peer_name,
                      const TunnelOptions& opts);

  // -- Peers -----------------------------------------------------------------

  /// Snapshot of currently known peers.
  std::vector<PeerInfo> ListPeers() const;

  // -- Callbacks (thread-safe) -----------------------------------------------

  /// Peer connect/disconnect events.
  void SetPeerCallback(PeerCallback cb);

  /// Non-fatal error events (logging, metrics).
  void SetErrorCallback(ErrorCallback cb);

  /// Relay Redirect events. If the callback returns true
  /// (default), the SDK auto-reconnects to the target URL.
  void SetRedirectCallback(RedirectCallback cb);

  // -- Raw access (advanced) -------------------------------------------------

  /// Send raw MeshData to a peer by ID.
  Result<> SendMeshData(uint16_t dst_peer_id,
                        std::span<const uint8_t> data);

  /// Allocate a frame from the zero-copy pool.
  /// Returns nullptr if pool is exhausted.
  std::unique_ptr<FrameBuffer> AllocFrame();

 private:
  Client();
  struct Impl;
  Impl* impl_ = nullptr;

  static bool DoConnect(Impl* impl);
  static void RecvLoop(Impl* impl);
  static void DispatchFrame(Impl* impl,
                            hyper_derp::HdFrameType ftype,
                            const uint8_t* buf, int len);
};

}  // namespace hd::sdk

#endif  // HD_SDK_CLIENT_H_
