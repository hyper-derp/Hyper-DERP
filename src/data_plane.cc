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

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace hyper_derp {

// -- Internal forward declarations -------------------------------------------

static void SlabFreeItem(Worker* w, SendItem* item);
static inline void FrameFree(uint8_t* buf);

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
                      const uint8_t key[kKeySize]) {
  return static_cast<int>(
      Fnv1a(key, kKeySize) % num_workers);
}

// -- Per-worker hash table ---------------------------------------------------

static int HtFind(Worker* w,
                  const uint8_t key[kKeySize],
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
    if (memcmp(p->key, key, kKeySize) == 0) {
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
      memcmp(p->key, key, kKeySize) == 0) {
    return p;
  }
  return nullptr;
}

static int HtInsert(Worker* w, int fd,
                    const uint8_t key[kKeySize]) {
  Peer* p = nullptr;
  HtFind(w, key, &p);
  if (!p) {
    return -1;
  }
  p->fd = fd;
  memcpy(p->key, key, kKeySize);
  p->rbuf_len = 0;
  p->occupied = 1;
  p->send_head = nullptr;
  p->send_tail = nullptr;
  p->send_next = nullptr;
  p->send_inflight = 0;
  p->no_zc = 0;
  p->zc_draining = 0;
  p->poll_write_pending = 0;
  return 0;
}

static void HtRemove(Worker* w,
                     const uint8_t key[kKeySize]) {
  Peer* p = HtLookup(w, key);
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
    FrameFree(item->data);
    SlabFreeItem(w, item);
    item = next;
  }
  p->send_head = nullptr;
  p->send_tail = nullptr;
  p->send_next = nullptr;
  p->send_inflight = 0;
  p->occupied = 2;
}

// -- Replicated routing table ------------------------------------------------

static int RouteFind(Route* routes,
                     const uint8_t key[kKeySize],
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
    if (memcmp(r->key, key, kKeySize) == 0) {
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
      && memcmp(r->key, key, kKeySize) == 0) {
    return r;
  }
  return nullptr;
}

static void RouteSet(Route* routes,
                     const uint8_t key[kKeySize],
                     int worker_id, int fd) {
  Route* r = nullptr;
  RouteFind(routes, key, &r);
  if (!r) {
    return;
  }
  memcpy(r->key, key, kKeySize);
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
    Ctx* ctx, const uint8_t key[kKeySize],
    int worker_id, int fd) {
  for (int i = 0; i < ctx->num_workers; i++) {
    RouteSet(ctx->workers[i]->routes, key, worker_id, fd);
  }
}

static void BroadcastRouteRemove(
    Ctx* ctx, const uint8_t key[kKeySize]) {
  for (int i = 0; i < ctx->num_workers; i++) {
    RouteRemove(ctx->workers[i]->routes, key);
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

// -- Cross-shard transfer ring (MPSC) ----------------------------------------

static void XferRingInit(XferRing* r) {
  r->head = 0;
  r->tail = 0;
  r->lock = 0;
}

static void XferLock(XferRing* r) {
  while (__atomic_test_and_set(&r->lock, __ATOMIC_ACQUIRE)) {
  }
}

static void XferUnlock(XferRing* r) {
  __atomic_clear(&r->lock, __ATOMIC_RELEASE);
}

static int XferPush(XferRing* r, const Xfer* x) {
  XferLock(r);
  uint32_t cur_head = r->head;
  uint32_t cur_tail =
      __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
  uint32_t next = (cur_head + 1) % kXferRingSize;
  if (next == cur_tail) {
    XferUnlock(r);
    return -1;
  }
  int was_empty = (cur_head == cur_tail);
  r->items[cur_head] = *x;
  __atomic_store_n(&r->head, next, __ATOMIC_RELEASE);
  XferUnlock(r);
  return was_empty ? 1 : 0;
}

static int XferPop(XferRing* r, Xfer* x) {
  if (r->tail ==
      __atomic_load_n(&r->head, __ATOMIC_ACQUIRE)) {
    return -1;
  }
  *x = r->items[r->tail];
  __atomic_store_n(
      &r->tail, (r->tail + 1) % kXferRingSize,
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

static inline uint8_t* FrameAlloc(int size) {
  return static_cast<uint8_t*>(malloc(size));
}

static inline void FrameFree(uint8_t* buf) {
  free(buf);
}

// -- Enqueue helpers (called from control plane) -----------------------------

static void EnqueueCmd(Worker* w, const Cmd* cmd) {
  while (RingPush(&w->cmd_ring, cmd) != 0) {
  }
  SignalEventfd(w->event_fd);
}

void DpAddPeer(Ctx* ctx, int fd,
               const uint8_t key[kKeySize]) {
  int wid = PeerWorker(ctx->num_workers, key);
  Cmd cmd{};
  cmd.type = kCmdAddPeer;
  cmd.fd = fd;
  memcpy(cmd.key, key, kKeySize);
  EnqueueCmd(ctx->workers[wid], &cmd);
}

void DpRemovePeer(Ctx* ctx,
                  const uint8_t key[kKeySize]) {
  int wid = PeerWorker(ctx->num_workers, key);
  Cmd cmd{};
  cmd.type = kCmdRemovePeer;
  memcpy(cmd.key, key, kKeySize);
  EnqueueCmd(ctx->workers[wid], &cmd);
}

void DpWrite(Ctx* ctx, const uint8_t key[kKeySize],
             uint8_t* data, int data_len) {
  int wid = PeerWorker(ctx->num_workers, key);
  Cmd cmd{};
  cmd.type = kCmdWrite;
  memcpy(cmd.key, key, kKeySize);
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

// -- io_uring SQE helpers ----------------------------------------------------

static inline struct io_uring_sqe* GetSqe(Worker* w) {
  struct io_uring_sqe* sqe = io_uring_get_sqe(&w->ring);
  if (sqe) {
    return sqe;
  }
  io_uring_submit(&w->ring);
  return io_uring_get_sqe(&w->ring);
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
    return;
  }
  if (w->use_provided_bufs) {
    io_uring_prep_recv(sqe, fd, nullptr, 0, 0);
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
  if (w->recv_inflight >= kRecvBudget) {
    DeferRecv(w, fd);
    return;
  }
  SubmitRecvNow(w, fd);
}

static void DrainDeferredRecvs(Worker* w) {
  while (w->recv_defer_tail != w->recv_defer_head &&
         w->recv_inflight < kRecvBudget) {
    int fd = w->recv_defer_buf[w->recv_defer_tail];
    w->recv_defer_tail =
        (w->recv_defer_tail + 1) % kRecvDeferSize;
    if (fd >= 0 && fd < kMaxFd && w->fd_map[fd]) {
      SubmitRecvNow(w, fd);
    }
  }
}

static void SubmitSend(Worker* w, int fd,
                       const uint8_t* data, int len,
                       int extra_flags) {
  struct io_uring_sqe* sqe = GetSqe(w);
  if (!sqe) {
    return;
  }
  io_uring_prep_send(sqe, fd, data, len,
                     MSG_NOSIGNAL | extra_flags);
  if (w->use_fixed_files) {
    sqe->flags |= IOSQE_FIXED_FILE;
  }
  io_uring_sqe_set_data64(
      sqe, MakeUserData(kOpSend, fd));
}

static void SubmitSendZc(Worker* w, int fd,
                         const uint8_t* data, int len,
                         int extra_flags) {
  struct io_uring_sqe* sqe = GetSqe(w);
  if (!sqe) {
    return;
  }
  io_uring_prep_send_zc(sqe, fd, data, len,
                        MSG_NOSIGNAL | extra_flags, 0);
  if (w->use_fixed_files) {
    sqe->flags |= IOSQE_FIXED_FILE;
  }
  io_uring_sqe_set_data64(
      sqe, MakeUserData(kOpSend, fd));
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

static inline void SubmitPeerSend(Worker* w, Peer* peer,
                                  const uint8_t* data,
                                  int len, int more) {
  int flags = more ? MSG_MORE : 0;
  if (peer->no_zc) {
    SubmitSend(w, peer->fd, data, len, flags);
  } else {
    SubmitSendZc(w, peer->fd, data, len, flags);
  }
}

// -- Send queue management ---------------------------------------------------

static void EnqueueSend(Worker* w, Peer* peer,
                        uint8_t* data, int len) {
  SendItem* item = SlabAllocItem(w);
  if (!item) {
    w->stats.slab_exhausts++;
    FrameFree(data);
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

  if (!peer->send_next) {
    peer->send_next = item;
  }

  if (peer->send_inflight < kMaxSendsInflight &&
      peer->send_next && !peer->zc_draining) {
    int more = (peer->send_next->next != nullptr);
    SubmitPeerSend(w, peer, peer->send_next->data,
                   peer->send_next->len, more);
    peer->send_next = peer->send_next->next;
    peer->send_inflight++;
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
      FrameFree(done->data);
      SlabFreeItem(w, done);
    }
  }

  peer->send_inflight--;

  if (peer->send_next &&
      peer->send_inflight < kMaxSendsInflight) {
    int more = (peer->send_next->next != nullptr);
    SubmitPeerSend(w, peer, peer->send_next->data,
                   peer->send_next->len, more);
    peer->send_next = peer->send_next->next;
    peer->send_inflight++;
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
    uint8_t tag = static_cast<uint8_t>(type);
    (void)WriteAllBlocking(w->pipe_wr, &tag, 1);
    uint8_t len_buf[4];
    len_buf[0] = static_cast<uint8_t>(payload_len >> 24);
    len_buf[1] = static_cast<uint8_t>(payload_len >> 16);
    len_buf[2] = static_cast<uint8_t>(payload_len >> 8);
    len_buf[3] = static_cast<uint8_t>(payload_len);
    // Prepend the fd for demux on the control side.
    uint8_t fd_buf[4];
    fd_buf[0] = static_cast<uint8_t>(src->fd >> 24);
    fd_buf[1] = static_cast<uint8_t>(src->fd >> 16);
    fd_buf[2] = static_cast<uint8_t>(src->fd >> 8);
    fd_buf[3] = static_cast<uint8_t>(src->fd);
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
  uint8_t* frame = FrameAlloc(frame_len);
  if (!frame) {
    return;
  }
  BuildRecvPacket(frame, src->key, pkt_data, pkt_len);

  // Same-shard fast path.
  Peer* dst = HtLookup(w, dst_key);
  if (dst) {
    EnqueueSend(w, dst, frame, frame_len);
    return;
  }

  // Cross-shard: lookup in replicated routing table.
  Route* route = RouteLookup(w->routes, dst_key);
  if (!route) {
    FrameFree(frame);
    return;
  }

  Xfer x{};
  x.dst_fd = route->fd;
  x.frame = frame;
  x.frame_len = frame_len;
  Worker* dst_w = w->ctx->workers[route->worker_id];
  x.dst_gen = __atomic_load_n(
      &dst_w->fd_gen[route->fd], __ATOMIC_ACQUIRE);
  int rc = XferPush(&dst_w->xfer_ring, &x);
  if (rc == 1) {
    SignalEventfd(dst_w->xfer_efd);
  } else if (rc == -1) {
    FrameFree(frame);
    w->stats.xfer_drops++;
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
                               struct io_uring_cqe* cqe) {
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
      }
      goto return_buf;
    }

    int n = cqe->res;
    w->stats.recv_bytes += n;
    int off = 0;

    // Complete any partial frame in rbuf.
    if (peer->rbuf_len > 0) {
      while (peer->rbuf_len < kFrameHeaderSize &&
             off < n) {
        peer->rbuf[peer->rbuf_len++] = buf[off++];
      }
      if (peer->rbuf_len < kFrameHeaderSize) {
        goto return_buf;
      }

      uint32_t payload_len =
          ReadPayloadLen(peer->rbuf);
      if (!IsValidPayloadLen(payload_len)) {
        NotifyPeerClose(w, peer);
        peer->rbuf_len = 0;
        goto return_buf;
      }

      int frame_len = kFrameHeaderSize +
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
      ForwardMsg(w, ReadFrameType(peer->rbuf),
                 peer->rbuf + kFrameHeaderSize,
                 static_cast<int>(payload_len), peer);
      peer->rbuf_len = 0;
    }

    // Process complete frames from provided buffer.
    while (off + kFrameHeaderSize <= n) {
      if (!__atomic_load_n(
              &w->running, __ATOMIC_ACQUIRE)) {
        goto return_buf;
      }
      uint32_t payload_len =
          ReadPayloadLen(buf + off);
      if (!IsValidPayloadLen(payload_len)) {
        NotifyPeerClose(w, peer);
        goto return_buf;
      }
      int frame_len = kFrameHeaderSize +
                      static_cast<int>(payload_len);
      if (off + frame_len > n) {
        break;
      }
      ForwardMsg(w, ReadFrameType(buf + off),
                 buf + off + kFrameHeaderSize,
                 static_cast<int>(payload_len), peer);
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

  if (fd >= 0 && fd < kMaxFd && w->fd_map[fd]) {
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
    }
    return;
  }

  int n = cqe->res;
  w->stats.recv_bytes += n;
  peer->rbuf_len += n;

  while (peer->rbuf_len >= kFrameHeaderSize) {
    if (!__atomic_load_n(
            &w->running, __ATOMIC_ACQUIRE)) {
      return;
    }
    uint32_t payload_len =
        ReadPayloadLen(peer->rbuf);
    if (!IsValidPayloadLen(payload_len)) {
      NotifyPeerClose(w, peer);
      return;
    }
    int frame_len = kFrameHeaderSize +
                    static_cast<int>(payload_len);
    if (peer->rbuf_len < frame_len) {
      break;
    }

    ForwardMsg(w, ReadFrameType(peer->rbuf),
               peer->rbuf + kFrameHeaderSize,
               static_cast<int>(payload_len), peer);

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
  w->recv_inflight--;
  if (w->recv_inflight < 0) {
    w->recv_inflight = 0;
  }

  if (cqe->flags & IORING_CQE_F_BUFFER) {
    HandleRecvProvided(w, cqe);
    return;
  }

  if (w->use_provided_bufs) {
    if (cqe->res == -ENOBUFS) {
      int fd = UdFd(cqe->user_data);
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
      FrameFree(item->data);
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
        FrameFree(item->data);
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
          SubmitPeerSend(w, peer,
                         peer->send_next->data,
                         peer->send_next->len, more);
          peer->send_next = peer->send_next->next;
          peer->send_inflight++;
        }
      }
      return;
    }
    if (cqe->res == -EAGAIN) {
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
    if (cqe->res == -EPIPE ||
        cqe->res == -ECONNRESET ||
        cqe->res == -ENOTCONN) {
      NotifyPeerClose(w, peer);
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
    uint8_t* newbuf = FrameAlloc(remaining);
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
      FrameFree(item->data);
      SlabFreeItem(w, item);
    }
    SendItem* ni = SlabAllocItem(w);
    if (!ni) {
      w->stats.slab_exhausts++;
      FrameFree(newbuf);
      peer->send_inflight--;
      if (peer->send_next &&
          peer->send_inflight < kMaxSendsInflight) {
        int more =
            (peer->send_next->next != nullptr);
        SubmitPeerSend(w, peer,
                       peer->send_next->data,
                       peer->send_next->len, more);
        peer->send_next = peer->send_next->next;
        peer->send_inflight++;
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
    SubmitSend(w, fd, newbuf, remaining, 0);
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
  while (peer->send_next &&
         peer->send_inflight < kMaxSendsInflight) {
    int more = (peer->send_next->next != nullptr);
    SubmitPeerSend(w, peer, peer->send_next->data,
                   peer->send_next->len, more);
    peer->send_next = peer->send_next->next;
    peer->send_inflight++;
  }
}

// -- Command processing ------------------------------------------------------

static void ProcessCmdAdd(Worker* w, Cmd* cmd) {
  if (cmd->fd < 0 || cmd->fd >= kMaxFd) {
    return;
  }
  HtInsert(w, cmd->fd, cmd->key);
  Peer* p = HtLookup(w, cmd->key);
  if (!p) {
    return;
  }
  w->fd_map[cmd->fd] = p;
  w->fd_gen[cmd->fd]++;

  BroadcastRouteAdd(w->ctx, cmd->key, w->id, cmd->fd);

  SetNonblock(cmd->fd);

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
  Peer* p = HtLookup(w, cmd->key);
  if (!p) {
    return;
  }
  int fd = p->fd;
  if (fd >= 0 && fd < kMaxFd) {
    w->fd_map[fd] = nullptr;
    if (w->use_fixed_files) {
      int fds[1] = {-1};
      io_uring_register_files_update(
          &w->ring, fd, fds, 1);
    }
  }
  HtRemove(w, cmd->key);
  BroadcastRouteRemove(w->ctx, cmd->key);
}

static void ProcessCmdWrite(Worker* w, Cmd* cmd) {
  Peer* p = HtLookup(w, cmd->key);
  if (p) {
    // Build a DERP frame wrapping the data.
    int frame_len = kFrameHeaderSize + cmd->data_len;
    uint8_t* frame = FrameAlloc(frame_len);
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
      case kCmdStop:
        w->running = 0;
        break;
    }
  }
}

// -- Cross-shard inbox processing --------------------------------------------

static void ProcessXfer(Worker* w) {
  DrainEventfd(w->xfer_efd);
  Xfer x{};
  while (XferPop(&w->xfer_ring, &x) == 0) {
    if (x.dst_fd < 0 || x.dst_fd >= kMaxFd ||
        !w->fd_map[x.dst_fd]) {
      free(x.frame);
      continue;
    }
    Peer* peer = w->fd_map[x.dst_fd];
    if (x.dst_gen != w->fd_gen[x.dst_fd]) {
      free(x.frame);
      continue;
    }
    EnqueueSend(w, peer, x.frame, x.frame_len);
  }
}

// -- Worker thread entry point -----------------------------------------------

static void* WorkerRun(void* arg) {
  auto* w = static_cast<Worker*>(arg);

  SubmitPollMultishot(w, w->event_fd, kOpPollCmd);
  SubmitPollMultishot(w, w->xfer_efd, kOpPollXfer);
  io_uring_submit(&w->ring);

  struct __kernel_timespec ts {};
  ts.tv_sec = 0;
  ts.tv_nsec = 100000000;  // 100ms

  while (__atomic_load_n(&w->running, __ATOMIC_ACQUIRE)) {
    io_uring_submit(&w->ring);

    struct io_uring_cqe* cqe = nullptr;
    int ret = io_uring_wait_cqe_timeout(
        &w->ring, &cqe, &ts);
    if (ret == -ETIME) {
      continue;
    }
    if (ret < 0) {
      if (ret == -EINTR) {
        continue;
      }
      break;
    }

    {
      unsigned head;
      struct io_uring_cqe* c = nullptr;
      int nr = 0;
      io_uring_for_each_cqe(&w->ring, head, c) {
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
            break;
          case kOpPollXfer:
            ProcessXfer(w);
            break;
          case kOpPollWrite:
            HandlePollWrite(w, c);
            break;
        }
        nr++;
      }
      io_uring_cq_advance(&w->ring, nr);
    }

    ProcessXfer(w);
    DrainDeferredRecvs(w);
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

  int pipefd[2];
  if (pipe(pipefd) < 0) {
    return -1;
  }
  ctx->pipe_rds[id] = pipefd[0];
  w->pipe_wr = pipefd[1];

  w->event_fd =
      eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (w->event_fd < 0) {
    close(pipefd[0]);
    close(w->pipe_wr);
    return -1;
  }

  w->xfer_efd =
      eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (w->xfer_efd < 0) {
    close(pipefd[0]);
    close(w->pipe_wr);
    close(w->event_fd);
    return -1;
  }

  struct io_uring_params params {};
  params.flags = IORING_SETUP_COOP_TASKRUN;
  int ret = io_uring_queue_init_params(
      kUringQueueDepth, &w->ring, &params);
  if (ret < 0) {
    memset(&params, 0, sizeof(params));
    ret = io_uring_queue_init_params(
        kUringQueueDepth, &w->ring, &params);
    if (ret < 0) {
      close(pipefd[0]);
      close(w->pipe_wr);
      close(w->event_fd);
      close(w->xfer_efd);
      return -1;
    }
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
    int br_ret;
    w->buf_ring = io_uring_setup_buf_ring(
        &w->ring, kPbufCount, id, 0, &br_ret);
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

  RingInit(&w->cmd_ring);
  XferRingInit(&w->xfer_ring);

  if (SlabInit(w) < 0) {
    close(pipefd[0]);
    close(w->pipe_wr);
    close(w->event_fd);
    close(w->xfer_efd);
    io_uring_queue_exit(&w->ring);
    return -1;
  }

  return 0;
}

static void WorkerDestroy(Worker* w, Ctx* ctx) {
  for (int i = 0; i < kHtCapacity; i++) {
    Peer* p = &w->ht[i];
    if (p->occupied == 1) {
      SendItem* item = p->send_head;
      while (item) {
        SendItem* next = item->next;
        FrameFree(item->data);
        SlabFreeItem(w, item);
        item = next;
      }
      p->send_head = nullptr;
      p->send_tail = nullptr;
    }
  }

  for (int i = 0; i < kMaxFd; i++) {
    while (w->notif_map[i]) {
      SendItem* item = w->notif_map[i];
      w->notif_map[i] = item->next;
      FrameFree(item->data);
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
