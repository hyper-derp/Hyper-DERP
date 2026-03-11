/// @file derp_scale_test.cc
/// @brief Multi-peer connection scaling test for DERP relay.
///
/// Connects N peers, then runs traffic between a small number
/// of active pairs. Measures connection capacity, throughput,
/// and message loss.
///
/// Usage:
///   derp-scale-test --peers N [--active-pairs K]
///     [--msg-size S] [--duration D] [--json]

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <pthread.h>
#include <signal.h>
#include <sodium.h>
#include <sys/socket.h>
#include <unistd.h>

#include "hyper_derp/bench.h"
#include "hyper_derp/client.h"
#include "hyper_derp/handshake.h"
#include "hyper_derp/protocol.h"

using namespace hyper_derp;

struct PeerState {
  DerpClient client;
  bool connected;
};

struct PairResult {
  uint64_t msgs_sent;
  uint64_t msgs_recv;
  uint64_t bytes_sent;
  uint64_t bytes_recv;
  uint64_t send_errors;
};

// Config.
static const char* g_host = "127.0.0.1";
static uint16_t g_port = 3340;
static int g_num_peers = 20;
static int g_active_pairs = 0;
static int g_msg_size = 1024;
static int g_duration_sec = 5;
static double g_rate_mbps = 100;  // Mbps, 0 = unlimited
static bool g_json = false;

// Traffic thread state.
static std::atomic<int> g_stop{0};
static PeerState* g_peers = nullptr;

struct TrafficArg {
  int sender_idx;
  int receiver_idx;
  PairResult result;
  bool is_sender;
};

static void* SenderThread(void* arg) {
  auto* ta = static_cast<TrafficArg*>(arg);
  PeerState* s = &g_peers[ta->sender_idx];
  PeerState* r = &g_peers[ta->receiver_idx];

  // Send timeout to avoid blocking forever.
  struct timeval tv = {1, 0};
  setsockopt(s->client.fd, SOL_SOCKET, SO_SNDTIMEO,
             &tv, sizeof(tv));

  auto* payload = new uint8_t[g_msg_size];
  for (int i = 0; i < g_msg_size; i++) {
    payload[i] = static_cast<uint8_t>(i & 0xff);
  }

  // Stop 1s before duration ends to let pipeline drain.
  int send_sec =
      g_duration_sec > 2 ? g_duration_sec - 1 : 1;
  uint64_t deadline_ns =
      NowNs() +
      static_cast<uint64_t>(send_sec) * 1000000000ULL;

  // Rate limiter: bytes-per-nanosecond budget.
  // Per-sender rate = total / active_pairs.
  double bytes_per_ns = 0;
  if (g_rate_mbps > 0 && g_active_pairs > 0) {
    double per_sender_mbps =
        g_rate_mbps / g_active_pairs;
    bytes_per_ns = per_sender_mbps * 1e6 / 8.0 / 1e9;
  }
  uint64_t send_start = NowNs();
  uint64_t total_bytes_paced = 0;

  while (NowNs() < deadline_ns) {
    // Token-bucket pace: spin-wait until budget allows.
    if (bytes_per_ns > 0) {
      uint64_t allowed = static_cast<uint64_t>(
          (NowNs() - send_start) * bytes_per_ns);
      while (total_bytes_paced + g_msg_size > allowed) {
        uint64_t wait_ns = static_cast<uint64_t>(
            (total_bytes_paced + g_msg_size - allowed) /
            bytes_per_ns);
        if (wait_ns > 1000) {
          struct timespec ts_sleep;
          ts_sleep.tv_sec = 0;
          ts_sleep.tv_nsec =
              static_cast<long>(wait_ns);
          nanosleep(&ts_sleep, nullptr);
        }
        allowed = static_cast<uint64_t>(
            (NowNs() - send_start) * bytes_per_ns);
        if (NowNs() >= deadline_ns) {
          goto done;
        }
      }
    }

    if (!ClientSendPacket(&s->client,
                          r->client.public_key,
                          payload, g_msg_size)) {
      ta->result.send_errors++;
      continue;
    }
    ta->result.msgs_sent++;
    ta->result.bytes_sent += g_msg_size;
    total_bytes_paced += g_msg_size;
  }
done:

  delete[] payload;
  return nullptr;
}

static void* ReceiverThread(void* arg) {
  auto* ta = static_cast<TrafficArg*>(arg);
  PeerState* r = &g_peers[ta->receiver_idx];

  (void)ClientSetTimeout(&r->client, 100);

  uint8_t buf[kMaxFramePayload];
  int buf_len;
  FrameType ftype;
  int idle = 0;

  while (true) {
    auto result = ClientRecvFrame(
        &r->client, &ftype, buf, &buf_len, sizeof(buf));
    if (!result) {
      if (g_stop.load()) {
        idle++;
        // 50 * 100ms = 5s of drain.
        if (idle >= 50) break;
      }
      continue;
    }
    idle = 0;
    if (ftype == FrameType::kRecvPacket) {
      int data_len = buf_len - kKeySize;
      if (data_len > 0) {
        ta->result.bytes_recv += data_len;
      }
      ta->result.msgs_recv++;
    }
  }

  return nullptr;
}

static bool ConnectPeer(PeerState* p) {
  if (!ClientConnect(&p->client, g_host, g_port)) {
    return false;
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
    } else if (strcmp(argv[i], "--json") == 0) {
      g_json = true;
    } else {
      fprintf(stderr,
          "Usage: derp-scale-test [options]\n"
          "  --peers N           Total peers (20)\n"
          "  --active-pairs K    Pairs with traffic "
          "(default: min(10, peers/2))\n"
          "  --msg-size N        Message size (1024)\n"
          "  --duration N        Traffic seconds (5)\n"
          "  --rate-mbps F       Aggregate send rate "
          "(100, 0=unlimited)\n"
          "  --host/--port       Relay address\n"
          "  --json              JSON to stdout\n");
      return 1;
    }
  }

  if (g_num_peers < 2) {
    fprintf(stderr, "Need at least 2 peers\n");
    return 1;
  }

  // Default active pairs: min(10, peers/2).
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

  g_peers = new PeerState[g_num_peers]();
  for (int i = 0; i < g_num_peers; i++) {
    (void)ClientInit(&g_peers[i].client);
  }

  // Phase 1: Connect all peers sequentially.
  fprintf(stderr, "=== Phase 1: Connecting %d peers ===\n",
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
        fprintf(stderr, "  ... (suppressing further)\n");
      }
    }
    // Progress every 100 peers.
    if ((i + 1) % 100 == 0 || i + 1 == g_num_peers) {
      fprintf(stderr, "  %d/%d connected (%d failed)\n",
              connected, i + 1, failed);
    }
  }

  double connect_ms = (NowNs() - t_connect) / 1e6;
  fprintf(stderr, "Connected %d/%d in %.0f ms\n\n",
          connected, g_num_peers, connect_ms);

  // Phase 2: Traffic test with active pairs.
  // Active pairs: first g_active_pairs pairs of connected
  // peers. Even-indexed = sender, odd-indexed = receiver.
  uint64_t total_sent = 0, total_recv = 0;
  uint64_t total_bytes_sent = 0, total_bytes_recv = 0;
  uint64_t total_send_errors = 0;
  double traffic_secs = 0;
  int actual_pairs = 0;

  if (connected >= 2) {
    if (g_rate_mbps > 0) {
      fprintf(stderr,
              "=== Phase 2: Traffic (%d active pairs, "
              "%ds, %dB msgs, %.0f Mbps cap) ===\n",
              g_active_pairs, g_duration_sec,
              g_msg_size, g_rate_mbps);
    } else {
      fprintf(stderr,
              "=== Phase 2: Traffic (%d active pairs, "
              "%ds, %dB msgs, unlimited) ===\n",
              g_active_pairs, g_duration_sec,
              g_msg_size);
    }

    // Build active pair list from connected peers.
    auto* args = new TrafficArg[g_active_pairs * 2]();
    auto* threads = new pthread_t[g_active_pairs * 2];

    int pair_idx = 0;
    for (int i = 0; i + 1 < g_num_peers &&
         pair_idx < g_active_pairs; i += 2) {
      if (!g_peers[i].connected ||
          !g_peers[i + 1].connected) {
        continue;
      }
      int si = pair_idx * 2;
      int ri = pair_idx * 2 + 1;
      args[si].sender_idx = i;
      args[si].receiver_idx = i + 1;
      args[si].is_sender = true;
      args[ri].sender_idx = i;
      args[ri].receiver_idx = i + 1;
      args[ri].is_sender = false;
      pair_idx++;
    }
    actual_pairs = pair_idx;

    g_stop.store(0);
    uint64_t t_traffic = NowNs();

    // Use small stacks (256KB) to support many threads.
    // Receiver needs ~64KB for kMaxFramePayload buffer.
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);

    // Start receivers.
    for (int i = 0; i < actual_pairs; i++) {
      pthread_create(&threads[i * 2 + 1], &attr,
                     ReceiverThread, &args[i * 2 + 1]);
    }
    // Start senders.
    for (int i = 0; i < actual_pairs; i++) {
      pthread_create(&threads[i * 2], &attr,
                     SenderThread, &args[i * 2]);
    }
    pthread_attr_destroy(&attr);

    // Wait for senders (they respect duration deadline).
    for (int i = 0; i < actual_pairs; i++) {
      pthread_join(threads[i * 2], nullptr);
    }
    g_stop.store(1);

    // Wait for receivers (they drain then exit).
    for (int i = 0; i < actual_pairs; i++) {
      pthread_join(threads[i * 2 + 1], nullptr);
    }

    traffic_secs = (NowNs() - t_traffic) / 1e9;

    // Aggregate.
    for (int i = 0; i < actual_pairs; i++) {
      auto& s = args[i * 2].result;
      auto& r = args[i * 2 + 1].result;
      total_sent += s.msgs_sent;
      total_recv += r.msgs_recv;
      total_bytes_sent += s.bytes_sent;
      total_bytes_recv += r.bytes_recv;
      total_send_errors += s.send_errors;
    }

    delete[] args;
    delete[] threads;
  }

  double loss_pct = 0;
  if (total_sent > 0) {
    loss_pct =
        100.0 * (1.0 - static_cast<double>(total_recv) /
                           total_sent);
  }
  double throughput_mbps =
      traffic_secs > 0
          ? total_bytes_recv * 8.0 / 1e6 / traffic_secs
          : 0;

  fprintf(stderr,
          "\n=== Results ===\n"
          "Peers:         %d / %d connected\n"
          "Connect time:  %.0f ms\n"
          "Active pairs:  %d\n"
          "Traffic time:  %.1f s\n"
          "Messages sent: %" PRIu64 "\n"
          "Messages recv: %" PRIu64 "\n"
          "Message loss:  %.2f%%\n"
          "Send errors:   %" PRIu64 "\n"
          "Throughput:    %.1f Mbps (recv)\n",
          connected, g_num_peers,
          connect_ms, actual_pairs,
          traffic_secs,
          total_sent, total_recv, loss_pct,
          total_send_errors, throughput_mbps);

  if (g_json) {
    char ts[32];
    NowIso8601(ts, sizeof(ts));
    printf("{\n"
           "  \"timestamp\": \"%s\",\n"
           "  \"relay\": \"hyper-derp\",\n"
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
           ts, g_num_peers, connected, failed,
           connect_ms, actual_pairs,
           g_duration_sec, g_msg_size, g_rate_mbps,
           total_sent, total_recv, loss_pct,
           total_send_errors, throughput_mbps);
  }

  // Cleanup.
  for (int i = 0; i < g_num_peers; i++) {
    if (g_peers[i].connected) {
      ClientClose(&g_peers[i].client);
    }
  }
  delete[] g_peers;
  return 0;
}
