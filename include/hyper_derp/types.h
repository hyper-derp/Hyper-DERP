/// @file types.h
/// @brief Core data-oriented types for the DERP relay.

#ifndef INCLUDE_HYPER_DERP_TYPES_H_
#define INCLUDE_HYPER_DERP_TYPES_H_

#include <cstdint>
#include <liburing.h>
#include <pthread.h>

#include "hyper_derp/protocol.h"

namespace hyper_derp {

// -- Tuning constants --------------------------------------------------------

/// Hash table capacity (must be power of 2).
inline constexpr int kHtCapacity = 4096;

/// Command ring buffer size (must be power of 2).
inline constexpr int kCmdRingSize = 1024;

/// Per-source SPSC transfer ring size (must be power of 2).
/// Each worker has one ring per source worker. Sized to
/// prevent xfer_drops at high worker counts under load.
/// N workers → N×N rings total.
inline constexpr int kXferSpscSize = 16384;

/// Maximum worker threads.
inline constexpr int kMaxWorkers = 32;

/// Maximum file descriptor value tracked.
inline constexpr int kMaxFd = 65536;

/// Per-peer read buffer size.
inline constexpr int kReadBufSize =
    kFrameHeaderSize + kMaxFramePayload;

/// Slab allocator pool size (per worker).
/// Sized to absorb send queue depth during small-packet
/// bursts where recv batches produce hundreds of frames.
inline constexpr int kSlabSize = 65536;


/// io_uring submission queue depth.
inline constexpr int kUringQueueDepth = 4096;

/// Maximum concurrent in-flight sends per peer.
/// At 250 peers/worker, 16 × 250 = 4000 fits within the
/// 4096-entry SQ with headroom for recv and poll SQEs.
inline constexpr int kMaxSendsInflight = 16;

/// Maximum sends submitted per POLLOUT event. Prevents
/// a single peer from monopolizing the SQ after becoming
/// writable.
inline constexpr int kPollWriteBatch = 16;

/// Maximum CQEs processed per batch iteration. Prevents
/// recv burst avalanches from generating unbounded SQEs.
inline constexpr int kMaxCqeBatch = 256;

/// Maximum queued sends per peer. Per-peer drop limit.
inline constexpr int kMaxSendQueueDepth = 2048;

/// Absolute cap on send pressure before recv pauses.
/// Adaptive threshold scales with peer count but is clamped
/// to this value. Must stay below kSlabSize.
inline constexpr int kSendPressureMax = 32768;

/// Per-peer contribution to the adaptive send pressure
/// threshold. Effective high = min(kSendPressureMax,
/// peer_count * kPressurePerPeer).
inline constexpr int kPressurePerPeer = 512;

/// Compute the adaptive recv-pause threshold for a given
/// peer count. Scales linearly so low peer counts get a
/// tight threshold (preventing per-peer drops) while high
/// counts get a looser one.
inline constexpr int SendPressureHigh(int peer_count) {
  int h = peer_count * kPressurePerPeer;
  if (h < kPressurePerPeer) h = kPressurePerPeer;
  if (h > kSendPressureMax) h = kSendPressureMax;
  return h;
}

/// Resume divisor scaled by peer count. Low worker configs
/// (1-2 workers, ≤12 peers) need wider hysteresis to
/// prevent backpressure oscillation that causes latency
/// stalls.
inline constexpr int PressureResumeDiv(int peer_count) {
  if (peer_count <= 12) return 8;
  if (peer_count <= 24) return 6;
  return 4;
}

/// Compute the recv-resume threshold.
inline constexpr int SendPressureLow(int peer_count) {
  return SendPressureHigh(peer_count) /
         PressureResumeDiv(peer_count);
}

/// Minimum CQE batch iterations that recv stays paused
/// after triggering. Prevents rapid toggle when drain
/// rate is high.
inline constexpr int kRecvPauseMinBatches = 8;

/// Busy-spin iterations before blocking wait.
inline constexpr int kBusySpinDefault = 256;

/// Reduced busy-spin for 1-2 worker configs. Gives kTLS
/// more CPU time to drain send queues on constrained
/// configs where each worker shares a core with the
/// kernel kTLS thread.
inline constexpr int kBusySpinLowWorker = 64;

/// Maximum concurrent recv SQEs per worker.
inline constexpr int kRecvBudget = 512;

/// Deferred recv ring buffer size.
inline constexpr int kRecvDeferSize = 4096;

/// Provided buffer count per worker. Higher values reduce
/// ENOBUFS re-arms under multishot recv.
inline constexpr int kPbufCount = 512;

/// Provided buffer size per buffer.
inline constexpr int kPbufSize = kReadBufSize;

/// Frame pool buffer size (per worker). Covers typical
/// forwarded WireGuard frames (5 + 32 + 1400 = 1437 bytes).
/// Frames larger than this fall back to malloc.
inline constexpr int kFramePoolBufSize = 2048;

/// Frame pool count per worker (2048 * 16384 = 32 MiB).
/// Sized for ~500us of in-flight sends at 20 Gbps
/// (1.7M frames/sec × 0.5ms ≈ 850 outstanding).
/// 16384 provides 19× headroom for cross-shard delays.
inline constexpr int kFramePoolCount = 16384;

// -- Operation and command tags ----------------------------------------------

/// io_uring operation tags encoded in user_data.
enum Op : uint8_t {
  kOpRecv = 1,
  kOpSend = 2,
  kOpPollCmd = 3,
  kOpPollXfer = 4,
  kOpPollWrite = 5,
};

/// Command types for the per-worker command ring.
enum CmdType : uint8_t {
  kCmdAddPeer = 1,
  kCmdRemovePeer = 2,
  kCmdWrite = 3,
  kCmdStop = 4,
  kCmdSetFwdRule = 5,
  kCmdMovePeerOut = 6,
};

// -- Data-oriented structs ---------------------------------------------------

/// Linked-list node for pending sends.
struct SendItem {
  uint8_t* data;
  int len;
  SendItem* next;
};

/// Protocol type for each peer connection.
enum class PeerProtocol : uint8_t {
  kDerp = 0,
  kHd   = 1,
};

/// Per-peer mutable state. Stored in a flat hash table
/// indexed by peer key. Fields are ordered hot-to-cold.
/// rbuf is allocated separately to keep the hot struct
/// small for hash table probing (~96 bytes vs ~1540).
struct Peer {
  // --- Cache line 0: recv + forwarding hot path ---
  // Recv: touched on every received frame.
  int fd;
  int rbuf_len;
  uint8_t occupied;  // 0=empty, 1=live, 2=tombstone
  PeerProtocol protocol = PeerProtocol::kDerp;
  uint16_t peer_id = 0;
  // HD 1:1 forwarding: touched on every forwarded HD
  // Data frame. Same cache line as fd/occupied — zero
  // additional cache misses for the common case.
  int fwd_count = 0;
  int fwd1_dst_fd = -1;
  int fwd1_dst_worker = -1;
  Peer* fwd1_peer = nullptr;
  // Remaining line 0 space: send-path fields that fit.
  int send_inflight;
  int send_queued;
  int send_pending;  // Queued items await first submit.

  // --- Cache line 1: send path ---
  SendItem* send_head;
  SendItem* send_tail;
  SendItem* send_next;
  int no_zc;
  int zc_draining;
  int poll_write_pending;

  // Full rule table for multi-rule/MeshData paths.
  static constexpr int kMaxPeerRules = 16;
  struct FwdRule {
    Peer* peer;
    Key key;
    int dst_fd;
    int dst_worker;
  };
  FwdRule fwd[kMaxPeerRules]{};

  // Cold: accessed on connect/disconnect.
  Key key;

  // Per-peer rate limiting.
  uint64_t recv_bytes_window = 0;  // Bytes in current window.
  uint64_t window_start_ns = 0;   // Window start time.

  // Per-peer read buffer for frame reassembly (heap).
  uint8_t* rbuf;
};

/// Replicated routing table entry. Each worker has a full
/// copy; writers use atomic store with release ordering.
struct Route {
  Key key;
  int worker_id;
  int fd;
  int occupied;  // Atomic: 0=empty, 1=live, 2=tombstone
};

/// Command passed from control plane to a worker thread.
struct Cmd {
  CmdType type;
  int fd;
  Key key;
  uint8_t* data;
  int data_len;
  PeerProtocol protocol = PeerProtocol::kDerp;
  uint16_t peer_id = 0;  // HD peer ID for MeshData routing.
  Key dst_key;  // For kCmdSetFwdRule.
  int retries = 0;  // Self-retry counter.
};

/// Cross-shard transfer item. Carries a pre-framed buffer
/// to a destination peer on another worker.
struct Xfer {
  int dst_fd;
  uint8_t* frame;
  int frame_len;
  uint32_t dst_gen;
};

/// Per-worker statistics (read with relaxed atomics).
struct WorkerStats {
  uint64_t send_drops;
  uint64_t xfer_drops;
  uint64_t slab_exhausts;
  uint64_t recv_bytes;
  uint64_t send_bytes;
  uint64_t send_epipe;
  uint64_t send_econnreset;
  uint64_t send_eagain;
  uint64_t send_other_err;
  uint64_t recv_enobufs;
  uint64_t send_queue_drops;
  uint64_t sq_overflow;
  uint64_t recv_pauses;
  uint64_t frame_pool_hits;
  uint64_t frame_pool_misses;
  uint64_t hd_mesh_forwards;
  uint64_t hd_fleet_forwards;
  uint64_t rate_limit_drops;
  uint64_t hd_fwd_no_rule;
  uint64_t hd_fwd_no_route;
  uint64_t hd_fwd_same_worker;
};

/// SPSC ring buffer for commands to a worker.
struct CmdRing {
  Cmd cmds[kCmdRingSize];
  uint32_t head;
  uint32_t tail;
};

/// SPSC ring buffer for cross-shard transfers.
/// One ring per (source worker, destination worker) pair.
/// No lock needed — single producer, single consumer.
/// head/tail on separate cache lines to avoid false sharing.
struct XferSpscRing {
  Xfer items[kXferSpscSize];
  alignas(64) uint32_t head;
  alignas(64) uint32_t tail;
};

// Forward declaration.
struct Ctx;

/// Per-worker thread state. Owns a disjoint set of peers
/// and an io_uring instance.
struct Worker {
  int id;
  Ctx* ctx;
  int running;
  int ring_exited;

  // io_uring instance.
  struct io_uring ring;
  int use_fixed_files;
  int use_provided_bufs;
  uint8_t* provided_bufs;
  struct io_uring_buf_ring* buf_ring;

  // Peer hash table (open-addressing, linear probe).
  Peer ht[kHtCapacity];

  // fd → Peer* map for O(1) lookup by fd.
  Peer* fd_map[kMaxFd];

  // HD peer ID → Peer* for MeshData O(1) routing.
  // Allocated lazily on first HD peer (65536 × 8 = 512 KB).
  Peer** peer_id_map = nullptr;

  // Per-fd generation counter (guards against fd reuse
  // races in cross-shard transfers).
  uint32_t fd_gen[kMaxFd];

  // Per-fd list of send items awaiting ZC notification.
  SendItem* notif_map[kMaxFd];

  // Replicated routing table (one copy per worker).
  Route routes[kHtCapacity];

  // Command ring (control plane → worker).
  CmdRing cmd_ring;

  // Cross-shard transfer inbox: one SPSC ring per source
  // worker. xfer_inbox[i] is written by worker i only.
  XferSpscRing* xfer_inbox[kMaxWorkers];

  // eventfd for waking the worker (commands).
  int event_fd;

  // eventfd for waking the worker (cross-shard transfers).
  int xfer_efd;

  // Write end of the control pipe (for non-transport
  // frames that need Go/control-plane handling).
  int pipe_wr;

  // Recv flow control.
  int recv_inflight;
  int recv_defer_buf[kRecvDeferSize];
  int recv_defer_head;
  int recv_defer_tail;

  // Send-pressure-based recv pause.
  int send_pressure;         // Total queued sends.
  int recv_paused;           // 1 = paused due to pressure.
  int recv_pause_countdown;  // Batches before Low check.
  int peer_count;            // Active peers on this worker.
  int busy_spins;            // Busy-spin limit.

  // Deferred send flush (batch coalescing).
  int pending_fds[kMaxCqeBatch];
  int pending_count;

  // Batched cross-shard eventfd signaling.
  // Bit i set means worker i needs a signal after this
  // CQE batch finishes.
  uint32_t xfer_signal_pending;

  // Bitmask of source workers with pending data in this
  // worker's xfer_inbox. Set atomically by senders
  // (fetch_or), consumed atomically by ProcessXfer
  // (exchange). On separate cache line from writer-side
  // fields to avoid false sharing with senders.
  alignas(64) uint32_t pending_sources;

  // Multishot recv support (kernel 6.0+).
  int use_multishot_recv;

  // SQPOLL mode (kernel thread submits on our behalf).
  int use_sqpoll;

  // Slab allocator for SendItem nodes.
  SendItem* slab_items;
  SendItem* slab_item_free;

  // Frame pool allocator (fixed-size buffer pool).
  uint8_t* frame_pool;
  uint8_t* frame_pool_end;
  void* frame_pool_free;

  // Per-source SPSC return inboxes for cross-worker buffer
  // returns. Worker i pushes to owner->frame_return_inbox[i].
  // Each slot is a FrameNode* linked list head.
  alignas(64) void* frame_return_inbox[kMaxWorkers];

  // Per-worker stats.
  WorkerStats stats;

  // Worker thread handle.
  pthread_t thread;
};

/// Per-peer receive rate limit (bytes/sec). 0 = unlimited.
/// Configurable via ServerConfig.
inline constexpr uint64_t kDefaultPeerRateLimit = 0;

/// Top-level data plane context.
struct Ctx {
  Worker* workers[kMaxWorkers];
  int num_workers;
  int pipe_rds[kMaxWorkers];
  int pin_cores[kMaxWorkers];  // -1 = no pinning
  /// Per-socket send/recv buffer size. 0 = OS default.
  int sockbuf_size;
  /// SQPOLL mode: kernel thread polls SQ on our behalf.
  int sqpoll;
  /// Per-peer receive rate limit (bytes/sec, 0 = unlimited).
  uint64_t peer_rate_limit = kDefaultPeerRateLimit;
  /// This relay's fleet ID (0 = standalone).
  uint16_t relay_id = 0;
  /// Relay-to-relay routing: relay_id -> peer_id of the
  /// neighbor relay's HD connection on this relay.
  /// Indexed by destination relay_id. 0 = no mapping.
  uint16_t relay_peer_map[65536]{};
};

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_TYPES_H_
