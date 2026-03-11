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

#include <cstdint>
#include <pthread.h>

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
  int len;
};

/// @brief Control plane state.
struct ControlPlane {
  Ctx* data_plane;

  // Peer registry (open-addressing hash table).
  CpPeer peers[kCpMaxPeers];
  CpPeer* fd_map[kMaxFd];
  int peer_count;

  // Watcher fds (peers that sent WatchConns).
  int watcher_fds[kCpMaxWatchers];
  int watcher_count;

  // Per-worker pipe readers.
  PipeReader readers[kMaxWorkers];

  // epoll fd for multiplexing pipe reads.
  int epoll_fd;

  // Thread synchronization.
  pthread_mutex_t mutex;
  int running;
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
/// @param arg Pointer to ControlPlane.
/// @returns nullptr.
void* CpRunLoop(void* arg);

/// @brief Signal the control thread to stop.
/// @param cp Control plane.
void CpStop(ControlPlane* cp);

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_CONTROL_PLANE_H_
