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

/// Cross-shard transfer ring size (must be power of 2).
/// Sized to absorb small-packet bursts: a single provided
/// buffer recv can contain hundreds of 64B frames, all
/// pushed before the consumer drains.
inline constexpr int kXferRingSize = 65536;

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

/// Resume ratio: recv resumes when pressure drops to 1/4
/// of the effective high threshold.
inline constexpr int kPressureResumeDiv = 4;

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

/// Compute the recv-resume threshold (1/kPressureResumeDiv
/// of the high threshold).
inline constexpr int SendPressureLow(int peer_count) {
  return SendPressureHigh(peer_count) / kPressureResumeDiv;
}

/// Maximum concurrent recv SQEs per worker.
inline constexpr int kRecvBudget = 512;

/// Deferred recv ring buffer size.
inline constexpr int kRecvDeferSize = 4096;

/// Provided buffer count per worker. Higher values reduce
/// ENOBUFS re-arms under multishot recv.
inline constexpr int kPbufCount = 512;

/// Provided buffer size per buffer.
inline constexpr int kPbufSize = kReadBufSize;

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
};

// -- Data-oriented structs ---------------------------------------------------

/// Linked-list node for pending sends.
struct SendItem {
  uint8_t* data;
  int len;
  SendItem* next;
};

/// Per-peer mutable state. Stored in a flat hash table
/// indexed by peer key. Fields are ordered hot-to-cold.
struct Peer {
  // Hot: accessed every packet.
  int fd;
  int rbuf_len;
  uint8_t occupied;  // 0=empty, 1=live, 2=tombstone

  // Warm: accessed on send path.
  SendItem* send_head;
  SendItem* send_tail;
  SendItem* send_next;
  int send_inflight;
  int send_queued;
  int no_zc;
  int zc_draining;
  int poll_write_pending;

  // Cold: accessed on connect/disconnect.
  Key key;

  // Per-peer read buffer for frame reassembly.
  uint8_t rbuf[kReadBufSize];
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
};

/// SPSC ring buffer for commands to a worker.
struct CmdRing {
  Cmd cmds[kCmdRingSize];
  uint32_t head;
  uint32_t tail;
};

/// MPSC ring buffer for cross-shard transfers.
/// head/tail are on separate cache lines to avoid
/// false sharing between producer and consumer.
struct XferRing {
  Xfer items[kXferRingSize];
  alignas(64) uint32_t head;
  alignas(64) uint32_t tail;
  char lock;
  uint32_t push_count;
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

  // Per-fd generation counter (guards against fd reuse
  // races in cross-shard transfers).
  uint32_t fd_gen[kMaxFd];

  // Per-fd list of send items awaiting ZC notification.
  SendItem* notif_map[kMaxFd];

  // Replicated routing table (one copy per worker).
  Route routes[kHtCapacity];

  // Command ring (control plane → worker).
  CmdRing cmd_ring;

  // Cross-shard transfer inbox.
  XferRing xfer_ring;

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
  int send_pressure;  // Total queued sends across all peers.
  int recv_paused;    // 1 = recv paused due to send pressure.
  int peer_count;     // Active peers on this worker.

  // Multishot recv support (kernel 6.0+).
  int use_multishot_recv;

  // Slab allocator for SendItem nodes.
  SendItem* slab_items;
  SendItem* slab_item_free;


  // Per-worker stats.
  WorkerStats stats;

  // Worker thread handle.
  pthread_t thread;
};

/// Top-level data plane context.
struct Ctx {
  Worker* workers[kMaxWorkers];
  int num_workers;
  int pipe_rds[kMaxWorkers];
  int pin_cores[kMaxWorkers];  // -1 = no pinning
  /// Per-socket send/recv buffer size. 0 = OS default.
  int sockbuf_size;
};

}  // namespace hyper_derp

#endif  // INCLUDE_HYPER_DERP_TYPES_H_
