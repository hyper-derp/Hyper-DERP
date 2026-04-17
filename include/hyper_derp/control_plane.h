/// @file control_plane.h
/// @brief DERP control plane: peer registry, watcher
///   notifications, and control frame processing.
///
/// Implements the server-side DERP control frames:
///   WatchConns, PeerPresent, PeerGone, Ping/Pong,
///   NotePreferred, ClosePeer.
///
/// The data plane pipes non-transport frames here via
/// per-worker pipes. The control thread reads those pipes
/// and dispatches to the appropriate handlers.

#ifndef INCLUDE_HYPER_DERP_CONTROL_PLANE_H_
#define INCLUDE_HYPER_DERP_CONTROL_PLANE_H_

#include <atomic>
#include <cstdint>
#include <mutex>

#include "hyper_derp/ice.h"
#include "hyper_derp/protocol.h"
#include "hyper_derp/types.h"

namespace hyper_derp {

/// Maximum tracked peers in the control plane registry.
inline constexpr int kCpMaxPeers = 4096;

/// Maximum concurrent watchers.
inline constexpr int kCpMaxWatchers = 64;

/// Per-pipe read buffer size.
inline constexpr int kPipeBufSize = 65536 + 64;

/// Pipe message header: [4B fd][1B type][4B len].
inline constexpr int kPipeMsgHeader = 9;

/// @brief Registered peer entry in the control plane.
struct CpPeer {
  Key key{};
  int fd = -1;
  uint8_t occupied = 0;  // 0=empty, 1=live, 2=tombstone
  uint8_t preferred = 0;
};

/// @brief Per-pipe read buffer for partial message
///   reassembly.
struct PipeReader {
  uint8_t buf[kPipeBufSize];
  int len = 0;
};

/// @brief Control plane state.
struct ControlPlane {
  Ctx* data_plane = nullptr;

  // Peer registry (open-addressing hash table).
  CpPeer peers[kCpMaxPeers]{};
  CpPeer* fd_map[kMaxFd]{};
  int peer_count = 0;

  // Watcher fds (peers that sent WatchConns).
  int watcher_fds[kCpMaxWatchers]{};
  int watcher_count = 0;

  // Per-worker pipe readers.
  PipeReader readers[kMaxWorkers];

  // epoll fd for multiplexing pipe reads.
  int epoll_fd = -1;

  // Thread synchronization.
  std::mutex mutex;
  std::atomic<int> running{0};

  // -- Level 2 ICE integration ----------------------
  // ICE agent pointer (owned by Server).
  IceAgent* ice_agent = nullptr;
  // timerfd for periodic ICE connectivity checks.
  int ice_timer_fd = -1;
  // UDP socket for STUN binding requests/responses.
  int stun_udp_fd = -1;
  // Level 2 enabled flag.
  bool level2_enabled = false;
};

/// @brief Initialize the control plane.
/// @param cp Control plane state to initialize.
/// @param dp Data plane context (for DpWrite/DpRemovePeer).
void CpInit(ControlPlane* cp, Ctx* dp);

/// @brief Destroy the control plane, freeing resources.
/// @param cp Control plane to destroy.
void CpDestroy(ControlPlane* cp);

/// @brief Register a newly connected peer.
///
/// Called from the accept thread after DpAddPeer. Notifies
/// all watchers with PeerPresent.
/// @param cp Control plane.
/// @param key Peer's 32-byte public key.
/// @param fd Peer's socket fd.
void CpOnPeerConnect(ControlPlane* cp,
                     const Key& key, int fd);

/// @brief Handle peer disconnect.
///
/// Called when the data plane reports a peer gone. Notifies
/// all watchers with PeerGone.
/// @param cp Control plane.
/// @param key Peer's 32-byte public key.
/// @param fd Peer's socket fd.
/// @param reason Disconnect reason.
void CpOnPeerGone(ControlPlane* cp,
                  const Key& key, int fd,
                  PeerGoneReason reason);

/// @brief Process a control frame from the data plane pipe.
/// @param cp Control plane.
/// @param fd Source peer's socket fd.
/// @param type Frame type.
/// @param payload Frame payload.
/// @param payload_len Payload length.
void CpProcessFrame(ControlPlane* cp, int fd,
                    FrameType type,
                    const uint8_t* payload,
                    int payload_len);

/// @brief Control thread entry point.
///
/// Reads from all data plane control pipes via epoll,
/// parses pipe messages, and dispatches to CpProcessFrame.
/// Blocks until CpStop is called.
/// @param cp Control plane.
void CpRunLoop(ControlPlane* cp);

/// @brief Signal the control thread to stop.
/// @param cp Control plane.
void CpStop(ControlPlane* cp);

/// @brief Enable Level 2 ICE in the control plane.
///
/// Sets the ICE agent pointer, creates a timerfd for
/// periodic connectivity checks, and binds a UDP socket
/// for STUN traffic. Must be called before CpRunLoop.
/// @param cp Control plane.
/// @param agent ICE agent (owned by Server).
/// @param stun_port UDP port for STUN binding.
/// @return 0 on success, -1 on failure.
int CpEnableLevel2(ControlPlane* cp,
                   IceAgent* agent,
                   uint16_t stun_port);

/// @brief Process an HD PeerInfo frame from a peer.
///
/// Parses remote ICE candidates from the payload and
/// advances the ICE state machine for the peer pair.
/// @param cp Control plane.
/// @param fd Source peer's socket fd.
/// @param payload PeerInfo payload (after HD frame header).
/// @param payload_len Payload length.
void CpHandleHdPeerInfo(ControlPlane* cp, int fd,
                        const uint8_t* payload,
                        int payload_len);

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_CONTROL_PLANE_H_
