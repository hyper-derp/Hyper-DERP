/// @file derp_scale_test_uring.cc
/// @brief io_uring-based multi-peer scaling test for DERP relay.
///
/// Multi-threaded io_uring event loops, each owning a partition
/// of peers with its own ring and provided buffer pool. Scales
/// to 10k+ active pairs.
///
/// Usage:
///   derp-scale-test-uring --peers N [--active-pairs K]
///     [--msg-size S] [--duration D] [--rate-mbps R]
///     [--client-threads T] [--pin-core C]
///     [--host H] [--port P] [--tls] [--json]

#include <bit>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <liburing.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sodium.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include "hyper_derp/bench.h"
#include "hyper_derp/client.h"
#include "hyper_derp/handshake.h"
#include "hyper_derp/protocol.h"

using hyper_derp::ClientClose;
using hyper_derp::ClientConnect;
using hyper_derp::ClientHandshake;
using hyper_derp::ClientInit;
using hyper_derp::ClientTlsConnect;
using hyper_derp::ClientUpgrade;
using hyper_derp::DerpClient;
using hyper_derp::FrameType;
using hyper_derp::IsValidPayloadLen;
using hyper_derp::kFrameHeaderSize;
using hyper_derp::Key;
using hyper_derp::kKeySize;
using hyper_derp::kMaxFramePayload;
using hyper_derp::NowIso8601;
using hyper_derp::NowNs;
using hyper_derp::ReadFrameType;
using hyper_derp::ReadPayloadLen;
using hyper_derp::WriteFrameHeader;

// -- Config -----------------------------------------------------------

static const char* g_host = "127.0.0.1";
static uint16_t g_port = 3340;
static int g_num_peers = 20;
static int g_active_pairs = 0;
static int g_msg_size = 1024;
static int g_duration_sec = 5;
static double g_rate_mbps = 100;
static bool g_json = false;
static bool g_use_tls = false;
static int g_pin_core = -1;
static int g_num_threads = 1;

// -- io_uring ops -----------------------------------------------------

static constexpr uint8_t kOpRecv = 1;
static constexpr uint8_t kOpSend = 2;
static constexpr uint8_t kOpTimer = 6;

static inline uint64_t MakeUd(uint8_t op, int idx) {
  return (static_cast<uint64_t>(op) << 56) |
         static_cast<uint64_t>(idx);
}
static inline uint8_t UdOp(uint64_t ud) {
  return static_cast<uint8_t>(ud >> 56);
}
static inline int UdIdx(uint64_t ud) {
  return static_cast<int>(ud & 0x00FFFFFFFFFFFFFFULL);
}

// -- Per-peer state ---------------------------------------------------

// Reassembly buffer computed at runtime: must hold one full
// frame (header + key + msg) plus one provided buffer.
static int g_rbuf_size = 8192;

struct ScalePeer {
  DerpClient client;
  bool connected;
  int partner_idx;
  bool is_sender;

  // Pre-built send frame (header + key + payload).
  uint8_t* send_frame;
  int send_frame_len;
  bool send_pending;

  // Recv reassembly (heap-allocated, g_rbuf_size bytes).
  uint8_t* rbuf;
  int rbuf_len;
  bool recv_pending;

  // Stats.
  uint64_t msgs_sent;
  uint64_t msgs_recv;
  uint64_t bytes_sent;
  uint64_t bytes_recv;
  uint64_t send_errors;
};

// -- Globals ----------------------------------------------------------

static ScalePeer* g_peers = nullptr;

// -- Per-thread worker context ----------------------------------------

static constexpr int kProvBufSize = 4096;

struct ClientWorker {
  int id;
  int peer_start;
  int peer_count;
  int pin_core;

  struct io_uring ring;
  struct io_uring_buf_ring* buf_ring;
  uint8_t* buf_base;
  int buf_count;
  uint16_t buf_group_id;

  // Stats aggregated after join.
  uint64_t total_sent;
  uint64_t total_recv;
  uint64_t total_bytes_sent;
  uint64_t total_bytes_recv;
  uint64_t total_send_errors;
  double traffic_secs;
};

// -- Token bucket rate limiter ----------------------------------------

struct TokenBucket {
  double bytes_per_ns;
  uint64_t start_ns;
  uint64_t total_bytes;
};

static inline int64_t BucketAvail(
    const TokenBucket* tb, uint64_t now_ns) {
  if (tb->bytes_per_ns <= 0) {
    return INT64_MAX;
  }
  int64_t allowed = static_cast<int64_t>(
      (now_ns - tb->start_ns) * tb->bytes_per_ns);
  return allowed - static_cast<int64_t>(tb->total_bytes);
}

// -- Helpers ----------------------------------------------------------

static void SetNonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
}

static bool ConnectPeer(ScalePeer* p) {
  if (!ClientConnect(&p->client, g_host, g_port)) {
    return false;
  }
  if (g_use_tls) {
    if (!ClientTlsConnect(&p->client)) {
      ClientClose(&p->client);
      return false;
    }
  }
  if (!ClientUpgrade(&p->client)) {
    ClientClose(&p->client);
    return false;
  }
  if (!ClientHandshake(&p->client)) {
    ClientClose(&p->client);
    return false;
  }
  p->connected = true;
  return true;
}

static inline uint8_t* BufPtr(ClientWorker* w, int bid) {
  return w->buf_base + bid * kProvBufSize;
}

static void ReturnBuf(ClientWorker* w, int bid) {
  io_uring_buf_ring_add(
      w->buf_ring, BufPtr(w, bid), kProvBufSize, bid,
      io_uring_buf_ring_mask(w->buf_count), 0);
  io_uring_buf_ring_advance(w->buf_ring, 1);
}

static void SubmitRecvMultishot(
    ClientWorker* w, ScalePeer* p, int idx) {
  struct io_uring_sqe* sqe =
      io_uring_get_sqe(&w->ring);
  if (!sqe) {
    io_uring_submit(&w->ring);
    sqe = io_uring_get_sqe(&w->ring);
    if (!sqe) return;
  }
  io_uring_prep_recv(sqe, p->client.fd, nullptr, 0, 0);
  sqe->ioprio |= IORING_RECV_MULTISHOT;
  sqe->buf_group = w->buf_group_id;
  sqe->flags |= IOSQE_BUFFER_SELECT;
  io_uring_sqe_set_data64(sqe, MakeUd(kOpRecv, idx));
  p->recv_pending = true;
}

static void SubmitSend(
    ClientWorker* w, ScalePeer* p, int idx) {
  struct io_uring_sqe* sqe =
      io_uring_get_sqe(&w->ring);
  if (!sqe) {
    io_uring_submit(&w->ring);
    sqe = io_uring_get_sqe(&w->ring);
    if (!sqe) return;
  }
  io_uring_prep_send(sqe, p->client.fd,
                     p->send_frame, p->send_frame_len,
                     MSG_NOSIGNAL);
  io_uring_sqe_set_data64(sqe, MakeUd(kOpSend, idx));
  p->send_pending = true;
}

// -- CQE handlers ----------------------------------------------------

static void HandleRecv(
    ClientWorker* w, struct io_uring_cqe* cqe) {
  int idx = UdIdx(cqe->user_data);
  ScalePeer* p = &g_peers[idx];

  if (cqe->res <= 0) {
    p->recv_pending = false;
    if (p->connected) {
      SubmitRecvMultishot(w, p, idx);
    }
    return;
  }

  if (!(cqe->flags & IORING_CQE_F_BUFFER)) {
    p->recv_pending = false;
    return;
  }
  int bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
  uint8_t* buf = BufPtr(w, bid);
  int len = cqe->res;

  int space = g_rbuf_size - p->rbuf_len;
  int copy = len < space ? len : space;
  if (copy > 0) {
    memcpy(p->rbuf + p->rbuf_len, buf, copy);
    p->rbuf_len += copy;
  }

  ReturnBuf(w, bid);

  while (p->rbuf_len >= kFrameHeaderSize) {
    uint32_t plen = ReadPayloadLen(p->rbuf);
    if (!IsValidPayloadLen(plen)) {
      break;
    }
    int frame_len =
        kFrameHeaderSize + static_cast<int>(plen);
    if (p->rbuf_len < frame_len) {
      if (frame_len > g_rbuf_size) {
        p->rbuf_len = 0;
      }
      break;
    }

    FrameType ftype = ReadFrameType(p->rbuf);
    if (ftype == FrameType::kRecvPacket) {
      int data_len = static_cast<int>(plen) - kKeySize;
      if (data_len > 0) {
        p->bytes_recv += data_len;
      }
      p->msgs_recv++;
    }

    int remaining = p->rbuf_len - frame_len;
    if (remaining > 0) {
      memmove(p->rbuf, p->rbuf + frame_len, remaining);
    }
    p->rbuf_len = remaining;
  }

  if (!(cqe->flags & IORING_CQE_F_MORE)) {
    p->recv_pending = false;
    if (p->connected) {
      SubmitRecvMultishot(w, p, idx);
    }
  }
}

static void HandleSend(struct io_uring_cqe* cqe) {
  int idx = UdIdx(cqe->user_data);
  ScalePeer* p = &g_peers[idx];
  p->send_pending = false;

  if (cqe->res > 0) {
    p->msgs_sent++;
    p->bytes_sent += g_msg_size;
  } else if (cqe->res < 0 && cqe->res != -EAGAIN) {
    p->send_errors++;
  }
}

// -- Worker thread ----------------------------------------------------

static void* WorkerThread(void* arg) {
  ClientWorker* w = static_cast<ClientWorker*>(arg);

  // Pin to core.
  if (w->pin_core >= 0) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(w->pin_core, &cpuset);
    sched_setaffinity(0, sizeof(cpuset), &cpuset);
  }

  int ps = w->peer_start;
  int pe = w->peer_start + w->peer_count;

  // Init io_uring ring.
  unsigned depth = std::bit_ceil(
      static_cast<unsigned>(w->peer_count + 256));
  if (depth > 32768) depth = 32768;
  if (depth < 256) depth = 256;

  struct io_uring_params params {};
  params.flags = IORING_SETUP_COOP_TASKRUN;
  if (io_uring_queue_init_params(
          depth, &w->ring, &params) < 0) {
    fprintf(stderr, "worker %d: ring init failed\n",
            w->id);
    return nullptr;
  }

  // Provided buffer ring.
  int nr = std::bit_ceil(
      static_cast<unsigned>(w->peer_count * 4));
  if (nr < 256) nr = 256;
  if (nr > 4096) nr = 4096;
  w->buf_count = nr;
  w->buf_group_id = static_cast<uint16_t>(w->id);

  int ret;
  w->buf_ring = io_uring_setup_buf_ring(
      &w->ring, nr, w->buf_group_id, 0, &ret);
  if (!w->buf_ring) {
    fprintf(stderr,
            "worker %d: buf ring failed: %d\n",
            w->id, ret);
    io_uring_queue_exit(&w->ring);
    return nullptr;
  }

  w->buf_base = new uint8_t[nr * kProvBufSize];
  for (int i = 0; i < nr; i++) {
    io_uring_buf_ring_add(
        w->buf_ring, w->buf_base + i * kProvBufSize,
        kProvBufSize, i,
        io_uring_buf_ring_mask(nr), i);
  }
  io_uring_buf_ring_advance(w->buf_ring, nr);

  // Submit initial multishot recv for receivers.
  for (int i = ps; i < pe; i++) {
    if (g_peers[i].connected && !g_peers[i].is_sender) {
      SubmitRecvMultishot(w, &g_peers[i], i);
    }
  }

  // Duration timer.
  int send_sec =
      g_duration_sec > 2 ? g_duration_sec - 1 : 1;
  struct __kernel_timespec send_ts {};
  send_ts.tv_sec = send_sec;
  {
    struct io_uring_sqe* sqe =
        io_uring_get_sqe(&w->ring);
    io_uring_prep_timeout(sqe, &send_ts, 0, 0);
    io_uring_sqe_set_data64(
        sqe, MakeUd(kOpTimer, 0));
  }

  // Token bucket — rate split across threads.
  TokenBucket bucket {};
  bucket.bytes_per_ns =
      g_rate_mbps * 1e6 / 8.0 / 1e9 / g_num_threads;
  bucket.start_ns = NowNs();

  io_uring_submit(&w->ring);
  uint64_t t_traffic = NowNs();
  bool sends_active = true;

  // -- Event loop ---------------------------------------------------

  int send_cursor = ps;
  bool running = true;

  while (running) {
    if (sends_active) {
      uint64_t now = NowNs();
      int64_t avail = BucketAvail(&bucket, now);
      int submitted = 0;

      for (int scan = 0;
           scan < w->peer_count && avail > 0;
           scan++) {
        int idx = send_cursor;
        send_cursor++;
        if (send_cursor >= pe) {
          send_cursor = ps;
        }

        ScalePeer* p = &g_peers[idx];
        if (!p->connected || !p->is_sender ||
            p->send_pending) {
          continue;
        }

        SubmitSend(w, p, idx);
        bucket.total_bytes += g_msg_size;
        avail -= g_msg_size;
        submitted++;
      }

      if (submitted == 0) {
        sched_yield();
      }
    }

    io_uring_submit(&w->ring);

    struct __kernel_timespec pace {};
    if (sends_active) {
      pace.tv_nsec = 500000;
    } else {
      pace.tv_nsec = 5000000;
    }
    struct io_uring_cqe* cqe = nullptr;
    io_uring_wait_cqe_timeout(&w->ring, &cqe, &pace);

    unsigned head;
    int ncqe = 0;
    io_uring_for_each_cqe(&w->ring, head, cqe) {
      uint8_t op = UdOp(cqe->user_data);
      switch (op) {
        case kOpRecv:
          HandleRecv(w, cqe);
          break;
        case kOpSend:
          HandleSend(cqe);
          break;
        case kOpTimer:
          if (sends_active) {
            sends_active = false;
            // Drain timeout — must be static.
            static struct __kernel_timespec drain {};
            drain.tv_sec = 2;
            drain.tv_nsec = 0;
            struct io_uring_sqe* sqe =
                io_uring_get_sqe(&w->ring);
            if (sqe) {
              io_uring_prep_timeout(
                  sqe, &drain, 0, 0);
              io_uring_sqe_set_data64(
                  sqe, MakeUd(kOpTimer, 1));
            }
          } else {
            running = false;
          }
          break;
      }
      ncqe++;
      if (ncqe >= 256) {
        io_uring_cq_advance(&w->ring, ncqe);
        io_uring_submit(&w->ring);
        ncqe = 0;
        break;
      }
    }
    if (ncqe > 0) {
      io_uring_cq_advance(&w->ring, ncqe);
    }
  }

  w->traffic_secs = (NowNs() - t_traffic) / 1e9;

  // Aggregate this worker's peer stats.
  for (int i = ps; i < pe; i++) {
    ScalePeer* p = &g_peers[i];
    if (p->is_sender) {
      w->total_sent += p->msgs_sent;
      w->total_bytes_sent += p->bytes_sent;
      w->total_send_errors += p->send_errors;
    } else {
      w->total_recv += p->msgs_recv;
      w->total_bytes_recv += p->bytes_recv;
    }
  }

  // Cleanup ring.
  io_uring_free_buf_ring(
      &w->ring, w->buf_ring, w->buf_count,
      w->buf_group_id);
  delete[] w->buf_base;
  io_uring_queue_exit(&w->ring);

  return nullptr;
}

// -- Main -------------------------------------------------------------

int main(int argc, char** argv) {
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
      g_host = argv[++i];
    } else if (strcmp(argv[i], "--port") == 0 &&
               i + 1 < argc) {
      g_port = static_cast<uint16_t>(atoi(argv[++i]));
    } else if (strcmp(argv[i], "--peers") == 0 &&
               i + 1 < argc) {
      g_num_peers = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--active-pairs") == 0 &&
               i + 1 < argc) {
      g_active_pairs = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--msg-size") == 0 &&
               i + 1 < argc) {
      g_msg_size = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--duration") == 0 &&
               i + 1 < argc) {
      g_duration_sec = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--rate-mbps") == 0 &&
               i + 1 < argc) {
      g_rate_mbps = atof(argv[++i]);
    } else if (strcmp(argv[i], "--client-threads") == 0 &&
               i + 1 < argc) {
      g_num_threads = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--json") == 0) {
      g_json = true;
    } else if (strcmp(argv[i], "--tls") == 0) {
      g_use_tls = true;
    } else if (strcmp(argv[i], "--pin-core") == 0 &&
               i + 1 < argc) {
      g_pin_core = atoi(argv[++i]);
    } else {
      fprintf(stderr,
          "Usage: derp-scale-test-uring [options]\n"
          "  --peers N           Total peers (20)\n"
          "  --active-pairs K    Pairs with traffic\n"
          "  --msg-size N        Message size (1024)\n"
          "  --duration N        Traffic seconds (5)\n"
          "  --rate-mbps F       Aggregate send rate "
          "(100, 0=unlimited)\n"
          "  --client-threads N  io_uring threads (1)\n"
          "  --host/--port       Relay address\n"
          "  --tls               TLS 1.3 (kTLS)\n"
          "  --pin-core N        Pin first thread to N\n"
          "  --json              JSON to stdout\n");
      return 1;
    }
  }

  if (g_num_peers < 2) {
    fprintf(stderr, "Need at least 2 peers\n");
    return 1;
  }
  if (g_num_threads < 1) g_num_threads = 1;
  if (g_num_threads > 32) g_num_threads = 32;

  // Round peers up to even multiple of threads × 2
  // so each thread gets whole pairs.
  int unit = g_num_threads * 2;
  g_num_peers = ((g_num_peers + unit - 1) / unit) * unit;

  int max_pairs = g_num_peers / 2;
  if (g_active_pairs <= 0) {
    g_active_pairs = max_pairs < 10 ? max_pairs : 10;
  }
  if (g_active_pairs > max_pairs) {
    g_active_pairs = max_pairs;
  }

  if (sodium_init() < 0) {
    fprintf(stderr, "sodium_init failed\n");
    return 1;
  }
  signal(SIGPIPE, SIG_IGN);

  // Raise fd limit for high peer counts.
  {
    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    rlim_t need =
        static_cast<rlim_t>(g_num_peers + 256);
    if (rl.rlim_cur < need) {
      rl.rlim_cur = need < rl.rlim_max
                        ? need : rl.rlim_max;
      setrlimit(RLIMIT_NOFILE, &rl);
    }
  }

  // Size rbuf to hold one full frame + one provided buffer.
  int frame_max = kFrameHeaderSize + kKeySize + g_msg_size;
  g_rbuf_size = frame_max + kProvBufSize;
  g_rbuf_size = static_cast<int>(
      std::bit_ceil(static_cast<unsigned>(g_rbuf_size)));

  g_peers = new ScalePeer[g_num_peers]();
  for (int i = 0; i < g_num_peers; i++) {
    (void)ClientInit(&g_peers[i].client);
    g_peers[i].rbuf = new uint8_t[g_rbuf_size]();
  }

  // Token bucket cap.
  if (g_rate_mbps <= 0) {
    g_rate_mbps = 5000;
    fprintf(stderr,
            "Rate capped at %.0f Mbps (use --rate-mbps "
            "to override)\n", g_rate_mbps);
  }

  // -- Phase 1: Connect -----------------------------------------------

  fprintf(stderr,
          "=== Phase 1: Connecting %d peers ===\n",
          g_num_peers);

  uint64_t t_connect = NowNs();
  int connected = 0;
  int failed = 0;

  for (int i = 0; i < g_num_peers; i++) {
    if (ConnectPeer(&g_peers[i])) {
      connected++;
    } else {
      failed++;
      if (failed <= 5) {
        fprintf(stderr, "  peer %d: connect failed\n", i);
      } else if (failed == 6) {
        fprintf(stderr,
                "  ... (suppressing further)\n");
      }
    }
    if ((i + 1) % 100 == 0 || i + 1 == g_num_peers) {
      fprintf(stderr, "  %d/%d connected (%d failed)\n",
              connected, i + 1, failed);
    }
  }

  double connect_ms = (NowNs() - t_connect) / 1e6;
  fprintf(stderr, "Connected %d/%d in %.0f ms\n\n",
          connected, g_num_peers, connect_ms);

  if (connected < 2) {
    fprintf(stderr, "Not enough peers connected\n");
    delete[] g_peers;
    return 1;
  }

  // -- Build pair list ------------------------------------------------
  // Pairs are adjacent: (0,1), (2,3), ... so each thread's
  // contiguous range contains whole pairs.

  int actual_pairs = 0;
  for (int i = 0; i + 1 < g_num_peers &&
       actual_pairs < g_active_pairs; i += 2) {
    if (!g_peers[i].connected ||
        !g_peers[i + 1].connected) {
      continue;
    }
    int si = i;
    int ri = i + 1;
    g_peers[si].is_sender = true;
    g_peers[si].partner_idx = ri;
    g_peers[ri].is_sender = false;
    g_peers[ri].partner_idx = si;

    int payload_len = kKeySize + g_msg_size;
    int frame_len = kFrameHeaderSize + payload_len;
    g_peers[si].send_frame = new uint8_t[frame_len];
    g_peers[si].send_frame_len = frame_len;
    WriteFrameHeader(g_peers[si].send_frame,
                     FrameType::kSendPacket,
                     static_cast<uint32_t>(payload_len));
    memcpy(g_peers[si].send_frame + kFrameHeaderSize,
           g_peers[ri].client.public_key.data(),
           kKeySize);
    for (int b = 0; b < g_msg_size; b++) {
      g_peers[si].send_frame[kFrameHeaderSize +
                             kKeySize + b] =
          static_cast<uint8_t>(b & 0xff);
    }

    actual_pairs++;
  }

  if (actual_pairs == 0) {
    fprintf(stderr, "No active pairs\n");
    delete[] g_peers;
    return 1;
  }

  // -- Phase 2: io_uring traffic --------------------------------------

  fprintf(stderr,
          "=== Phase 2: Traffic (%d pairs, %ds, "
          "%dB, %.0f Mbps, %d threads) ===\n",
          actual_pairs, g_duration_sec,
          g_msg_size, g_rate_mbps, g_num_threads);

  // Set all peer fds to non-blocking.
  for (int i = 0; i < g_num_peers; i++) {
    if (g_peers[i].connected) {
      SetNonblock(g_peers[i].client.fd);
    }
  }

  // Partition peers across threads.
  int peers_per_thread = g_num_peers / g_num_threads;
  auto* workers = new ClientWorker[g_num_threads]();
  auto* threads = new pthread_t[g_num_threads];

  for (int t = 0; t < g_num_threads; t++) {
    workers[t].id = t;
    workers[t].peer_start = t * peers_per_thread;
    workers[t].peer_count = peers_per_thread;
    if (g_pin_core >= 0) {
      workers[t].pin_core = g_pin_core + t;
    } else {
      workers[t].pin_core = -1;
    }
  }

  // Spawn worker threads.
  for (int t = 0; t < g_num_threads; t++) {
    pthread_create(&threads[t], nullptr,
                   WorkerThread, &workers[t]);
    if (workers[t].pin_core >= 0) {
      fprintf(stderr, "  thread %d: peers [%d..%d), "
              "core %d\n", t, workers[t].peer_start,
              workers[t].peer_start +
              workers[t].peer_count,
              workers[t].pin_core);
    }
  }

  // Wait for all threads to finish.
  for (int t = 0; t < g_num_threads; t++) {
    pthread_join(threads[t], nullptr);
  }

  // -- Aggregate stats ------------------------------------------------

  uint64_t total_sent = 0, total_recv = 0;
  uint64_t total_bytes_sent = 0, total_bytes_recv = 0;
  uint64_t total_send_errors = 0;
  double max_traffic_secs = 0;

  for (int t = 0; t < g_num_threads; t++) {
    total_sent += workers[t].total_sent;
    total_recv += workers[t].total_recv;
    total_bytes_sent += workers[t].total_bytes_sent;
    total_bytes_recv += workers[t].total_bytes_recv;
    total_send_errors += workers[t].total_send_errors;
    if (workers[t].traffic_secs > max_traffic_secs) {
      max_traffic_secs = workers[t].traffic_secs;
    }
  }

  double loss_pct = 0;
  if (total_sent > 0) {
    loss_pct =
        100.0 * (1.0 - static_cast<double>(total_recv) /
                            total_sent);
  }
  double throughput_mbps =
      max_traffic_secs > 0
          ? total_bytes_recv * 8.0 / 1e6 /
              max_traffic_secs
          : 0;

  fprintf(stderr,
          "\n=== Results ===\n"
          "Engine:        io_uring (%d threads)\n"
          "Peers:         %d / %d connected\n"
          "Connect time:  %.0f ms\n"
          "Active pairs:  %d\n"
          "Traffic time:  %.1f s\n"
          "Messages sent: %" PRIu64 "\n"
          "Messages recv: %" PRIu64 "\n"
          "Message loss:  %.2f%%\n"
          "Send errors:   %" PRIu64 "\n"
          "Throughput:    %.1f Mbps (recv)\n",
          g_num_threads,
          connected, g_num_peers,
          connect_ms, actual_pairs,
          max_traffic_secs,
          total_sent, total_recv, loss_pct,
          total_send_errors, throughput_mbps);

  if (g_json) {
    char ts[32];
    NowIso8601(ts, sizeof(ts));
    printf("{\n"
           "  \"timestamp\": \"%s\",\n"
           "  \"relay\": \"hyper-derp\",\n"
           "  \"engine\": \"io_uring\",\n"
           "  \"client_threads\": %d,\n"
           "  \"total_peers\": %d,\n"
           "  \"connected_peers\": %d,\n"
           "  \"connect_failed\": %d,\n"
           "  \"connect_time_ms\": %.1f,\n"
           "  \"active_pairs\": %d,\n"
           "  \"duration_sec\": %d,\n"
           "  \"message_size\": %d,\n"
           "  \"rate_mbps\": %.1f,\n"
           "  \"messages_sent\": %" PRIu64 ",\n"
           "  \"messages_recv\": %" PRIu64 ",\n"
           "  \"message_loss_pct\": %.4f,\n"
           "  \"send_errors\": %" PRIu64 ",\n"
           "  \"throughput_mbps\": %.1f\n"
           "}\n",
           ts, g_num_threads,
           g_num_peers, connected, failed,
           connect_ms, actual_pairs,
           g_duration_sec, g_msg_size, g_rate_mbps,
           total_sent, total_recv, loss_pct,
           total_send_errors, throughput_mbps);
  }

  // Cleanup.
  for (int i = 0; i < g_num_peers; i++) {
    delete[] g_peers[i].rbuf;
    if (g_peers[i].send_frame) {
      delete[] g_peers[i].send_frame;
    }
    if (g_peers[i].connected) {
      ClientClose(&g_peers[i].client);
    }
  }
  delete[] g_peers;
  delete[] workers;
  delete[] threads;
  return 0;
}
