/// @file data_plane.cc
/// @brief io_uring data plane — sharded workers with
///   zero-copy sends and cross-shard MPSC forwarding.
///
/// Adapted from tested C prototype. Each worker owns a
/// disjoint set of peers determined by hashing the peer
/// key. Cross-shard forwarding uses MPSC ring buffers
/// signaled via eventfd.
///
/// Sends use io_uring SEND_ZC (zero-copy) to eliminate
/// userspace-to-kernel copy. Recvs read into per-peer
/// reassembly buffers. Frame parsing uses the DERP 5-byte
/// header format (1 type + 4 length).

#include "hyper_derp/data_plane.h"

#include <spdlog/spdlog.h>

#include "hyper_derp/hd_bridge.h"
#include "hyper_derp/hd_protocol.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// Compat guards for io_uring features (kernel 6.0+).
#ifndef IORING_SETUP_SINGLE_ISSUER
#define IORING_SETUP_SINGLE_ISSUER (1U << 12)
#endif
#ifndef IORING_SETUP_DEFER_TASKRUN
#define IORING_SETUP_DEFER_TASKRUN (1U << 13)
#endif
#ifndef IORING_SETUP_SQPOLL
#define IORING_SETUP_SQPOLL (1U << 1)
#endif
#ifndef IORING_RECV_MULTISHOT
#define IORING_RECV_MULTISHOT (1U << 1)
#endif

namespace hyper_derp {

// -- Inline timing -----------------------------------------------------------

static inline uint64_t NowNsRelaxed() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

// -- Internal forward declarations -------------------------------------------

static void SlabFreeItem(Worker* w, SendItem* item);
static void FrameFree(Worker* w, uint8_t* buf);
static int WorkerInitRing(Worker* w);
static void DrainDeferredRecvs(Worker* w);
static void ForwardMsg(Worker* w, FrameType type,
                       uint8_t* payload, int payload_len,
                       Peer* src);
static void EnqueueSend(Worker* w, Peer* peer,
                        uint8_t* data, int len);
static uint8_t* FrameAlloc(Worker* w, int size);

// -- user_data encoding ------------------------------------------------------

static inline uint64_t MakeUserData(uint8_t op, int fd) {
  return (static_cast<uint64_t>(op) << 56) |
         static_cast<uint64_t>(static_cast<uint32_t>(fd));
}

static inline uint8_t UdOp(uint64_t ud) {
  return static_cast<uint8_t>(ud >> 56);
}

static inline int UdFd(uint64_t ud) {
  return static_cast<int>(static_cast<uint32_t>(
      ud & 0xFFFFFFFF));
}

// -- Hash helpers (FNV-1a) ---------------------------------------------------

static uint32_t Fnv1a(const uint8_t* data, int len) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < len; i++) {
    h ^= data[i];
    h *= 16777619u;
  }
  return h;
}

static int PeerWorker(int num_workers,
                      const Key& key) {
  return static_cast<int>(
      Fnv1a(key.data(), kKeySize) % num_workers);
}

// -- Per-worker hash table ---------------------------------------------------

static int HtFind(Worker* w, const uint8_t* key,
                  Peer** out) {
  uint32_t h = Fnv1a(key, kKeySize);
  uint32_t mask = kHtCapacity - 1;
  int first_tombstone = -1;

  for (uint32_t i = 0; i < kHtCapacity; i++) {
    uint32_t idx = (h + i) & mask;
    Peer* p = &w->ht[idx];

    if (p->occupied == 0) {
      if (first_tombstone >= 0) {
        *out = &w->ht[first_tombstone];
        return first_tombstone;
      }
      *out = p;
      return static_cast<int>(idx);
    }
    if (p->occupied == 2) {
      if (first_tombstone < 0) {
        first_tombstone = static_cast<int>(idx);
      }
      continue;
    }
    if (memcmp(p->key.data(), key, kKeySize) == 0) {
      *out = p;
      return static_cast<int>(idx);
    }
  }

  if (first_tombstone >= 0) {
    *out = &w->ht[first_tombstone];
    return first_tombstone;
  }
  *out = nullptr;
  return -1;
}

static Peer* HtLookup(Worker* w, const uint8_t* key) {
  Peer* p = nullptr;
  HtFind(w, key, &p);
  if (p && p->occupied == 1 &&
      memcmp(p->key.data(), key, kKeySize) == 0) {
    return p;
  }
  return nullptr;
}

static int HtInsert(Worker* w, int fd,
                    const Key& key) {
  Peer* p = nullptr;
  HtFind(w, key.data(), &p);
  if (!p) {
    return -1;
  }
  // Allocate rbuf on connect (kept off the hot struct).
  if (!p->rbuf) {
    p->rbuf = static_cast<uint8_t*>(
        malloc(kReadBufSize));
    if (!p->rbuf) {
      return -1;
    }
  }
  p->fd = fd;
  p->key = key;
  p->rbuf_len = 0;
  p->occupied = 1;
  p->send_head = nullptr;
  p->send_tail = nullptr;
  p->send_next = nullptr;
  p->send_inflight = 0;
  p->send_queued = 0;
  p->no_zc = 0;
  p->zc_draining = 0;
  p->poll_write_pending = 0;
  p->send_pending = 0;
  p->peer_id = 0;
  p->fwd_count = 0;
  for (int i = 0; i < Peer::kMaxPeerRules; i++) {
    p->fwd_peers[i] = nullptr;
    p->fwd_dst_worker[i] = -1;
    p->fwd_dst_fd[i] = -1;
  }
  p->recv_bytes_window = 0;
  p->window_start_ns = 0;
  return 0;
}

// Forward declaration (needed by HtRemove).
static void DrainDeferredRecvs(Worker* w);

static void HtRemove(Worker* w, const Key& key) {
  Peer* p = HtLookup(w, key.data());
  if (!p) {
    return;
  }
  SendItem* item = p->send_head;
  int inflight = p->send_inflight;
  while (item && inflight > 0) {
    SendItem* next = item->next;
    item->next = w->notif_map[p->fd];
    w->notif_map[p->fd] = item;
    item = next;
    inflight--;
  }
  while (item) {
    SendItem* next = item->next;
    FrameFree(w, item->data);
    SlabFreeItem(w, item);
    item = next;
  }
  w->send_pressure -= p->send_queued;
  if (w->send_pressure < 0) {
    w->send_pressure = 0;
  }
  p->send_head = nullptr;
  p->send_tail = nullptr;
  p->send_next = nullptr;
  p->send_inflight = 0;
  p->send_queued = 0;
  p->send_pending = 0;
  free(p->rbuf);
  p->rbuf = nullptr;
  p->rbuf_len = 0;
  p->occupied = 2;
  if (w->peer_count > 0) w->peer_count--;

  // Resume recv if removing this peer relieved pressure.
  if (w->recv_paused &&
      w->recv_pause_countdown <= 0 &&
      w->send_pressure <= SendPressureLow(w->peer_count)) {
    w->recv_paused = 0;
    DrainDeferredRecvs(w);
  }
}

// -- Replicated routing table ------------------------------------------------

static int RouteFind(Route* routes,
                     const uint8_t* key,
                     Route** out) {
  uint32_t h = Fnv1a(key, kKeySize);
  uint32_t mask = kHtCapacity - 1;
  int first_tombstone = -1;

  for (uint32_t i = 0; i < kHtCapacity; i++) {
    uint32_t idx = (h + i) & mask;
    Route* r = &routes[idx];
    int occ = __atomic_load_n(
        &r->occupied, __ATOMIC_ACQUIRE);

    if (occ == 0) {
      if (first_tombstone >= 0) {
        *out = &routes[first_tombstone];
        return first_tombstone;
      }
      *out = r;
      return static_cast<int>(idx);
    }
    if (occ == 2) {
      if (first_tombstone < 0) {
        first_tombstone = static_cast<int>(idx);
      }
      continue;
    }
    if (memcmp(r->key.data(), key, kKeySize) == 0) {
      *out = r;
      return static_cast<int>(idx);
    }
  }

  if (first_tombstone >= 0) {
    *out = &routes[first_tombstone];
    return first_tombstone;
  }
  *out = nullptr;
  return -1;
}

static Route* RouteLookup(Route* routes,
                          const uint8_t* key) {
  Route* r = nullptr;
  RouteFind(routes, key, &r);
  if (r &&
      __atomic_load_n(&r->occupied, __ATOMIC_ACQUIRE) == 1
      && memcmp(r->key.data(), key, kKeySize) == 0) {
    return r;
  }
  return nullptr;
}

static void RouteSet(Route* routes,
                     const Key& key,
                     int worker_id, int fd) {
  Route* r = nullptr;
  RouteFind(routes, key.data(), &r);
  if (!r) {
    return;
  }
  r->key = key;
  r->worker_id = worker_id;
  r->fd = fd;
  __atomic_store_n(&r->occupied, 1, __ATOMIC_RELEASE);
}

static void RouteRemove(Route* routes,
                        const uint8_t* key) {
  Route* r = RouteLookup(routes, key);
  if (r) {
    __atomic_store_n(&r->occupied, 2, __ATOMIC_RELEASE);
  }
}

static void BroadcastRouteAdd(
    Ctx* ctx, const Key& key,
    int worker_id, int fd) {
  for (int i = 0; i < ctx->num_workers; i++) {
    RouteSet(ctx->workers[i]->routes, key, worker_id, fd);
  }
}

static void BroadcastRouteRemove(
    Ctx* ctx, const Key& key) {
  for (int i = 0; i < ctx->num_workers; i++) {
    RouteRemove(ctx->workers[i]->routes, key.data());
  }
}

// -- Command ring (SPSC) ----------------------------------------------------

static void RingInit(CmdRing* r) {
  r->head = 0;
  r->tail = 0;
}

static int RingPush(CmdRing* r, const Cmd* cmd) {
  uint32_t next = (r->head + 1) % kCmdRingSize;
  if (next == r->tail) {
    return -1;
  }
  r->cmds[r->head] = *cmd;
  __atomic_store_n(&r->head, next, __ATOMIC_RELEASE);
  return 0;
}

static int RingPop(CmdRing* r, Cmd* cmd) {
  if (r->tail ==
      __atomic_load_n(&r->head, __ATOMIC_ACQUIRE)) {
    return -1;
  }
  *cmd = r->cmds[r->tail];
  __atomic_store_n(
      &r->tail, (r->tail + 1) % kCmdRingSize,
      __ATOMIC_RELEASE);
  return 0;
}

// -- Cross-shard transfer ring (SPSC per source) -----------------------------

static void XferSpscInit(XferSpscRing* r) {
  r->head = 0;
  r->tail = 0;
}

/// Push to a per-source SPSC ring. No lock — single
/// producer (source worker i) writes head, single consumer
/// (destination worker) reads tail.
/// Returns 1 if the ring was empty (signal needed),
/// 0 if consumer is already running, -1 if full.
static int XferSpscPush(XferSpscRing* r, const Xfer* x) {
  uint32_t cur_head = r->head;
  uint32_t cur_tail =
      __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
  uint32_t next = (cur_head + 1) & (kXferSpscSize - 1);
  if (next == cur_tail) {
    return -1;
  }
  r->items[cur_head] = *x;
  __atomic_store_n(&r->head, next, __ATOMIC_RELEASE);
  return (cur_head == cur_tail) ? 1 : 0;
}

static int XferSpscPop(XferSpscRing* r, Xfer* x) {
  uint32_t cur_tail = r->tail;
  if (cur_tail ==
      __atomic_load_n(&r->head, __ATOMIC_ACQUIRE)) {
    return -1;
  }
  *x = r->items[cur_tail];
  __atomic_store_n(
      &r->tail, (cur_tail + 1) & (kXferSpscSize - 1),
      __ATOMIC_RELEASE);
  return 0;
}

// -- eventfd helpers ---------------------------------------------------------

static void SignalEventfd(int efd) {
  uint64_t val = 1;
  (void)write(efd, &val, sizeof(val));
}

static void DrainEventfd(int efd) {
  uint64_t val;
  (void)read(efd, &val, sizeof(val));
}

// -- Per-worker slab allocator -----------------------------------------------

static int SlabInit(Worker* w) {
  w->slab_items = static_cast<SendItem*>(
      calloc(kSlabSize, sizeof(SendItem)));
  if (!w->slab_items) {
    return -1;
  }
  // Hint THP for the slab allocation.
  madvise(w->slab_items,
          kSlabSize * sizeof(SendItem),
          MADV_HUGEPAGE);
  w->slab_item_free = &w->slab_items[0];
  for (int i = 0; i < kSlabSize - 1; i++) {
    w->slab_items[i].next = &w->slab_items[i + 1];
  }
  w->slab_items[kSlabSize - 1].next = nullptr;
  return 0;
}

static void SlabDestroy(Worker* w) {
  free(w->slab_items);
  w->slab_items = nullptr;
  w->slab_item_free = nullptr;
}

static SendItem* SlabAllocItem(Worker* w) {
  if (w->slab_item_free) {
    SendItem* item = w->slab_item_free;
    w->slab_item_free = item->next;
    item->next = nullptr;
    return item;
  }
  return static_cast<SendItem*>(malloc(sizeof(SendItem)));
}

static void SlabFreeItem(Worker* w, SendItem* item) {
  if (item >= w->slab_items &&
      item < w->slab_items + kSlabSize) {
    item->next = w->slab_item_free;
    w->slab_item_free = item;
  } else {
    free(item);
  }
}

// -- Per-worker frame pool allocator -----------------------------------------

/// Freelist node overlaid on the first 8 bytes of each
/// free pool buffer. Only valid when the buffer is free.
struct FrameNode {
  FrameNode* next;
};

static int FramePoolInit(Worker* w) {
  static constexpr size_t kPoolBytes =
      static_cast<size_t>(kFramePoolCount) * kFramePoolBufSize;
  w->frame_pool = static_cast<uint8_t*>(
      calloc(kFramePoolCount, kFramePoolBufSize));
  if (!w->frame_pool) {
    return -1;
  }
  w->frame_pool_end = w->frame_pool + kPoolBytes;
  // Hint THP for the frame pool (8 MiB).
  madvise(w->frame_pool, kPoolBytes, MADV_HUGEPAGE);
  auto* head = reinterpret_cast<FrameNode*>(w->frame_pool);
  w->frame_pool_free = head;
  for (int i = 0; i < kFramePoolCount - 1; i++) {
    auto* cur = reinterpret_cast<FrameNode*>(
        w->frame_pool + i * kFramePoolBufSize);
    auto* nxt = reinterpret_cast<FrameNode*>(
        w->frame_pool + (i + 1) * kFramePoolBufSize);
    cur->next = nxt;
  }
  auto* last = reinterpret_cast<FrameNode*>(
      w->frame_pool +
      (kFramePoolCount - 1) * kFramePoolBufSize);
  last->next = nullptr;
  for (int i = 0; i < kMaxWorkers; i++) {
    w->frame_return_inbox[i] = nullptr;
  }
  return 0;
}

static void FramePoolDestroy(Worker* w) {
  free(w->frame_pool);
  w->frame_pool = nullptr;
  w->frame_pool_end = nullptr;
  w->frame_pool_free = nullptr;
  for (int i = 0; i < kMaxWorkers; i++) {
    w->frame_return_inbox[i] = nullptr;
  }
}

/// Push a buffer to the owning worker's per-source return
/// inbox. Only worker src_id writes to slot [src_id]; the
/// owner worker drains it with atomic exchange. CAS loop
/// handles the race where the consumer nulls the slot
/// between our load and store (retries at most once).
static void FrameReturnPush(Worker* ow, int src_id,
                             uint8_t* buf) {
  auto* node = reinterpret_cast<FrameNode*>(buf);
  void* old = __atomic_load_n(
      &ow->frame_return_inbox[src_id], __ATOMIC_ACQUIRE);
  for (;;) {
    node->next = static_cast<FrameNode*>(old);
    if (__atomic_compare_exchange_n(
            &ow->frame_return_inbox[src_id],
            &old, node, false,
            __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
      return;
    }
  }
}

/// Drain all per-source return inboxes into the local
/// freelist. Only the owning worker calls this.
static void FrameReturnDrain(Worker* w) {
  int n = w->ctx->num_workers;
  for (int src = 0; src < n; src++) {
    auto* list = static_cast<FrameNode*>(
        __atomic_exchange_n(
            &w->frame_return_inbox[src],
            nullptr, __ATOMIC_ACQUIRE));
    while (list) {
      FrameNode* next = list->next;
      list->next = static_cast<FrameNode*>(
          w->frame_pool_free);
      w->frame_pool_free = list;
      list = next;
    }
  }
}

/// Check if buf belongs to the given worker's pool.
static inline bool FramePoolOwns(const Worker* w,
                                 const uint8_t* buf) {
  return buf >= w->frame_pool && buf < w->frame_pool_end;
}

static uint8_t* FrameAlloc(Worker* w, int size) {
  if (size <= kFramePoolBufSize) {
    if (w->frame_pool_free) {
      auto* node = static_cast<FrameNode*>(
          w->frame_pool_free);
      w->frame_pool_free = node->next;
      w->stats.frame_pool_hits++;
      return reinterpret_cast<uint8_t*>(node);
    }
    // Try reclaiming cross-worker returns.
    FrameReturnDrain(w);
    if (w->frame_pool_free) {
      auto* node = static_cast<FrameNode*>(
          w->frame_pool_free);
      w->frame_pool_free = node->next;
      w->stats.frame_pool_hits++;
      return reinterpret_cast<uint8_t*>(node);
    }
  }
  w->stats.frame_pool_misses++;
  return static_cast<uint8_t*>(malloc(size));
}

/// Find which worker owns a pool buffer. Returns the owner
/// id, or -1 if buf is not from any pool. Uses stored
/// frame_pool / frame_pool_end for O(1) per-worker check.
static inline int FramePoolOwnerId(
    Worker* w, uint8_t* buf) {
  // Fast path: check local pool first (common case).
  if (FramePoolOwns(w, buf)) return w->id;
  // Check other workers' pools.
  Ctx* ctx = w->ctx;
  for (int i = 0; i < ctx->num_workers; i++) {
    if (i != w->id && FramePoolOwns(ctx->workers[i], buf))
      return i;
  }
  return -1;
}

static void FrameFree(Worker* w, uint8_t* buf) {
  if (!buf) return;
  // Fast path: local pool buffer.
  if (FramePoolOwns(w, buf)) {
    auto* node = reinterpret_cast<FrameNode*>(buf);
    node->next = static_cast<FrameNode*>(
        w->frame_pool_free);
    w->frame_pool_free = node;
    return;
  }
  // Cross-worker return via per-source SPSC inbox.
  int owner = FramePoolOwnerId(w, buf);
  if (owner >= 0) {
    FrameReturnPush(w->ctx->workers[owner], w->id, buf);
    return;
  }
  // Oversize or pre-pool buffer — malloc'd.
  free(buf);
}

// -- Enqueue helpers (called from control plane) -----------------------------

/// Maximum retries before dropping a command. Prevents
/// the accept loop from spinning forever when a worker's
/// command ring is full under extreme load.
static constexpr int kCmdPushRetries = 10000;

static int EnqueueCmd(Worker* w, const Cmd* cmd) {
  for (int i = 0; i < kCmdPushRetries; i++) {
    if (RingPush(&w->cmd_ring, cmd) == 0) {
      SignalEventfd(w->event_fd);
      return 0;
    }
    // Yield to let the worker drain commands.
    sched_yield();
  }
  // Worker is overwhelmed — signal anyway so it drains.
  SignalEventfd(w->event_fd);
  return -1;
}

void DpAddPeer(Ctx* ctx, int fd, const Key& key,
               PeerProtocol protocol, uint16_t peer_id) {
  int wid = PeerWorker(ctx->num_workers, key);
  DpAddPeerToWorker(ctx, fd, key, protocol, wid, peer_id);
}

void DpAddPeerToWorker(Ctx* ctx, int fd, const Key& key,
                       PeerProtocol protocol, int wid,
                       uint16_t peer_id) {
  if (wid < 0 || wid >= ctx->num_workers) {
    wid = PeerWorker(ctx->num_workers, key);
  }
  Cmd cmd{};
  cmd.type = kCmdAddPeer;
  cmd.fd = fd;
  cmd.key = key;
  cmd.protocol = protocol;
  cmd.peer_id = peer_id;
  EnqueueCmd(ctx->workers[wid], &cmd);
}

void DpRemovePeer(Ctx* ctx, const Key& key) {
  int wid = PeerWorker(ctx->num_workers, key);
  Cmd cmd{};
  cmd.type = kCmdRemovePeer;
  cmd.key = key;
  EnqueueCmd(ctx->workers[wid], &cmd);
}

void DpAddFwdRule(Ctx* ctx, const Key& peer_key,
                  const Key& dst_key) {
  int src_wid = PeerWorker(ctx->num_workers, peer_key);

  // Single command to the source worker. The worker
  // handles migration internally — no cross-worker
  // command ordering issues.
  Cmd cmd{};
  cmd.type = kCmdSetFwdRule;
  cmd.key = peer_key;
  cmd.dst_key = dst_key;
  EnqueueCmd(ctx->workers[src_wid], &cmd);
}

void DpWrite(Ctx* ctx, const Key& key,
             uint8_t* data, int data_len) {
  int wid = PeerWorker(ctx->num_workers, key);
  Cmd cmd{};
  cmd.type = kCmdWrite;
  cmd.key = key;
  cmd.data = data;
  cmd.data_len = data_len;
  EnqueueCmd(ctx->workers[wid], &cmd);
}

void DpStop(Ctx* ctx) {
  for (int i = 0; i < ctx->num_workers; i++) {
    __atomic_store_n(&ctx->workers[i]->running, 0,
                     __ATOMIC_RELEASE);
  }
  Cmd cmd{};
  cmd.type = kCmdStop;
  for (int i = 0; i < ctx->num_workers; i++) {
    EnqueueCmd(ctx->workers[i], &cmd);
  }
}

// -- Write helpers -----------------------------------------------------------

static void SetNonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
}

static int WriteAllBlocking(int fd, const uint8_t* buf,
                            int len) {
  int total = 0;
  while (total < len) {
    int n = write(fd, buf + total, len - total);
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN) {
        continue;
      }
      return -1;
    }
    if (n == 0) {
      return -1;
    }
    total += n;
  }
  return 0;
}

// -- Direct cross-shard send --------------------------------------------------

// -- io_uring SQE helpers ----------------------------------------------------

/// Get an SQE, flushing the submission queue if full.
/// Returns nullptr only if the ring is truly exhausted
/// (should not happen with kUringQueueDepth = 4096).
static inline struct io_uring_sqe* GetSqe(Worker* w) {
  struct io_uring_sqe* sqe = io_uring_get_sqe(&w->ring);
  if (sqe) {
    return sqe;
  }
  // SQ full — flush pending submissions and retry.
  io_uring_submit(&w->ring);
  sqe = io_uring_get_sqe(&w->ring);
  if (!sqe) {
    w->stats.sq_overflow++;
  }
  return sqe;
}

/// Submit a send SQE for a fd not owned by this worker.
/// Used for HD cross-shard forwarding. The buffer is
/// freed on completion via the bit-63 check in the CQE
/// loop (no Peer needed).
static void EnqueueSendDirect(Worker* w, int fd,
                              uint8_t* buf, int len) {
  struct io_uring_sqe* sqe = GetSqe(w);
  if (!sqe) {
    FrameFree(w, buf);
    return;
  }
  io_uring_prep_send(sqe, fd, buf, len,
                     MSG_DONTWAIT | MSG_NOSIGNAL);
  sqe->user_data = reinterpret_cast<uint64_t>(buf) |
                   (1ULL << 63);
  w->stats.send_bytes += len;
}

static void DeferRecv(Worker* w, int fd) {
  int next =
      (w->recv_defer_head + 1) % kRecvDeferSize;
  if (next == w->recv_defer_tail) {
    return;
  }
  w->recv_defer_buf[w->recv_defer_head] = fd;
  w->recv_defer_head = next;
}

static void SubmitRecvNow(Worker* w, int fd) {
  if (fd < 0 || fd >= kMaxFd || !w->fd_map[fd]) {
    return;
  }
  struct io_uring_sqe* sqe = GetSqe(w);
  if (!sqe) {
    DeferRecv(w, fd);
    return;
  }
  if (w->use_provided_bufs) {
    io_uring_prep_recv(sqe, fd, nullptr, 0, 0);
    if (w->use_multishot_recv) {
      sqe->ioprio |= IORING_RECV_MULTISHOT;
    }
    sqe->buf_group = w->id;
    sqe->flags |= IOSQE_BUFFER_SELECT;
  } else {
    Peer* p = w->fd_map[fd];
    int space = kReadBufSize - p->rbuf_len;
    if (space <= 0) {
      return;
    }
    io_uring_prep_recv(
        sqe, fd, p->rbuf + p->rbuf_len, space, 0);
  }
  if (w->use_fixed_files) {
    sqe->flags |= IOSQE_FIXED_FILE;
  }
  io_uring_sqe_set_data64(
      sqe, MakeUserData(kOpRecv, fd));
  w->recv_inflight++;
}

static void SubmitRecv(Worker* w, int fd) {
  if (fd < 0 || fd >= kMaxFd || !w->fd_map[fd]) {
    return;
  }
  if (w->recv_inflight >= kRecvBudget || w->recv_paused) {
    DeferRecv(w, fd);
    return;
  }
  SubmitRecvNow(w, fd);
}

static void DrainDeferredRecvs(Worker* w) {
  while (w->recv_defer_tail != w->recv_defer_head &&
         w->recv_inflight < kRecvBudget &&
         !w->recv_paused) {
    int fd = w->recv_defer_buf[w->recv_defer_tail];
    w->recv_defer_tail =
        (w->recv_defer_tail + 1) % kRecvDeferSize;
    if (fd >= 0 && fd < kMaxFd && w->fd_map[fd]) {
      SubmitRecvNow(w, fd);
    }
  }
}

static bool SubmitSend(Worker* w, int fd,
                       const uint8_t* data, int len,
                       int extra_flags) {
  struct io_uring_sqe* sqe = GetSqe(w);
  if (!sqe) {
    return false;
  }
  io_uring_prep_send(sqe, fd, data, len,
                     MSG_NOSIGNAL | extra_flags);
  if (w->use_fixed_files) {
    sqe->flags |= IOSQE_FIXED_FILE;
  }
  io_uring_sqe_set_data64(
      sqe, MakeUserData(kOpSend, fd));
  return true;
}

static bool SubmitSendZc(Worker* w, int fd,
                         const uint8_t* data, int len,
                         int extra_flags) {
  struct io_uring_sqe* sqe = GetSqe(w);
  if (!sqe) {
    return false;
  }
  io_uring_prep_send_zc(sqe, fd, data, len,
                        MSG_NOSIGNAL | extra_flags, 0);
  if (w->use_fixed_files) {
    sqe->flags |= IOSQE_FIXED_FILE;
  }
  io_uring_sqe_set_data64(
      sqe, MakeUserData(kOpSend, fd));
  return true;
}

static void SubmitPollMultishot(Worker* w, int fd,
                                uint8_t op) {
  struct io_uring_sqe* sqe = GetSqe(w);
  if (!sqe) {
    return;
  }
  io_uring_prep_poll_multishot(sqe, fd, POLLIN);
  if (w->use_fixed_files) {
    sqe->flags |= IOSQE_FIXED_FILE;
  }
  io_uring_sqe_set_data64(sqe, MakeUserData(op, fd));
}

/// Threshold below which regular send is used instead of
/// SEND_ZC. At WireGuard MTU (1437B framed), ZC's extra
/// notification CQE and page-pinning cost exceed the
/// memcpy it avoids. Only use ZC for jumbo frames where
/// copy cost actually dominates.
static constexpr int kZcThreshold = 4096;

static inline bool SubmitPeerSend(Worker* w, Peer* peer,
                                  const uint8_t* data,
                                  int len, int more) {
  int flags = more ? MSG_MORE : 0;
  if (peer->no_zc || len <= kZcThreshold) {
    return SubmitSend(w, peer->fd, data, len, flags);
  } else {
    return SubmitSendZc(w, peer->fd, data, len, flags);
  }
}

// -- Send queue management ---------------------------------------------------

static void EnqueueSend(Worker* w, Peer* peer,
                        uint8_t* data, int len) {
  if (peer->send_queued >= kMaxSendQueueDepth) {
    w->stats.send_queue_drops++;
    FrameFree(w, data);
    return;
  }

  SendItem* item = SlabAllocItem(w);
  if (!item) {
    w->stats.slab_exhausts++;
    FrameFree(w, data);
    return;
  }
  item->data = data;
  item->len = len;
  item->next = nullptr;

  if (peer->send_tail) {
    peer->send_tail->next = item;
  } else {
    peer->send_head = item;
  }
  peer->send_tail = item;
  peer->send_queued++;
  w->send_pressure++;

  if (!peer->send_next) {
    peer->send_next = item;
  }

  // Defer first send to allow batch coalescing: multiple
  // frames for the same peer accumulate during CQE
  // processing, then get flushed together with MSG_MORE.
  if (peer->send_inflight == 0 && !peer->send_pending &&
      !peer->zc_draining) {
    peer->send_pending = 1;
    if (w->pending_count < kMaxCqeBatch) {
      w->pending_fds[w->pending_count++] = peer->fd;
    }
  } else if (peer->send_inflight > 0 &&
             peer->send_inflight < kMaxSendsInflight &&
             peer->send_next && !peer->zc_draining) {
    // Peer already has inflight sends — keep pipeline
    // full immediately (no benefit from deferring).
    int more = (peer->send_next->next != nullptr);
    if (SubmitPeerSend(w, peer, peer->send_next->data,
                       peer->send_next->len, more)) {
      peer->send_next = peer->send_next->next;
      peer->send_inflight++;
    }
  }

  // Pause recv when send queues are deep — TCP flow
  // control propagates backpressure to senders.
  if (!w->recv_paused &&
      w->send_pressure >= SendPressureHigh(w->peer_count)) {
    w->recv_paused = 1;
    w->recv_pause_countdown = kRecvPauseMinBatches;
    w->stats.recv_pauses++;
  }
}

static void DrainSendQueue(Worker* w, Peer* peer,
                           int defer_notif) {
  if (peer->send_head) {
    SendItem* done = peer->send_head;
    peer->send_head = done->next;
    if (!peer->send_head) {
      peer->send_tail = nullptr;
      peer->send_next = nullptr;
    }
    if (defer_notif) {
      done->next = w->notif_map[peer->fd];
      w->notif_map[peer->fd] = done;
    } else {
      FrameFree(w, done->data);
      SlabFreeItem(w, done);
    }
    if (peer->send_queued > 0) peer->send_queued--;
    if (w->send_pressure > 0) w->send_pressure--;

    // Resume recv when send pressure drops and minimum
    // pause duration has elapsed.
    if (w->recv_paused) {
      if (w->recv_pause_countdown > 0) {
        w->recv_pause_countdown--;
      } else if (w->send_pressure <=
                 SendPressureLow(w->peer_count)) {
        w->recv_paused = 0;
        DrainDeferredRecvs(w);
      }
    }
  }

  if (peer->send_inflight > 0) {
    peer->send_inflight--;
  }

  if (peer->send_next &&
      peer->send_inflight < kMaxSendsInflight) {
    int more = (peer->send_next->next != nullptr);
    if (SubmitPeerSend(w, peer, peer->send_next->data,
                       peer->send_next->len, more)) {
      peer->send_next = peer->send_next->next;
      peer->send_inflight++;
    }
  }
}

/// Flush peers that deferred their first send during CQE
/// batch processing. Submitting them here means all frames
/// that arrived in the batch are already queued, so MSG_MORE
/// is set correctly and the kernel can coalesce TCP segments.
static void FlushPendingSends(Worker* w) {
  for (int i = 0; i < w->pending_count; i++) {
    int fd = w->pending_fds[i];
    if (fd < 0 || fd >= kMaxFd || !w->fd_map[fd]) {
      continue;
    }
    Peer* peer = w->fd_map[fd];
    peer->send_pending = 0;
    if (!peer->send_next || peer->zc_draining) {
      continue;
    }
    int submitted = 0;
    while (peer->send_next &&
           peer->send_inflight < kMaxSendsInflight &&
           submitted < kPollWriteBatch) {
      int more = (peer->send_next->next != nullptr);
      if (SubmitPeerSend(w, peer, peer->send_next->data,
                         peer->send_next->len, more)) {
        peer->send_next = peer->send_next->next;
        peer->send_inflight++;
        submitted++;
      } else {
        break;
      }
    }
  }
  w->pending_count = 0;
}

// -- HD frame dispatch -------------------------------------------------------

/// Returns the header size for the peer's protocol.
static inline int FrameHdrSize(const Peer* p) {
  return p->protocol == PeerProtocol::kHd
             ? kHdFrameHeaderSize
             : kFrameHeaderSize;
}

/// Read payload length from header bytes, protocol-aware.
static inline uint32_t ReadPeerPayloadLen(
    const Peer* p, const uint8_t* hdr) {
  if (p->protocol == PeerProtocol::kHd) {
    return HdReadPayloadLen(hdr);
  }
  return ReadPayloadLen(hdr);
}

/// Validate payload length, protocol-aware.
static inline bool IsValidPeerPayloadLen(
    const Peer* p, uint32_t len) {
  if (p->protocol == PeerProtocol::kHd) {
    return HdIsValidPayloadLen(len);
  }
  return IsValidPayloadLen(len);
}

// Forward declarations for HD forwarding.
// frame points to the complete HD frame (header+payload).
static void ForwardHdData(Worker* w, Peer* src,
                          const uint8_t* frame,
                          int frame_len);

/// Dispatch a complete HD frame.
static void DispatchHdFrame(Worker* w, Peer* peer,
                            const uint8_t* hdr,
                            int payload_len) {
  auto hd_type = HdReadFrameType(hdr);
  const uint8_t* payload = hdr + kHdFrameHeaderSize;

  if (hd_type == HdFrameType::kData) {
    int frame_len = kHdFrameHeaderSize + payload_len;
    // Fast path: 1 rule. Same code path as ForwardMsg.
    if (__builtin_expect(peer->fwd_count == 1, 1)) {
      uint8_t* buf = FrameAlloc(w, frame_len);
      if (!buf) return;
      memcpy(buf, hdr, frame_len);

      Peer* dst = peer->fwd_peers[0];
      if (dst && dst->occupied == 1) {
        // Same-shard: direct enqueue.
        EnqueueSend(w, dst, buf, frame_len);
      } else {
        // Lazy resolution: if the cached pointer or
        // route is stale/missing, re-resolve now.
        if (!dst || dst->occupied != 1) {
          // Try same-shard lookup first.
          dst = HtLookup(w, peer->fwd_keys[0].data());
          if (dst) {
            peer->fwd_peers[0] = dst;
            EnqueueSend(w, dst, buf, frame_len);
            return;
          }
        }
        // Cross-shard: xfer ring to destination worker.
        int dst_fd = peer->fwd_dst_fd[0];
        int dst_id = peer->fwd_dst_worker[0];
        if (dst_fd < 0) {
          Route* rt = RouteLookup(w->routes,
              peer->fwd_keys[0].data());
          if (rt) {
            peer->fwd_dst_worker[0] = rt->worker_id;
            peer->fwd_dst_fd[0] = rt->fd;
            dst_fd = rt->fd;
            dst_id = rt->worker_id;
          }
        }
        if (dst_fd < 0) {
          w->stats.hd_fwd_no_route++;
          FrameFree(w, buf);
          return;
        }
        if (dst_id == w->id ||
            dst_id >= w->ctx->num_workers) {
          w->stats.hd_fwd_same_worker++;
          FrameFree(w, buf);
          return;
        }
        Worker* dst_w = w->ctx->workers[dst_id];
        Xfer x{};
        x.dst_fd = dst_fd;
        x.frame = buf;
        x.frame_len = frame_len;
        x.dst_gen = __atomic_load_n(
            &dst_w->fd_gen[dst_fd],
            __ATOMIC_ACQUIRE);
        int rc = XferSpscPush(
            dst_w->xfer_inbox[w->id], &x);
        if (rc >= 0) {
          __atomic_fetch_or(
              &dst_w->pending_sources,
              1u << static_cast<unsigned>(w->id),
              __ATOMIC_RELEASE);
          if (rc == 1) {
            w->xfer_signal_pending |=
                (1u << static_cast<unsigned>(dst_id));
          }
        } else {
          FrameFree(w, buf);
          w->stats.xfer_drops++;
          // Count xfer drops as send pressure so
          // recv pauses when cross-shard can't keep up.
          w->send_pressure++;
        }
      }
      return;
    }
    ForwardHdData(w, peer, hdr, frame_len);
  } else if (hd_type == HdFrameType::kPing &&
             payload_len == kHdPingDataSize) {
    uint8_t pong[kHdFrameHeaderSize + kHdPingDataSize];
    HdBuildPong(pong, payload);
    uint8_t* buf = FrameAlloc(w, sizeof(pong));
    if (buf) {
      memcpy(buf, pong, sizeof(pong));
      EnqueueSend(w, peer, buf,
                  static_cast<int>(sizeof(pong)));
    }
  } else if (hd_type == HdFrameType::kMeshData) {
    // MeshData: [4B header][2B dst_peer_id][payload]
    // Route to a specific peer by local peer ID. The
    // frame is converted to a plain Data frame for the
    // receiver (strip mesh header, rewrite type).
    if (payload_len < kHdMeshDstSize) return;
    uint16_t dst_id = HdReadMeshDst(payload);

    // O(1) lookup via peer ID map.
    Peer* dst = nullptr;
    if (w->peer_id_map && dst_id > 0) {
      dst = w->peer_id_map[dst_id];
    }

    if (!dst || dst->occupied != 1) {
      // Destination not on this worker (cross-shard
      // mesh routing TBD).
      return;
    }

    // Authorization: source must have a forwarding rule
    // that includes this destination.
    bool authorized = false;
    for (int r = 0; r < peer->fwd_count; r++) {
      if (peer->fwd_peers[r] == dst) {
        authorized = true;
        break;
      }
    }
    if (!authorized) return;

    // Build a Data frame for the destination (strip
    // the 2B mesh destination header).
    int fwd_payload_len =
        payload_len - kHdMeshDstSize;
    int fwd_frame_len =
        kHdFrameHeaderSize + fwd_payload_len;
    uint8_t* buf = FrameAlloc(w, fwd_frame_len);
    if (!buf) return;
    HdWriteFrameHeader(buf, HdFrameType::kData,
        static_cast<uint32_t>(fwd_payload_len));
    memcpy(buf + kHdFrameHeaderSize,
           payload + kHdMeshDstSize,
           fwd_payload_len);
    EnqueueSend(w, dst, buf, fwd_frame_len);
    w->stats.hd_mesh_forwards++;
  } else if (hd_type == HdFrameType::kFleetData) {
    // FleetData: [4B header][2B relay_id][2B peer_id]
    // [payload]
    if (payload_len < kHdFleetDstSize) return;
    uint16_t dst_relay = HdReadFleetRelay(payload);
    uint16_t dst_peer = HdReadFleetPeer(payload);

    if (dst_relay == w->ctx->relay_id ||
        dst_relay == 0) {
      // Local delivery: route to local peer by ID.
      Peer* dst = nullptr;
      if (w->peer_id_map && dst_peer > 0) {
        dst = w->peer_id_map[dst_peer];
      }
      if (!dst || dst->occupied != 1) return;

      // Strip fleet header, deliver as Data.
      int fwd_len = payload_len - kHdFleetDstSize;
      int frame_len =
          kHdFrameHeaderSize + fwd_len;
      uint8_t* buf = FrameAlloc(w, frame_len);
      if (!buf) return;
      HdWriteFrameHeader(buf, HdFrameType::kData,
          static_cast<uint32_t>(fwd_len));
      memcpy(buf + kHdFrameHeaderSize,
             payload + kHdFleetDstSize, fwd_len);
      EnqueueSend(w, dst, buf, frame_len);
      w->stats.hd_fleet_forwards++;
    } else {
      // Remote delivery: forward to next-hop relay
      // peer via relay_peer_map.
      uint16_t next_peer =
          w->ctx->relay_peer_map[dst_relay];
      if (next_peer == 0 || !w->peer_id_map) return;
      Peer* relay_peer =
          w->peer_id_map[next_peer];
      if (!relay_peer || relay_peer->occupied != 1)
        return;

      // Forward entire FleetData frame as-is.
      int frame_len =
          kHdFrameHeaderSize + payload_len;
      uint8_t* buf = FrameAlloc(w, frame_len);
      if (!buf) return;
      memcpy(buf, hdr, frame_len);
      EnqueueSend(w, relay_peer, buf, frame_len);
      w->stats.hd_fleet_forwards++;
    }
  } else if (hd_type == HdFrameType::kPeerInfo ||
             hd_type == HdFrameType::kRouteAnnounce) {
    // Forward PeerInfo and RouteAnnounce to control
    // plane for processing. Pipe format: [4B fd BE]
    // [1B type][4B len BE][payload].
    uint8_t fd_buf[4];
    fd_buf[0] = static_cast<uint8_t>(peer->fd >> 24);
    fd_buf[1] = static_cast<uint8_t>(peer->fd >> 16);
    fd_buf[2] = static_cast<uint8_t>(peer->fd >> 8);
    fd_buf[3] = static_cast<uint8_t>(peer->fd);
    uint8_t tag = static_cast<uint8_t>(hd_type);
    uint8_t len_buf[4];
    len_buf[0] =
        static_cast<uint8_t>(payload_len >> 24);
    len_buf[1] =
        static_cast<uint8_t>(payload_len >> 16);
    len_buf[2] =
        static_cast<uint8_t>(payload_len >> 8);
    len_buf[3] =
        static_cast<uint8_t>(payload_len);
    (void)WriteAllBlocking(w->pipe_wr, fd_buf, 4);
    (void)WriteAllBlocking(w->pipe_wr, &tag, 1);
    (void)WriteAllBlocking(w->pipe_wr, len_buf, 4);
    (void)WriteAllBlocking(w->pipe_wr, payload,
                           payload_len);
  }
  // Other HD frame types (PeerGone) are control frames
  // handled elsewhere.
}

/// Dispatch a complete frame based on peer protocol.
/// ForwardMsg takes non-const payload for historical
/// reasons; the cast is safe because it only reads.
static inline void DispatchFrame(Worker* w, Peer* peer,
                                 const uint8_t* hdr,
                                 int payload_len) {
  if (peer->protocol == PeerProtocol::kHd) {
    DispatchHdFrame(w, peer, hdr, payload_len);
  } else {
    auto* payload = const_cast<uint8_t*>(
        hdr + kFrameHeaderSize);
    ForwardMsg(w, ReadFrameType(hdr),
               payload, payload_len, peer);
  }
}

// -- Forwarding (HD protocol) ------------------------------------------------

/// Forward a complete HD frame. Mirrors ForwardMsg's
/// inline pattern: one alloc, one lookup, one send.
/// No per-packet hash. No header rebuild.
static void ForwardHdData(Worker* w, Peer* src,
                          const uint8_t* frame,
                          int frame_len) {
  if (src->fwd_count <= 0) {
    // No rules: broadcast to all same-shard HD peers.
    for (int i = 0; i < kHtCapacity; i++) {
      Peer* dst = &w->ht[i];
      if (dst->occupied != 1 || dst == src) continue;
      if (dst->protocol != PeerProtocol::kHd) continue;
      uint8_t* buf = FrameAlloc(w, frame_len);
      if (!buf) continue;
      memcpy(buf, frame, frame_len);
      EnqueueSend(w, dst, buf, frame_len);
    }
    return;
  }

  // Rule-based: one alloc + send per rule, same pattern
  // as ForwardMsg.
  for (int r = 0; r < src->fwd_count; r++) {
    uint8_t* buf = FrameAlloc(w, frame_len);
    if (!buf) return;
    memcpy(buf, frame, frame_len);

    // Same-shard fast path (cached pointer).
    Peer* dst = src->fwd_peers[r];
    if (dst && dst->occupied == 1) {
      EnqueueSend(w, dst, buf, frame_len);
      continue;
    }
    // Lazy same-shard re-resolve.
    if (!dst || dst->occupied != 1) {
      dst = HtLookup(w, src->fwd_keys[r].data());
      if (dst) {
        src->fwd_peers[r] = dst;
        EnqueueSend(w, dst, buf, frame_len);
        continue;
      }
    }

    // Cross-shard: cached or lazy-resolved route.
    int dst_fd = src->fwd_dst_fd[r];
    int dst_id = src->fwd_dst_worker[r];
    if (dst_fd < 0) {
      Route* rt = RouteLookup(w->routes,
          src->fwd_keys[r].data());
      if (rt) {
        src->fwd_dst_worker[r] = rt->worker_id;
        src->fwd_dst_fd[r] = rt->fd;
        dst_fd = rt->fd;
        dst_id = rt->worker_id;
      }
    }
    if (dst_fd < 0 || dst_id == w->id ||
        dst_id >= w->ctx->num_workers) {
      FrameFree(w, buf);
      continue;
    }
    Worker* dst_w = w->ctx->workers[dst_id];
    Xfer x{};
    x.dst_fd = dst_fd;
    x.frame = buf;
    x.frame_len = frame_len;
    x.dst_gen = __atomic_load_n(
        &dst_w->fd_gen[dst_fd], __ATOMIC_ACQUIRE);
    int rc = XferSpscPush(
        dst_w->xfer_inbox[w->id], &x);
    if (rc >= 0) {
      __atomic_fetch_or(
          &dst_w->pending_sources,
          1u << static_cast<unsigned>(w->id),
          __ATOMIC_RELEASE);
      if (rc == 1) {
        w->xfer_signal_pending |=
            (1u << static_cast<unsigned>(dst_id));
      }
    } else {
      FrameFree(w, buf);
      w->stats.xfer_drops++;
      w->send_pressure++;
    }
  }

  // Forward to same-shard DERP peers via bridge.
  const uint8_t* hd_payload = frame + kHdFrameHeaderSize;
  int hd_payload_len = frame_len - kHdFrameHeaderSize;
  for (int i = 0; i < kHtCapacity; i++) {
    Peer* dst = &w->ht[i];
    if (dst->occupied != 1 || dst == src) continue;
    if (dst->protocol != PeerProtocol::kDerp) continue;
    uint8_t derp_frame[kFrameHeaderSize + kKeySize +
                       kMaxFramePayload];
    int derp_len = BridgeHdToDerp(
        hd_payload, hd_payload_len, src->key,
        derp_frame, sizeof(derp_frame));
    if (derp_len > 0) {
      uint8_t* buf = FrameAlloc(w, derp_len);
      if (buf) {
        memcpy(buf, derp_frame, derp_len);
        EnqueueSend(w, dst, buf, derp_len);
      }
    }
  }

}

// -- Forwarding (DERP protocol) ----------------------------------------------

static void SendToPipe(Worker* w, int fd,
                       uint8_t* msg, int msg_len) {
  // Send fd + length-prefixed message to control plane.
  uint8_t hdr[4];
  hdr[0] = static_cast<uint8_t>(fd >> 24);
  hdr[1] = static_cast<uint8_t>(fd >> 16);
  hdr[2] = static_cast<uint8_t>(fd >> 8);
  hdr[3] = static_cast<uint8_t>(fd);
  uint8_t len_hdr[kFrameHeaderSize];
  WriteFrameHeader(len_hdr,
                   static_cast<FrameType>(msg[0]),
                   static_cast<uint32_t>(msg_len));
  (void)WriteAllBlocking(w->pipe_wr, hdr, 4);
  (void)WriteAllBlocking(w->pipe_wr, len_hdr,
                         kFrameHeaderSize);
  (void)WriteAllBlocking(w->pipe_wr, msg, msg_len);
}

static void ForwardMsg(Worker* w, FrameType type,
                       uint8_t* payload, int payload_len,
                       Peer* src) {
  if (!__atomic_load_n(&w->running, __ATOMIC_ACQUIRE)) {
    return;
  }

  // Only SendPacket frames are forwarded peer-to-peer.
  if (type != FrameType::kSendPacket) {
    // Non-transport frames go to the control plane pipe.
    // Wire format: [4B fd BE][1B type][4B len BE][payload]
    uint8_t fd_buf[4];
    fd_buf[0] = static_cast<uint8_t>(src->fd >> 24);
    fd_buf[1] = static_cast<uint8_t>(src->fd >> 16);
    fd_buf[2] = static_cast<uint8_t>(src->fd >> 8);
    fd_buf[3] = static_cast<uint8_t>(src->fd);
    uint8_t tag = static_cast<uint8_t>(type);
    uint8_t len_buf[4];
    len_buf[0] = static_cast<uint8_t>(payload_len >> 24);
    len_buf[1] = static_cast<uint8_t>(payload_len >> 16);
    len_buf[2] = static_cast<uint8_t>(payload_len >> 8);
    len_buf[3] = static_cast<uint8_t>(payload_len);
    (void)WriteAllBlocking(w->pipe_wr, fd_buf, 4);
    (void)WriteAllBlocking(w->pipe_wr, &tag, 1);
    (void)WriteAllBlocking(w->pipe_wr, len_buf, 4);
    (void)WriteAllBlocking(w->pipe_wr, payload,
                           payload_len);
    return;
  }

  // SendPacket: payload = [32-byte dst key] [packet data]
  if (payload_len < kKeySize) {
    return;
  }

  const uint8_t* dst_key = payload;
  const uint8_t* pkt_data = payload + kKeySize;
  int pkt_len = payload_len - kKeySize;

  // Build a RecvPacket frame for the destination.
  int frame_len =
      kFrameHeaderSize + kKeySize + pkt_len;
  uint8_t* frame = FrameAlloc(w, frame_len);
  if (!frame) {
    return;
  }
  BuildRecvPacket(frame, src->key, pkt_data, pkt_len);

  // Same-shard fast path.
  Peer* dst = HtLookup(w, dst_key);
  if (dst) {
    if (dst->protocol == PeerProtocol::kHd) {
      // DERP source -> HD destination: bridge.
      FrameFree(w, frame);
      uint8_t hd_frame[kHdFrameHeaderSize +
                        kMaxFramePayload];
      int hd_len = BridgeDerpToHd(
          payload, payload_len,
          hd_frame, sizeof(hd_frame));
      if (hd_len > 0) {
        uint8_t* buf = FrameAlloc(w, hd_len);
        if (buf) {
          memcpy(buf, hd_frame, hd_len);
          EnqueueSend(w, dst, buf, hd_len);
        }
      }
      return;
    }
    EnqueueSend(w, dst, frame, frame_len);
    return;
  }

  // Cross-shard: lookup in replicated routing table.
  Route* route = RouteLookup(w->routes, dst_key);
  if (!route) {
    FrameFree(w, frame);
    return;
  }

  Xfer x{};
  x.dst_fd = route->fd;
  x.frame = frame;
  x.frame_len = frame_len;
  int dst_id = route->worker_id;
  Worker* dst_w = w->ctx->workers[dst_id];
  x.dst_gen = __atomic_load_n(
      &dst_w->fd_gen[route->fd], __ATOMIC_ACQUIRE);
  int rc = XferSpscPush(dst_w->xfer_inbox[w->id], &x);
  if (rc >= 0) {
    // Tell destination which inbox has data.
    __atomic_fetch_or(&dst_w->pending_sources,
                      1u << static_cast<unsigned>(w->id),
                      __ATOMIC_RELEASE);
    if (rc == 1) {
      // Ring was empty — mark for batched signal.
      w->xfer_signal_pending |=
          (1u << static_cast<unsigned>(dst_id));
    }
  } else {
    FrameFree(w, frame);
    w->stats.xfer_drops++;
    w->send_pressure++;
  }
}

static void NotifyPeerClose(Worker* w, Peer* peer) {
  // Send a PeerGone to the control plane pipe.
  uint8_t buf[kFrameHeaderSize + kKeySize + 1];
  int n = BuildPeerGone(buf, peer->key,
                        PeerGoneReason::kDisconnected);
  uint8_t fd_buf[4];
  fd_buf[0] = static_cast<uint8_t>(peer->fd >> 24);
  fd_buf[1] = static_cast<uint8_t>(peer->fd >> 16);
  fd_buf[2] = static_cast<uint8_t>(peer->fd >> 8);
  fd_buf[3] = static_cast<uint8_t>(peer->fd);
  (void)WriteAllBlocking(w->pipe_wr, fd_buf, 4);
  (void)WriteAllBlocking(w->pipe_wr, buf, n);
}

// -- Recv CQE handling -------------------------------------------------------

static void HandleRecvProvided(Worker* w,
                               struct io_uring_cqe* cqe,
                               int multishot_active) {
  int fd = UdFd(cqe->user_data);
  int bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
  uint8_t* buf =
      w->provided_bufs +
      static_cast<size_t>(bid) * kPbufSize;
  int buf_mask = io_uring_buf_ring_mask(kPbufCount);

  if (fd < 0 || fd >= kMaxFd || !w->fd_map[fd]) {
    goto return_buf;
  }

  {
    Peer* peer = w->fd_map[fd];

    if (cqe->res <= 0) {
      if (cqe->res == 0 || cqe->res == -ECONNRESET) {
        NotifyPeerClose(w, peer);
        // Stop resubmitting recv on this dead fd.
        w->fd_map[fd] = nullptr;
      }
      goto return_buf;
    }

    int n = cqe->res;
    w->stats.recv_bytes += n;

    // Per-peer rate limiting.
    if (w->ctx->peer_rate_limit > 0) {
      uint64_t now = NowNsRelaxed();
      if (now - peer->window_start_ns > 1000000000ULL) {
        peer->recv_bytes_window = 0;
        peer->window_start_ns = now;
      }
      peer->recv_bytes_window += n;
      if (peer->recv_bytes_window >
          w->ctx->peer_rate_limit) {
        w->stats.rate_limit_drops++;
        goto return_buf;
      }
    }

    int off = 0;

    // Complete any partial frame in rbuf.
    int hdr_size = FrameHdrSize(peer);
    if (peer->rbuf_len > 0) {
      while (peer->rbuf_len < hdr_size &&
             off < n) {
        peer->rbuf[peer->rbuf_len++] = buf[off++];
      }
      if (peer->rbuf_len < hdr_size) {
        goto return_buf;
      }

      uint32_t payload_len =
          ReadPeerPayloadLen(peer, peer->rbuf);
      if (!IsValidPeerPayloadLen(peer, payload_len)) {
        NotifyPeerClose(w, peer);
        peer->rbuf_len = 0;
        goto return_buf;
      }

      int frame_len = hdr_size +
                      static_cast<int>(payload_len);
      int need = frame_len - peer->rbuf_len;
      if (need > n - off) {
        memcpy(peer->rbuf + peer->rbuf_len,
               buf + off, n - off);
        peer->rbuf_len += (n - off);
        goto return_buf;
      }

      memcpy(peer->rbuf + peer->rbuf_len,
             buf + off, need);
      off += need;
      DispatchFrame(w, peer, peer->rbuf,
                    static_cast<int>(payload_len));
      peer->rbuf_len = 0;
    }

    // Process complete frames from provided buffer.
    // Cache running flag — worst case we process one
    // extra batch after shutdown signal.
    int running = __atomic_load_n(
        &w->running, __ATOMIC_ACQUIRE);
    while (running && off + hdr_size <= n) {
      uint32_t payload_len =
          ReadPeerPayloadLen(peer, buf + off);
      if (!IsValidPeerPayloadLen(peer, payload_len)) {
        NotifyPeerClose(w, peer);
        goto return_buf;
      }
      int frame_len = hdr_size +
                      static_cast<int>(payload_len);
      if (off + frame_len > n) {
        break;
      }
      DispatchFrame(w, peer, buf + off,
                    static_cast<int>(payload_len));
      off += frame_len;
    }

    // Save partial frame tail in rbuf.
    int tail = n - off;
    if (tail > 0) {
      memcpy(peer->rbuf, buf + off, tail);
      peer->rbuf_len = tail;
    }
  }

return_buf:
  io_uring_buf_ring_add(w->buf_ring, buf, kPbufSize,
                        bid, buf_mask, 0);
  io_uring_buf_ring_advance(w->buf_ring, 1);

  if (!multishot_active &&
      fd >= 0 && fd < kMaxFd && w->fd_map[fd]) {
    SubmitRecv(w, fd);
  }
}

static void HandleRecvSingle(Worker* w,
                             struct io_uring_cqe* cqe) {
  int fd = UdFd(cqe->user_data);

  if (fd < 0 || fd >= kMaxFd || !w->fd_map[fd]) {
    return;
  }

  Peer* peer = w->fd_map[fd];

  if (cqe->res <= 0) {
    if (cqe->res == 0 || cqe->res == -ECONNRESET) {
      NotifyPeerClose(w, peer);
      w->fd_map[fd] = nullptr;
    }
    return;
  }

  int n = cqe->res;
  w->stats.recv_bytes += n;
  peer->rbuf_len += n;

  int hdr_size = FrameHdrSize(peer);
  int running = __atomic_load_n(
      &w->running, __ATOMIC_ACQUIRE);
  while (running && peer->rbuf_len >= hdr_size) {
    uint32_t payload_len =
        ReadPeerPayloadLen(peer, peer->rbuf);
    if (!IsValidPeerPayloadLen(peer, payload_len)) {
      NotifyPeerClose(w, peer);
      return;
    }
    int frame_len = hdr_size +
                    static_cast<int>(payload_len);
    if (peer->rbuf_len < frame_len) {
      break;
    }

    DispatchFrame(w, peer, peer->rbuf,
                  static_cast<int>(payload_len));

    int remaining = peer->rbuf_len - frame_len;
    if (remaining > 0) {
      memmove(peer->rbuf, peer->rbuf + frame_len,
              remaining);
    }
    peer->rbuf_len = remaining;
  }

  if (peer->occupied == 1) {
    SubmitRecv(w, fd);
  }
}

static void HandleRecvCqe(Worker* w,
                          struct io_uring_cqe* cqe) {
  // Multishot recv: IORING_CQE_F_MORE means the SQE is
  // still active — don't decrement inflight or resubmit.
  int multishot_active = cqe->flags & IORING_CQE_F_MORE;
  if (!multishot_active) {
    w->recv_inflight--;
    if (w->recv_inflight < 0) {
      w->recv_inflight = 0;
    }
  }

  if (cqe->flags & IORING_CQE_F_BUFFER) {
    HandleRecvProvided(w, cqe, multishot_active);
    return;
  }

  if (w->use_provided_bufs) {
    int fd = UdFd(cqe->user_data);
    // EOF or connection reset without a buffer attached.
    if (cqe->res == 0 || cqe->res == -ECONNRESET) {
      if (fd >= 0 && fd < kMaxFd && w->fd_map[fd]) {
        NotifyPeerClose(w, w->fd_map[fd]);
        w->fd_map[fd] = nullptr;
      }
      return;
    }
    if (cqe->res == -ENOBUFS || cqe->res == -EINVAL) {
      if (cqe->res == -ENOBUFS) {
        w->stats.recv_enobufs++;
      }
      if (cqe->res == -EINVAL) {
        // Kernel doesn't support multishot recv;
        // fall back.
        w->use_multishot_recv = 0;
      }
      if (fd >= 0 && fd < kMaxFd && w->fd_map[fd]) {
        SubmitRecv(w, fd);
      }
    }
    return;
  }

  HandleRecvSingle(w, cqe);
}

// -- Send CQE handling -------------------------------------------------------

static void HandleSendCqe(Worker* w,
                          struct io_uring_cqe* cqe) {
  int fd = UdFd(cqe->user_data);

  if (cqe->flags & IORING_CQE_F_NOTIF) {
    if (fd >= 0 && fd < kMaxFd && w->notif_map[fd]) {
      SendItem* item = w->notif_map[fd];
      w->notif_map[fd] = item->next;
      FrameFree(w, item->data);
      SlabFreeItem(w, item);
    }
    return;
  }

  if (fd < 0 || fd >= kMaxFd || !w->fd_map[fd]) {
    if (!(cqe->flags & IORING_CQE_F_MORE) &&
        fd >= 0 && fd < kMaxFd) {
      while (w->notif_map[fd]) {
        SendItem* item = w->notif_map[fd];
        w->notif_map[fd] = item->next;
        FrameFree(w, item->data);
        SlabFreeItem(w, item);
      }
    }
    return;
  }

  Peer* peer = w->fd_map[fd];
  int defer_notif = cqe->flags & IORING_CQE_F_MORE;

  if (cqe->res < 0) {
    if (cqe->res == -EOPNOTSUPP) {
      peer->no_zc = 1;
      peer->zc_draining = 1;
      peer->send_inflight--;
      if (peer->send_inflight == 0) {
        peer->zc_draining = 0;
        peer->send_next = peer->send_head;
        while (peer->send_next &&
               peer->send_inflight <
                   kMaxSendsInflight) {
          int more =
              (peer->send_next->next != nullptr);
          if (SubmitPeerSend(w, peer,
                             peer->send_next->data,
                             peer->send_next->len,
                             more)) {
            peer->send_next = peer->send_next->next;
            peer->send_inflight++;
          } else {
            break;
          }
        }
      }
      return;
    }
    if (cqe->res == -EAGAIN) {
      w->stats.send_eagain++;
      peer->send_inflight--;
      SendItem* p = peer->send_head;
      for (int i = 0;
           i < peer->send_inflight && p; i++) {
        p = p->next;
      }
      peer->send_next = p;
      if (!peer->poll_write_pending) {
        struct io_uring_sqe* sqe = GetSqe(w);
        if (sqe) {
          io_uring_prep_poll_add(sqe, fd, POLLOUT);
          if (w->use_fixed_files) {
            sqe->flags |= IOSQE_FIXED_FILE;
          }
          io_uring_sqe_set_data64(
              sqe,
              MakeUserData(kOpPollWrite, fd));
          peer->poll_write_pending = 1;
        }
      }
      return;
    }
    if (cqe->res == -ENOMEM) {
      // SEND_ZC can't pin pages — kernel socket memory
      // exhausted. Treat like EAGAIN: back off and wait
      // for POLLOUT. This preserves TCP backpressure
      // which prevents the recv path from outrunning
      // the send path.
      peer->send_inflight--;
      SendItem* p = peer->send_head;
      for (int i = 0;
           i < peer->send_inflight && p; i++) {
        p = p->next;
      }
      peer->send_next = p;
      if (!peer->poll_write_pending) {
        struct io_uring_sqe* sqe = GetSqe(w);
        if (sqe) {
          io_uring_prep_poll_add(sqe, fd, POLLOUT);
          if (w->use_fixed_files) {
            sqe->flags |= IOSQE_FIXED_FILE;
          }
          io_uring_sqe_set_data64(
              sqe,
              MakeUserData(kOpPollWrite, fd));
          peer->poll_write_pending = 1;
        }
      }
      return;
    }
    if (cqe->res == -EPIPE) {
      w->stats.send_epipe++;
      NotifyPeerClose(w, peer);
    } else if (cqe->res == -ECONNRESET ||
               cqe->res == -ENOTCONN) {
      w->stats.send_econnreset++;
      NotifyPeerClose(w, peer);
    } else {
      w->stats.send_other_err++;
    }
    w->stats.send_drops++;
    DrainSendQueue(w, peer, defer_notif);
    return;
  }

  w->stats.send_bytes += cqe->res;

  // Short write handling.
  if (peer->send_head &&
      cqe->res < peer->send_head->len) {
    int sent = cqe->res;
    SendItem* item = peer->send_head;
    int remaining = item->len - sent;
    uint8_t* newbuf = FrameAlloc(w, remaining);
    if (!newbuf) {
      DrainSendQueue(w, peer, defer_notif);
      return;
    }
    memcpy(newbuf, item->data + sent, remaining);
    peer->send_head = item->next;
    if (!peer->send_head) {
      peer->send_tail = nullptr;
    }
    if (defer_notif) {
      item->next = w->notif_map[fd];
      w->notif_map[fd] = item;
    } else {
      FrameFree(w, item->data);
      SlabFreeItem(w, item);
    }
    SendItem* ni = SlabAllocItem(w);
    if (!ni) {
      w->stats.slab_exhausts++;
      FrameFree(w, newbuf);
      peer->send_inflight--;
      if (peer->send_next &&
          peer->send_inflight < kMaxSendsInflight) {
        int more =
            (peer->send_next->next != nullptr);
        if (SubmitPeerSend(w, peer,
                           peer->send_next->data,
                           peer->send_next->len,
                           more)) {
          peer->send_next = peer->send_next->next;
          peer->send_inflight++;
        }
      }
      return;
    }
    ni->data = newbuf;
    ni->len = remaining;
    ni->next = peer->send_head;
    peer->send_head = ni;
    if (!peer->send_tail) {
      peer->send_tail = ni;
    }
    if (!SubmitSend(w, fd, newbuf, remaining, 0)) {
      // SQ full — count as a completed send so
      // DrainSendQueue can re-submit later.
      peer->send_inflight--;
    }
    return;
  }

  DrainSendQueue(w, peer, defer_notif);
}

// -- POLLOUT handler ---------------------------------------------------------

static void HandlePollWrite(Worker* w,
                            struct io_uring_cqe* cqe) {
  int fd = UdFd(cqe->user_data);
  if (fd < 0 || fd >= kMaxFd || !w->fd_map[fd]) {
    return;
  }
  Peer* peer = w->fd_map[fd];
  peer->poll_write_pending = 0;
  int submitted = 0;
  while (peer->send_next &&
         peer->send_inflight < kMaxSendsInflight &&
         submitted < kPollWriteBatch) {
    int more = (peer->send_next->next != nullptr);
    if (SubmitPeerSend(w, peer, peer->send_next->data,
                       peer->send_next->len, more)) {
      peer->send_next = peer->send_next->next;
      peer->send_inflight++;
      submitted++;
    } else {
      break;
    }
  }
  // Re-arm POLLOUT if more sends remain.
  if (peer->send_next &&
      peer->send_inflight < kMaxSendsInflight &&
      !peer->poll_write_pending) {
    struct io_uring_sqe* sqe = GetSqe(w);
    if (sqe) {
      io_uring_prep_poll_add(sqe, fd, POLLOUT);
      if (w->use_fixed_files) {
        sqe->flags |= IOSQE_FIXED_FILE;
      }
      io_uring_sqe_set_data64(
          sqe, MakeUserData(kOpPollWrite, fd));
      peer->poll_write_pending = 1;
    }
  }
}

// -- Command processing ------------------------------------------------------

static void ProcessCmdAdd(Worker* w, Cmd* cmd) {
  if (cmd->fd < 0 || cmd->fd >= kMaxFd) {
    return;
  }
  HtInsert(w, cmd->fd, cmd->key);
  Peer* p = HtLookup(w, cmd->key.data());
  if (!p) {
    return;
  }
  p->protocol = cmd->protocol;
  p->peer_id = cmd->peer_id;
  w->fd_map[cmd->fd] = p;
  w->fd_gen[cmd->fd]++;
  w->peer_count++;

  // Register in peer ID lookup table for MeshData routing.
  if (cmd->protocol == PeerProtocol::kHd &&
      cmd->peer_id > 0) {
    if (!w->peer_id_map) {
      w->peer_id_map = static_cast<Peer**>(
          calloc(65536, sizeof(Peer*)));
    }
    if (w->peer_id_map) {
      w->peer_id_map[cmd->peer_id] = p;
    }
  }

  BroadcastRouteAdd(w->ctx, cmd->key, w->id, cmd->fd);

  SetNonblock(cmd->fd);

  // Enlarge socket buffers to absorb bursts.
  int bufsize = w->ctx->sockbuf_size;
  if (bufsize > 0) {
    setsockopt(cmd->fd, SOL_SOCKET, SO_SNDBUF,
               &bufsize, sizeof(bufsize));
    setsockopt(cmd->fd, SOL_SOCKET, SO_RCVBUF,
               &bufsize, sizeof(bufsize));
  }

  // Detect AF_UNIX (ZC sends not supported).
  {
    struct sockaddr_storage sa;
    socklen_t sl = sizeof(sa);
    if (getsockname(cmd->fd,
                    reinterpret_cast<struct sockaddr*>(&sa),
                    &sl) == 0 &&
        sa.ss_family == AF_UNIX) {
      p->no_zc = 1;
    }
  }

  if (w->use_fixed_files) {
    int fds[1] = {cmd->fd};
    io_uring_register_files_update(
        &w->ring, cmd->fd, fds, 1);
  }

  SubmitRecv(w, cmd->fd);
}

static void ProcessCmdRemove(Worker* w, Cmd* cmd) {
  Peer* p = HtLookup(w, cmd->key.data());
  if (!p) {
    return;
  }
  // Clear peer ID map entry before removal.
  if (p->peer_id > 0 && w->peer_id_map) {
    w->peer_id_map[p->peer_id] = nullptr;
  }
  int fd = p->fd;
  if (fd >= 0 && fd < kMaxFd) {
    w->fd_map[fd] = nullptr;
    if (w->use_fixed_files) {
      int fds[1] = {-1};
      io_uring_register_files_update(
          &w->ring, fd, fds, 1);
    }
    close(fd);
  }
  HtRemove(w, cmd->key);
  BroadcastRouteRemove(w->ctx, cmd->key);
}

static void ProcessCmdWrite(Worker* w, Cmd* cmd) {
  Peer* p = HtLookup(w, cmd->key.data());
  if (p) {
    // Build a DERP frame wrapping the data.
    int frame_len = kFrameHeaderSize + cmd->data_len;
    uint8_t* frame = FrameAlloc(w, frame_len);
    if (frame) {
      // Control plane writes are opaque; use the raw
      // data as the frame payload. The caller is
      // responsible for including the frame type byte
      // and correct framing.
      memcpy(frame, cmd->data, cmd->data_len);
      EnqueueSend(w, p, frame, cmd->data_len);
    }
  }
  free(cmd->data);
}

static void ProcessCommands(Worker* w) {
  DrainEventfd(w->event_fd);
  Cmd cmd{};
  while (RingPop(&w->cmd_ring, &cmd) == 0) {
    switch (cmd.type) {
      case kCmdAddPeer:
        ProcessCmdAdd(w, &cmd);
        break;
      case kCmdRemovePeer:
        ProcessCmdRemove(w, &cmd);
        break;
      case kCmdWrite:
        ProcessCmdWrite(w, &cmd);
        break;
      case kCmdSetFwdRule: {
        Peer* p = HtLookup(w, cmd.key.data());
        if (!p || p->fwd_count >= Peer::kMaxPeerRules) {
          break;
        }
        int idx = p->fwd_count;
        p->fwd_keys[idx] = cmd.dst_key;

        // Check if destination is on this worker.
        Peer* dst = HtLookup(w, cmd.dst_key.data());
        p->fwd_peers[idx] = dst;
        if (!dst) {
          // Cross-shard: try to cache route now.
          // If route isn't broadcast yet, leave fd=-1;
          // the forwarding path does lazy lookup.
          Route* rt = RouteLookup(w->routes,
              cmd.dst_key.data());
          if (rt) {
            p->fwd_dst_worker[idx] = rt->worker_id;
            p->fwd_dst_fd[idx] = rt->fd;
          }
        }
        p->fwd_count++;
        break;
      }
      case kCmdMovePeerOut: {
        // Remove peer from this worker without closing
        // the fd. Used for same-shard migration.
        Peer* p = HtLookup(w, cmd.key.data());
        if (p) {
          // Clear peer ID map entry before removal.
          if (p->peer_id > 0 && w->peer_id_map) {
            w->peer_id_map[p->peer_id] = nullptr;
          }
          int fd = p->fd;
          if (fd >= 0 && fd < kMaxFd) {
            w->fd_map[fd] = nullptr;
          }
          HtRemove(w, cmd.key);
          // Don't broadcast route remove — the re-add
          // on the new worker will update the route.
          if (w->peer_count > 0) w->peer_count--;
        }
        break;
      }
      case kCmdStop:
        w->running = 0;
        break;
    }
  }
}

// -- Cross-shard inbox processing --------------------------------------------

/// Drain cross-shard inboxes using the pending_sources
/// bitmask. Only checks inboxes that senders marked as
/// having data — O(active sources) instead of O(N-1).
static void ProcessXfer(Worker* w) {
  uint32_t mask = __atomic_exchange_n(
      &w->pending_sources, 0, __ATOMIC_ACQUIRE);
  if (!mask) return;
  Xfer x{};
  while (mask) {
    int src = __builtin_ctz(mask);
    mask &= mask - 1;
    XferSpscRing* ring = w->xfer_inbox[src];
    while (XferSpscPop(ring, &x) == 0) {
      if (x.dst_fd < 0 || x.dst_fd >= kMaxFd ||
          !w->fd_map[x.dst_fd]) {
        FrameFree(w, x.frame);
        continue;
      }
      Peer* peer = w->fd_map[x.dst_fd];
      if (x.dst_gen != w->fd_gen[x.dst_fd]) {
        FrameFree(w, x.frame);
        continue;
      }
      EnqueueSend(w, peer, x.frame, x.frame_len);
    }
  }
}

/// Flush batched cross-shard eventfd signals. Called once
/// per CQE batch after all ForwardMsg calls are done.
/// One write(eventfd) per destination worker per batch,
/// instead of one per frame.
static void FlushXferSignals(Worker* w) {
  uint32_t pending = w->xfer_signal_pending;
  if (!pending) {
    return;
  }
  w->xfer_signal_pending = 0;
  while (pending) {
    int dst = __builtin_ctz(pending);
    SignalEventfd(w->ctx->workers[dst]->xfer_efd);
    pending &= pending - 1;
  }
}

// -- Worker thread entry point -----------------------------------------------

static void* WorkerRun(void* arg) {
  auto* w = static_cast<Worker*>(arg);

  // Initialize io_uring ring on the worker thread so that
  // SINGLE_ISSUER + DEFER_TASKRUN record this thread as
  // the submitter.
  if (WorkerInitRing(w) < 0) {
    spdlog::error("worker {} ring init failed", w->id);
    __atomic_store_n(&w->ring_exited, 1,
                     __ATOMIC_RELEASE);
    return nullptr;
  }

  // Pin to core if configured.
  int pin = w->ctx->pin_cores[w->id];
  if (pin >= 0) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(pin, &cpuset);
    if (sched_setaffinity(0, sizeof(cpuset),
                          &cpuset) == 0) {
      spdlog::info("worker {} pinned to core {}", w->id,
                   pin);
    } else {
      spdlog::warn("worker {} pin to core {} failed: {}",
                   w->id, pin, strerror(errno));
    }
  }

  SubmitPollMultishot(w, w->event_fd, kOpPollCmd);
  SubmitPollMultishot(w, w->xfer_efd, kOpPollXfer);
  io_uring_submit(&w->ring);

  struct __kernel_timespec ts {};
  ts.tv_sec = 0;
  ts.tv_nsec = 1000000;  // 1ms

  int idle_spins = 0;

  while (__atomic_load_n(&w->running, __ATOMIC_ACQUIRE)) {
    // Peek for ready CQEs without entering the kernel.
    struct io_uring_cqe* cqe = nullptr;
    int ret = io_uring_peek_cqe(&w->ring, &cqe);

    if (ret == -EAGAIN) {
      // No CQEs. Submit pending SQEs — on kernel 6.1+
      // with DEFER_TASKRUN, io_uring_enter processes
      // deferred task work on entry, making new CQEs
      // visible for the next peek.
      if (io_uring_sq_ready(&w->ring) > 0) {
        io_uring_submit(&w->ring);
      }
      if (idle_spins++ < w->busy_spins) {
        continue;
      }
      // Idle too long — block until an event arrives.
      ret = io_uring_wait_cqe_timeout(
          &w->ring, &cqe, &ts);
      if (ret == -ETIME || ret == -EINTR) {
        idle_spins = 0;
        continue;
      }
      if (ret < 0) {
        break;
      }
    }

    // Process available CQEs, capped to prevent
    // recv burst avalanches from generating unbounded SQEs.
    idle_spins = 0;
    {
      unsigned head;
      struct io_uring_cqe* c = nullptr;
      int nr = 0;
      io_uring_for_each_cqe(&w->ring, head, c) {
        if (nr >= kMaxCqeBatch) break;
        // Direct send completion: bit 63 marks a
        // cross-shard send with an embedded buffer ptr.
        if (c->user_data & (1ULL << 63)) {
          auto* buf = reinterpret_cast<uint8_t*>(
              c->user_data & ~(1ULL << 63));
          FrameFree(w, buf);
          nr++;
          continue;
        }
        uint8_t op = UdOp(c->user_data);
        switch (op) {
          case kOpRecv:
            HandleRecvCqe(w, c);
            break;
          case kOpSend:
            HandleSendCqe(w, c);
            break;
          case kOpPollCmd:
            ProcessCommands(w);
            if (!(c->flags & IORING_CQE_F_MORE)) {
              SubmitPollMultishot(
                  w, w->event_fd, kOpPollCmd);
            }
            break;
          case kOpPollXfer:
            DrainEventfd(w->xfer_efd);
            ProcessXfer(w);
            if (!(c->flags & IORING_CQE_F_MORE)) {
              SubmitPollMultishot(
                  w, w->xfer_efd, kOpPollXfer);
            }
            break;
          case kOpPollWrite:
            HandlePollWrite(w, c);
            break;
        }
        nr++;
      }
      io_uring_cq_advance(&w->ring, nr);
    }

    // Flush batched cross-shard signals (one eventfd
    // write per destination, not one per frame).
    FlushXferSignals(w);

    // Flush deferred first-sends with coalesced MSG_MORE.
    FlushPendingSends(w);

    // Drain cross-shard transfers and deferred recvs,
    // then submit all batched SQEs in one syscall.
    ProcessXfer(w);
    FlushPendingSends(w);
    DrainDeferredRecvs(w);
    FrameReturnDrain(w);
    io_uring_submit(&w->ring);
  }

  // Drain in-flight sends and ZC notifications.
  {
    struct __kernel_timespec drain_ts {};
    drain_ts.tv_sec = 0;
    drain_ts.tv_nsec = 10000000;  // 10ms
    for (int iter = 0; iter < 50; iter++) {
      io_uring_submit(&w->ring);
      struct io_uring_cqe* dcqe = nullptr;
      int ret = io_uring_wait_cqe_timeout(
          &w->ring, &dcqe, &drain_ts);
      if (ret == -ETIME) {
        int pending = 0;
        for (int i = 0; i < kMaxFd; i++) {
          if (w->notif_map[i]) {
            pending++;
          }
        }
        if (pending == 0) {
          break;
        }
        continue;
      }
      if (ret < 0) {
        break;
      }
      unsigned dhead;
      int dnr = 0;
      io_uring_for_each_cqe(&w->ring, dhead, dcqe) {
        dnr++;
        if (dcqe->user_data & (1ULL << 63)) {
          auto* buf = reinterpret_cast<uint8_t*>(
              dcqe->user_data & ~(1ULL << 63));
          FrameFree(w, buf);
          continue;
        }
        uint8_t dop = UdOp(dcqe->user_data);
        if (dop == kOpSend) {
          HandleSendCqe(w, dcqe);
        } else if (dop == kOpPollWrite) {
          HandlePollWrite(w, dcqe);
        }
      }
      io_uring_cq_advance(&w->ring, dnr);
    }
  }

  io_uring_queue_exit(&w->ring);
  __atomic_store_n(&w->ring_exited, 1,
                   __ATOMIC_RELEASE);

  return nullptr;
}

// -- Init / run / destroy ----------------------------------------------------

static int WorkerInit(Worker* w, int id, Ctx* ctx) {
  memset(w, 0, sizeof(*w));
  w->id = id;
  w->ctx = ctx;
  w->running = 1;
  w->busy_spins = ctx->num_workers <= 2
                      ? kBusySpinLowWorker
                      : kBusySpinDefault;
  w->pipe_wr = -1;
  w->event_fd = -1;
  w->xfer_efd = -1;

  int pipefd[2];
  if (pipe(pipefd) < 0) {
    return -1;
  }
  ctx->pipe_rds[id] = pipefd[0];
  w->pipe_wr = pipefd[1];

  w->event_fd =
      eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (w->event_fd < 0) {
    goto fail;
  }

  w->xfer_efd =
      eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (w->xfer_efd < 0) {
    goto fail;
  }

  RingInit(&w->cmd_ring);

  // Allocate per-source SPSC xfer inbox rings.
  for (int i = 0; i < kMaxWorkers; i++) {
    w->xfer_inbox[i] = static_cast<XferSpscRing*>(
        calloc(1, sizeof(XferSpscRing)));
    if (!w->xfer_inbox[i]) {
      goto fail;
    }
    XferSpscInit(w->xfer_inbox[i]);
  }

  if (SlabInit(w) < 0) {
    goto fail;
  }

  if (FramePoolInit(w) < 0) {
    SlabDestroy(w);
    goto fail;
  }

  return 0;

fail:
  for (int i = 0; i < kMaxWorkers; i++) {
    free(w->xfer_inbox[i]);
    w->xfer_inbox[i] = nullptr;
  }
  if (w->xfer_efd >= 0) close(w->xfer_efd);
  if (w->event_fd >= 0) close(w->event_fd);
  if (w->pipe_wr >= 0) close(w->pipe_wr);
  if (ctx->pipe_rds[id] >= 0) {
    close(ctx->pipe_rds[id]);
    ctx->pipe_rds[id] = -1;
  }
  w->pipe_wr = -1;
  w->event_fd = -1;
  w->xfer_efd = -1;
  return -1;
}

/// Initialize the io_uring ring, file table, and provided
/// buffer ring. Must be called from the worker thread so
/// that SINGLE_ISSUER + DEFER_TASKRUN record the correct
/// submitter task.
static int WorkerInitRing(Worker* w) {
  struct io_uring_params params {};
  int ret = -1;
  w->use_sqpoll = 0;

  // Try SQPOLL mode if requested. SQPOLL and DEFER_TASKRUN
  // are mutually exclusive (kernel rejects the combo).
  if (w->ctx->sqpoll) {
    params.flags = IORING_SETUP_SQPOLL |
                   IORING_SETUP_COOP_TASKRUN |
                   IORING_SETUP_SINGLE_ISSUER;
    params.sq_thread_idle = 2000;
    int pin = w->ctx->pin_cores[w->id];
    if (pin >= 0) {
      params.flags |= IORING_SETUP_SQ_AFF;
      params.sq_thread_cpu =
          static_cast<unsigned>(pin);
    }
    ret = io_uring_queue_init_params(
        kUringQueueDepth, &w->ring, &params);
    if (ret == 0) {
      w->use_sqpoll = 1;
      spdlog::info("worker {} using SQPOLL", w->id);
    } else {
      spdlog::warn(
          "worker {} SQPOLL failed ({}), falling back",
          w->id, strerror(-ret));
    }
  }

  // Standard init chain (also used as SQPOLL fallback).
  if (ret < 0) {
    memset(&params, 0, sizeof(params));
    params.flags = IORING_SETUP_SINGLE_ISSUER |
                   IORING_SETUP_DEFER_TASKRUN;
    ret = io_uring_queue_init_params(
        kUringQueueDepth, &w->ring, &params);
    if (ret == 0) {
      spdlog::info(
          "worker {} using DEFER_TASKRUN", w->id);
    }
  }
  if (ret < 0) {
    // Fall back to COOP_TASKRUN (kernel < 6.1).
    memset(&params, 0, sizeof(params));
    params.flags = IORING_SETUP_COOP_TASKRUN;
    ret = io_uring_queue_init_params(
        kUringQueueDepth, &w->ring, &params);
    if (ret == 0) {
      spdlog::info(
          "worker {} using COOP_TASKRUN", w->id);
    }
  }
  if (ret < 0) {
    // Fall back to no flags.
    memset(&params, 0, sizeof(params));
    ret = io_uring_queue_init_params(
        kUringQueueDepth, &w->ring, &params);
    if (ret == 0) {
      spdlog::info(
          "worker {} using basic io_uring", w->id);
    }
  }
  if (ret < 0) {
    return -1;
  }

  // Register sparse file table.
  w->use_fixed_files = 0;
  {
    int* ftable = static_cast<int*>(
        malloc(kMaxFd * sizeof(int)));
    if (ftable) {
      memset(ftable, -1, kMaxFd * sizeof(int));
      ftable[w->event_fd] = w->event_fd;
      ftable[w->xfer_efd] = w->xfer_efd;
      if (io_uring_register_files(
              &w->ring, ftable, kMaxFd) == 0) {
        w->use_fixed_files = 1;
      }
      free(ftable);
    }
  }

  // Register provided buffer ring.
  w->use_provided_bufs = 0;
  w->provided_bufs = static_cast<uint8_t*>(
      aligned_alloc(
          4096,
          static_cast<size_t>(kPbufCount) * kPbufSize));
  if (w->provided_bufs) {
    // Hint THP for provided buffer ring.
    madvise(w->provided_bufs,
            static_cast<size_t>(kPbufCount) * kPbufSize,
            MADV_HUGEPAGE);
    int br_ret;
    w->buf_ring = io_uring_setup_buf_ring(
        &w->ring, kPbufCount, w->id, 0, &br_ret);
    if (w->buf_ring) {
      int mask = io_uring_buf_ring_mask(kPbufCount);
      for (int i = 0; i < kPbufCount; i++) {
        io_uring_buf_ring_add(
            w->buf_ring,
            w->provided_bufs +
                static_cast<size_t>(i) * kPbufSize,
            kPbufSize, i, mask, i);
      }
      io_uring_buf_ring_advance(
          w->buf_ring, kPbufCount);
      w->use_provided_bufs = 1;
    } else {
      free(w->provided_bufs);
      w->provided_bufs = nullptr;
    }
  }

  // Enable multishot recv when provided buffers are active.
  w->use_multishot_recv = w->use_provided_bufs;

  return 0;
}

static void WorkerDestroy(Worker* w, Ctx* ctx) {
  for (int i = 0; i < kHtCapacity; i++) {
    Peer* p = &w->ht[i];
    if (p->occupied == 1) {
      SendItem* item = p->send_head;
      while (item) {
        SendItem* next = item->next;
        FrameFree(w, item->data);
        SlabFreeItem(w, item);
        item = next;
      }
      p->send_head = nullptr;
      p->send_tail = nullptr;
    }
    // Free heap-allocated rbuf regardless of state.
    free(p->rbuf);
    p->rbuf = nullptr;
  }

  for (int i = 0; i < kMaxFd; i++) {
    while (w->notif_map[i]) {
      SendItem* item = w->notif_map[i];
      w->notif_map[i] = item->next;
      FrameFree(w, item->data);
      SlabFreeItem(w, item);
    }
  }

  if (!__atomic_load_n(
          &w->ring_exited, __ATOMIC_ACQUIRE)) {
    io_uring_queue_exit(&w->ring);
  }

  if (w->provided_bufs) {
    free(w->provided_bufs);
    w->provided_bufs = nullptr;
    w->buf_ring = nullptr;
  }

  if (w->pipe_wr >= 0) {
    close(w->pipe_wr);
    w->pipe_wr = -1;
  }
  if (ctx->pipe_rds[w->id] >= 0) {
    close(ctx->pipe_rds[w->id]);
    ctx->pipe_rds[w->id] = -1;
  }
  if (w->event_fd >= 0) {
    close(w->event_fd);
    w->event_fd = -1;
  }
  if (w->xfer_efd >= 0) {
    close(w->xfer_efd);
    w->xfer_efd = -1;
  }

  Cmd cmd{};
  while (RingPop(&w->cmd_ring, &cmd) == 0) {
    if (cmd.type == kCmdWrite) {
      free(cmd.data);
    }
  }

  // Drain and free per-source xfer inbox rings.
  for (int i = 0; i < kMaxWorkers; i++) {
    if (w->xfer_inbox[i]) {
      Xfer x{};
      while (XferSpscPop(w->xfer_inbox[i], &x) == 0) {
        FrameFree(w, x.frame);
      }
      free(w->xfer_inbox[i]);
      w->xfer_inbox[i] = nullptr;
    }
  }

  free(w->peer_id_map);
  w->peer_id_map = nullptr;

  FramePoolDestroy(w);
  SlabDestroy(w);
}

int DpInit(Ctx* ctx, int num_workers) {
  memset(ctx, 0, sizeof(*ctx));

  if (num_workers < 1) {
    num_workers = 1;
  }
  if (num_workers > kMaxWorkers) {
    num_workers = kMaxWorkers;
  }
  ctx->num_workers = num_workers;
  for (int i = 0; i < kMaxWorkers; i++) {
    ctx->pin_cores[i] = -1;
  }

  for (int i = 0; i < num_workers; i++) {
    auto* w = static_cast<Worker*>(
        calloc(1, sizeof(Worker)));
    if (!w) {
      goto fail;
    }
    ctx->workers[i] = w;
    if (WorkerInit(w, i, ctx) < 0) {
      free(w);
      ctx->workers[i] = nullptr;
      goto fail;
    }
  }
  return 0;

fail:
  for (int i = 0; i < num_workers; i++) {
    if (ctx->workers[i]) {
      WorkerDestroy(ctx->workers[i], ctx);
      free(ctx->workers[i]);
      ctx->workers[i] = nullptr;
    }
  }
  return -1;
}

int DpRun(Ctx* ctx) {
  for (int i = 0; i < ctx->num_workers; i++) {
    int rc = pthread_create(
        &ctx->workers[i]->thread, nullptr,
        WorkerRun, ctx->workers[i]);
    if (rc != 0) {
      for (int j = 0; j < i; j++) {
        ctx->workers[j]->running = 0;
        SignalEventfd(ctx->workers[j]->event_fd);
        pthread_join(ctx->workers[j]->thread, nullptr);
      }
      return -1;
    }
  }

  for (int i = 0; i < ctx->num_workers; i++) {
    pthread_join(ctx->workers[i]->thread, nullptr);
  }
  return 0;
}

void DpDestroy(Ctx* ctx) {
  for (int i = 0; i < ctx->num_workers; i++) {
    if (ctx->workers[i]) {
      WorkerDestroy(ctx->workers[i], ctx);
      free(ctx->workers[i]);
      ctx->workers[i] = nullptr;
    }
  }
}

void DpGetStats(Ctx* ctx,
                uint64_t* send_drops,
                uint64_t* xfer_drops,
                uint64_t* slab_exhausts,
                uint64_t* recv_bytes,
                uint64_t* send_bytes) {
  uint64_t sd = 0, xd = 0, se = 0, rb = 0, sb = 0;
  for (int i = 0; i < ctx->num_workers; i++) {
    Worker* w = ctx->workers[i];
    if (!w) {
      continue;
    }
    sd += __atomic_load_n(&w->stats.send_drops,
                          __ATOMIC_RELAXED);
    xd += __atomic_load_n(&w->stats.xfer_drops,
                          __ATOMIC_RELAXED);
    se += __atomic_load_n(&w->stats.slab_exhausts,
                          __ATOMIC_RELAXED);
    rb += __atomic_load_n(&w->stats.recv_bytes,
                          __ATOMIC_RELAXED);
    sb += __atomic_load_n(&w->stats.send_bytes,
                          __ATOMIC_RELAXED);
  }
  *send_drops = sd;
  *xfer_drops = xd;
  *slab_exhausts = se;
  *recv_bytes = rb;
  *send_bytes = sb;
}

}  // namespace hyper_derp
