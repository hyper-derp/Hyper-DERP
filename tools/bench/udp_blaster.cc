/// @file udp_blaster.cc
/// @brief High-performance UDP packet generator for
///   AF_XDP relay benchmarking. Uses sendmmsg() for
///   batched sends and multiple threads.

#include <arpa/inet.h>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include "hyper_derp/bench.h"
#include "hyper_derp/hd_protocol.h"

using hyper_derp::HdFrameType;
using hyper_derp::HdWriteFrameHeader;
using hyper_derp::NowNs;
using hyper_derp::kHdFrameHeaderSize;

static volatile int g_running = 1;
static void SigHandler(int) { g_running = 0; }

// -- Config --

static const char* g_dst_ip = "10.50.0.2";
static uint16_t g_dst_port = 4000;
static const char* g_src_ip = nullptr;
static uint16_t g_src_port_base = 5000;
static int g_msg_size = 1400;
static int g_threads = 1;
static int g_duration = 10;
static double g_rate_mbps = 0;
static int g_batch = 64;
static const char* g_bind_dev = nullptr;

// -- Per-thread state --

struct ThreadCtx {
  int id;
  int fd;
  uint16_t src_port;
  uint64_t sent;
  uint64_t errors;
  double elapsed;
};

static void* BlastThread(void* arg) {
  auto* ctx = static_cast<ThreadCtx*>(arg);

  // Build HD Data frame payload.
  int frame_len = kHdFrameHeaderSize + g_msg_size;
  auto* frame = new uint8_t[frame_len];
  HdWriteFrameHeader(frame, HdFrameType::kData,
                     static_cast<uint32_t>(g_msg_size));
  for (int i = 0; i < g_msg_size; i++) {
    frame[kHdFrameHeaderSize + i] =
        static_cast<uint8_t>(i & 0xff);
  }

  // Destination address.
  sockaddr_in dst{};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(g_dst_port);
  inet_pton(AF_INET, g_dst_ip, &dst.sin_addr);

  // Prepare sendmmsg batch.
  auto* msgs = new mmsghdr[g_batch]();
  auto* iovs = new iovec[g_batch]();
  for (int i = 0; i < g_batch; i++) {
    iovs[i].iov_base = frame;
    iovs[i].iov_len = frame_len;
    msgs[i].msg_hdr.msg_name = &dst;
    msgs[i].msg_hdr.msg_namelen = sizeof(dst);
    msgs[i].msg_hdr.msg_iov = &iovs[i];
    msgs[i].msg_hdr.msg_iovlen = 1;
  }

  // Rate limiter.
  double bytes_per_ns = 0;
  if (g_rate_mbps > 0 && g_threads > 0) {
    double per_thread = g_rate_mbps / g_threads;
    bytes_per_ns = per_thread * 1e6 / 8.0 / 1e9;
  }

  uint64_t send_start = NowNs();
  uint64_t total_paced = 0;
  uint64_t deadline =
      send_start +
      static_cast<uint64_t>(g_duration) * 1000000000ULL;
  uint64_t sent = 0;
  uint64_t errors = 0;

  while (g_running && NowNs() < deadline) {
    // Rate limiting.
    if (bytes_per_ns > 0) {
      uint64_t now = NowNs();
      uint64_t allowed = static_cast<uint64_t>(
          (now - send_start) * bytes_per_ns);
      if (total_paced >= allowed) {
        uint64_t wait = static_cast<uint64_t>(
            (total_paced - allowed) / bytes_per_ns);
        if (wait > 1000) {
          struct timespec ts;
          ts.tv_sec = 0;
          ts.tv_nsec = static_cast<long>(wait);
          nanosleep(&ts, nullptr);
        }
        continue;
      }
    }

    int n = sendmmsg(ctx->fd, msgs, g_batch, 0);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        errors++;
        sched_yield();
        continue;
      }
      errors++;
      break;
    }
    sent += n;
    total_paced +=
        static_cast<uint64_t>(n) * g_msg_size;
  }

  ctx->sent = sent;
  ctx->errors = errors;
  ctx->elapsed = (NowNs() - send_start) / 1e9;

  delete[] frame;
  delete[] msgs;
  delete[] iovs;
  return nullptr;
}

static void Usage() {
  fprintf(stderr,
    "Usage: udp-blaster [options]\n"
    "  --dst-ip <ip>       Destination IP (10.50.0.2)\n"
    "  --dst-port <port>   Destination UDP port (4000)\n"
    "  --src-ip <ip>       Source IP (bind addr)\n"
    "  --src-port <port>   Base source port (5000)\n"
    "  --size <bytes>      Payload size (1400)\n"
    "  --threads <n>       Sender threads (1)\n"
    "  --duration <s>      Test duration (10)\n"
    "  --rate-mbps <mbps>  Rate cap (0=unlimited)\n"
    "  --batch <n>         sendmmsg batch size (64)\n"
    "  --bind-dev <dev>    Bind to device (SO_BINDTODEVICE)\n"
  );
}

int main(int argc, char* argv[]) {
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--dst-ip") == 0 &&
        i + 1 < argc) {
      g_dst_ip = argv[++i];
    } else if (strcmp(argv[i], "--dst-port") == 0 &&
               i + 1 < argc) {
      g_dst_port =
          static_cast<uint16_t>(atoi(argv[++i]));
    } else if (strcmp(argv[i], "--src-ip") == 0 &&
               i + 1 < argc) {
      g_src_ip = argv[++i];
    } else if (strcmp(argv[i], "--src-port") == 0 &&
               i + 1 < argc) {
      g_src_port_base =
          static_cast<uint16_t>(atoi(argv[++i]));
    } else if (strcmp(argv[i], "--size") == 0 &&
               i + 1 < argc) {
      g_msg_size = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--threads") == 0 &&
               i + 1 < argc) {
      g_threads = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--duration") == 0 &&
               i + 1 < argc) {
      g_duration = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--rate-mbps") == 0 &&
               i + 1 < argc) {
      g_rate_mbps = atof(argv[++i]);
    } else if (strcmp(argv[i], "--batch") == 0 &&
               i + 1 < argc) {
      g_batch = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--bind-dev") == 0 &&
               i + 1 < argc) {
      g_bind_dev = argv[++i];
    } else {
      Usage();
      return 1;
    }
  }

  signal(SIGPIPE, SIG_IGN);
  signal(SIGINT, SigHandler);

  fprintf(stderr,
      "udp-blaster: %s:%d → %s:%d, %dB, %d threads, "
      "%ds, batch=%d",
      g_src_ip ? g_src_ip : "*",
      g_src_port_base, g_dst_ip, g_dst_port,
      g_msg_size, g_threads, g_duration, g_batch);
  if (g_rate_mbps > 0) {
    fprintf(stderr, ", rate=%.0f Mbps", g_rate_mbps);
  }
  fprintf(stderr, "\n");

  // Create threads.
  auto* threads = new pthread_t[g_threads];
  auto* ctxs = new ThreadCtx[g_threads]();

  for (int i = 0; i < g_threads; i++) {
    ctxs[i].id = i;
    ctxs[i].src_port = g_src_port_base + i;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
      perror("socket");
      return 1;
    }

    // Bind to source.
    sockaddr_in src{};
    src.sin_family = AF_INET;
    src.sin_port = htons(ctxs[i].src_port);
    if (g_src_ip) {
      inet_pton(AF_INET, g_src_ip, &src.sin_addr);
    }
    if (bind(fd, reinterpret_cast<sockaddr*>(&src),
             sizeof(src)) < 0) {
      perror("bind");
      return 1;
    }

    // Bind to device if specified.
    if (g_bind_dev) {
      if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE,
                     g_bind_dev,
                     strlen(g_bind_dev)) < 0) {
        perror("SO_BINDTODEVICE");
      }
    }

    // Increase send buffer.
    int sndbuf = 16 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF,
               &sndbuf, sizeof(sndbuf));

    ctxs[i].fd = fd;
  }

  // Start threads.
  for (int i = 0; i < g_threads; i++) {
    pthread_create(&threads[i], nullptr,
                   BlastThread, &ctxs[i]);
  }

  // Wait.
  for (int i = 0; i < g_threads; i++) {
    pthread_join(threads[i], nullptr);
  }

  // Aggregate.
  uint64_t total_sent = 0;
  uint64_t total_errors = 0;
  double max_elapsed = 0;
  for (int i = 0; i < g_threads; i++) {
    total_sent += ctxs[i].sent;
    total_errors += ctxs[i].errors;
    if (ctxs[i].elapsed > max_elapsed) {
      max_elapsed = ctxs[i].elapsed;
    }
    close(ctxs[i].fd);
  }

  int frame_len = kHdFrameHeaderSize + g_msg_size;
  double pps = total_sent / max_elapsed;
  double mbps =
      total_sent * frame_len * 8.0 / 1e6 / max_elapsed;

  fprintf(stderr,
      "\nResults:\n"
      "  Packets sent: %" PRIu64 "\n"
      "  Errors:       %" PRIu64 "\n"
      "  Duration:     %.1f s\n"
      "  Rate:         %.0f pps\n"
      "  Throughput:   %.0f Mbps (%.1f Gbps)\n",
      total_sent, total_errors, max_elapsed,
      pps, mbps, mbps / 1000);

  delete[] threads;
  delete[] ctxs;
  return 0;
}
