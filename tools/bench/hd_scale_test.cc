/// @file hd_scale_test.cc
/// @brief Multi-peer connection scaling test for HD protocol.
///
/// Connects N HD peers, sets forwarding rules via REST API,
/// then runs traffic between active pairs.  Measures
/// connection capacity, throughput, and message loss.
///
/// Usage:
///   hd-scale-test --relay-key <hex> --peers 20
///     [--active-pairs 10] [--msg-size 1400]
///     [--duration 15] [--rate-mbps 3000] [--json]

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
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "hyper_derp/bench.h"
#include "hyper_derp/handshake.h"
#include "hyper_derp/hd_client.h"
#include "hyper_derp/hd_protocol.h"
#include "hyper_derp/protocol.h"

using hyper_derp::HdClient;
using hyper_derp::HdClientClose;
using hyper_derp::HdClientConnect;
using hyper_derp::HdClientEnroll;
using hyper_derp::HdClientInit;
using hyper_derp::HdClientRecvFrame;
using hyper_derp::HdClientSetTimeout;
using hyper_derp::HdClientTlsConnect;
using hyper_derp::HdClientUpgrade;
using hyper_derp::HdFrameType;
using hyper_derp::HdWriteFrameHeader;
using hyper_derp::Key;
using hyper_derp::KeyToHex;
using hyper_derp::kHdFrameHeaderSize;
using hyper_derp::kHdMaxFramePayload;
using hyper_derp::kKeySize;
using hyper_derp::NowIso8601;
using hyper_derp::NowNs;

// --- Peer and traffic state ---

struct PeerState {
  HdClient client;
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
static const char* g_relay_key_hex = nullptr;
static const char* g_metrics_host = nullptr;
static uint16_t g_metrics_port = 9191;
static int g_num_peers = 20;
static int g_active_pairs = 0;
static int g_msg_size = 1400;
static int g_duration_sec = 15;
static double g_rate_mbps = 3000;
static bool g_json = false;
static bool g_use_tls = false;

// Traffic thread state.
static std::atomic<int> g_stop{0};
static PeerState* g_peers = nullptr;
static Key g_relay_key{};

struct TrafficArg {
  int sender_idx;
  int receiver_idx;
  int pair_id;
  PairResult result;
  bool is_sender;
};

// --- Utility ---

/// Decodes a hex string into a byte array.
static bool HexDecode(const char* hex,
                      uint8_t* out, int len) {
  for (int i = 0; i < len; i++) {
    unsigned byte;
    if (sscanf(hex + i * 2, "%2x", &byte) != 1) {
      return false;
    }
    out[i] = static_cast<uint8_t>(byte);
  }
  return true;
}

// --- HTTP helper for forwarding rule setup ---

/// Sets a forwarding rule via the metrics REST API.
/// Sends POST /api/v1/peers/<sender_hex>/rules with
/// body {"dst_key":"<receiver_hex>"}.
static bool SetForwardingRule(const char* host, int port,
                              const char* sender_hex,
                              const char* receiver_hex) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
    close(fd);
    return false;
  }
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr),
              sizeof(addr)) < 0) {
    close(fd);
    return false;
  }

  // Build JSON body.
  char body[128];
  snprintf(body, sizeof(body),
           "{\"dst_key\":\"%s\"}", receiver_hex);
  int body_len = static_cast<int>(strlen(body));

  // Build HTTP request.
  char req[512];
  int req_len = snprintf(req, sizeof(req),
      "POST /api/v1/peers/%s/rules HTTP/1.1\r\n"
      "Host: %s:%d\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: %d\r\n"
      "Connection: close\r\n"
      "\r\n"
      "%s",
      sender_hex, host, port, body_len, body);

  // Send request.
  int total = 0;
  while (total < req_len) {
    int w = write(fd, req + total, req_len - total);
    if (w <= 0) {
      close(fd);
      return false;
    }
    total += w;
  }

  // Read response (just check for 200).
  char resp[512];
  int n = read(fd, resp, sizeof(resp) - 1);
  close(fd);

  if (n <= 0) return false;
  resp[n] = '\0';
  return strstr(resp, "200") != nullptr;
}

// --- Traffic threads ---

static void* SenderThread(void* arg) {
  auto* ta = static_cast<TrafficArg*>(arg);
  PeerState* s = &g_peers[ta->sender_idx];

  struct timeval tv = {0, 100000};
  setsockopt(s->client.fd, SOL_SOCKET, SO_SNDTIMEO,
             &tv, sizeof(tv));

  // HD Data frame: [4B header][payload] (no dst key).
  int frame_len = kHdFrameHeaderSize + g_msg_size;
  auto* frame = new uint8_t[frame_len];
  HdWriteFrameHeader(frame, HdFrameType::kData,
                     static_cast<uint32_t>(g_msg_size));
  for (int i = 0; i < g_msg_size; i++) {
    frame[kHdFrameHeaderSize + i] =
        static_cast<uint8_t>(i & 0xff);
  }

  int send_sec =
      g_duration_sec > 2 ? g_duration_sec - 1 : 1;
  uint64_t deadline_ns =
      NowNs() +
      static_cast<uint64_t>(send_sec) * 1000000000ULL;

  // Rate limiter: total rate / active pairs.
  double bytes_per_ns = 0;
  if (g_rate_mbps > 0 && g_active_pairs > 0) {
    double per_sender_mbps =
        g_rate_mbps / g_active_pairs;
    bytes_per_ns = per_sender_mbps * 1e6 / 8.0 / 1e9;
  }
  uint64_t send_start = NowNs();
  uint64_t total_bytes_paced = 0;

  while (NowNs() < deadline_ns) {
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
          ts_sleep.tv_nsec = static_cast<long>(wait_ns);
          nanosleep(&ts_sleep, nullptr);
        }
        allowed = static_cast<uint64_t>(
            (NowNs() - send_start) * bytes_per_ns);
        if (NowNs() >= deadline_ns) goto done;
      }
    }

    {
      int total = 0;
      bool ok = true;
      while (total < frame_len) {
        int w;
        if (s->client.ssl) {
          w = SSL_write(s->client.ssl, frame + total,
                        frame_len - total);
          if (w <= 0) {
            ta->result.send_errors++;
            ok = false;
            break;
          }
        } else {
          w = write(s->client.fd, frame + total,
                    frame_len - total);
          if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN ||
                errno == EWOULDBLOCK) {
              ta->result.send_errors++;
              ok = false;
              break;
            }
            ok = false;
            break;
          }
          if (w == 0) {
            ok = false;
            break;
          }
        }
        total += w;
      }
      if (ok) {
        ta->result.msgs_sent++;
        ta->result.bytes_sent += g_msg_size;
        total_bytes_paced += g_msg_size;
      }
    }
  }
done:
  delete[] frame;
  return nullptr;
}

static void* ReceiverThread(void* arg) {
  auto* ta = static_cast<TrafficArg*>(arg);
  PeerState* r = &g_peers[ta->receiver_idx];

  (void)HdClientSetTimeout(&r->client, 100);

  uint8_t buf[kHdMaxFramePayload];
  int buf_len;
  HdFrameType ftype;
  int idle = 0;
  uint64_t drain_deadline = 0;

  while (true) {
    auto result = HdClientRecvFrame(
        &r->client, &ftype, buf, &buf_len, sizeof(buf));
    if (!result) {
      if (g_stop.load()) {
        if (drain_deadline == 0) {
          drain_deadline = NowNs() + 2000000000ULL;
        }
        if (NowNs() >= drain_deadline) break;
        idle++;
        if (idle >= 20) break;
      }
      continue;
    }
    idle = 0;
    // HD Data frames carry payload directly (no src key).
    if (ftype == HdFrameType::kData) {
      if (buf_len > 0) {
        ta->result.bytes_recv += buf_len;
      }
      ta->result.msgs_recv++;
    }
  }

  return nullptr;
}

static bool ConnectPeer(PeerState* p) {
  auto conn =
      HdClientConnect(&p->client, g_host, g_port);
  if (!conn) {
    fprintf(stderr, "    connect: %s\n",
            conn.error().message.c_str());
    return false;
  }
  if (g_use_tls) {
    auto tls = HdClientTlsConnect(&p->client);
    if (!tls) {
      fprintf(stderr, "    tls: %s\n",
              tls.error().message.c_str());
      HdClientClose(&p->client);
      return false;
    }
  }
  auto upgrade = HdClientUpgrade(&p->client);
  if (!upgrade) {
    fprintf(stderr, "    upgrade: %s\n",
            upgrade.error().message.c_str());
    HdClientClose(&p->client);
    return false;
  }
  auto enroll = HdClientEnroll(&p->client);
  if (!enroll) {
    fprintf(stderr, "    enroll: %s\n",
            enroll.error().message.c_str());
    HdClientClose(&p->client);
    return false;
  }
  p->connected = true;
  return true;
}

// --- Main ---

int main(int argc, char** argv) {
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
      g_host = argv[++i];
    } else if (strcmp(argv[i], "--port") == 0 &&
               i + 1 < argc) {
      g_port = static_cast<uint16_t>(atoi(argv[++i]));
    } else if (strcmp(argv[i], "--relay-key") == 0 &&
               i + 1 < argc) {
      g_relay_key_hex = argv[++i];
    } else if (strcmp(argv[i], "--metrics-host") == 0 &&
               i + 1 < argc) {
      g_metrics_host = argv[++i];
    } else if (strcmp(argv[i], "--metrics-port") == 0 &&
               i + 1 < argc) {
      g_metrics_port =
          static_cast<uint16_t>(atoi(argv[++i]));
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
    } else if (strcmp(argv[i], "--tls") == 0) {
      g_use_tls = true;
    } else {
      fprintf(stderr,
          "Usage: hd-scale-test [options]\n"
          "  --relay-key HEX      Relay key (required)\n"
          "  --peers N            Total peers (20)\n"
          "  --active-pairs K     Pairs with traffic\n"
          "  --msg-size N         Message size (1400)\n"
          "  --duration N         Traffic seconds (15)\n"
          "  --rate-mbps F        Aggregate rate (3000)\n"
          "  --host/--port        Relay address\n"
          "  --metrics-host HOST  Metrics API host\n"
          "  --metrics-port PORT  Metrics API port (9191)\n"
          "  --tls                TLS 1.3 connect\n"
          "  --json               JSON output\n");
      return 1;
    }
  }

  if (!g_relay_key_hex) {
    fprintf(stderr,
            "Error: --relay-key is required\n");
    return 1;
  }

  int hlen = static_cast<int>(strlen(g_relay_key_hex));
  if (hlen != kKeySize * 2 ||
      !HexDecode(g_relay_key_hex,
                 g_relay_key.data(), kKeySize)) {
    fprintf(stderr,
            "Invalid --relay-key (need 64 hex chars)\n");
    return 1;
  }

  // Default metrics host to relay host.
  if (!g_metrics_host) {
    g_metrics_host = g_host;
  }

  if (sodium_init() < 0) {
    fprintf(stderr, "sodium_init failed\n");
    return 1;
  }
  signal(SIGPIPE, SIG_IGN);

  if (g_num_peers < 2) {
    fprintf(stderr, "Need at least 2 peers\n");
    return 1;
  }
  int max_pairs = g_num_peers / 2;
  if (g_active_pairs <= 0) {
    g_active_pairs = max_pairs < 10 ? max_pairs : 10;
  }
  if (g_active_pairs > max_pairs) {
    g_active_pairs = max_pairs;
  }

  g_peers = new PeerState[g_num_peers]();
  for (int i = 0; i < g_num_peers; i++) {
    auto r = HdClientInit(&g_peers[i].client);
    if (!r) {
      fprintf(stderr, "HdClientInit failed for peer %d\n",
              i);
      return 1;
    }
    g_peers[i].client.relay_key = g_relay_key;
  }

  // Phase 1: Connect peers.
  fprintf(stderr,
          "=== Phase 1: Connecting %d HD peers ===\n",
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

  // Phase 2: Set forwarding rules via REST API.
  fprintf(stderr,
          "=== Phase 2: Setting forwarding rules ===\n");

  int rules_set = 0;
  int rules_failed = 0;

  for (int i = 0; i < g_active_pairs; i++) {
    int si = i * 2;
    int ri = i * 2 + 1;
    if (si >= g_num_peers || ri >= g_num_peers) break;
    if (!g_peers[si].connected ||
        !g_peers[ri].connected) {
      continue;
    }

    char sender_hex[kKeySize * 2 + 1];
    char receiver_hex[kKeySize * 2 + 1];
    KeyToHex(g_peers[si].client.public_key, sender_hex);
    KeyToHex(g_peers[ri].client.public_key, receiver_hex);

    if (SetForwardingRule(g_metrics_host, g_metrics_port,
                          sender_hex, receiver_hex)) {
      rules_set++;
    } else {
      rules_failed++;
      fprintf(stderr,
              "  rule %d: failed (sender peer %d -> "
              "receiver peer %d)\n", i, si, ri);
    }
  }

  fprintf(stderr, "Rules set: %d, failed: %d\n\n",
          rules_set, rules_failed);

  // Phase 3: Traffic.
  uint64_t total_sent = 0, total_recv = 0;
  uint64_t total_bytes_sent = 0, total_bytes_recv = 0;
  uint64_t total_send_errors = 0;
  double traffic_secs = 0;
  int actual_pairs = 0;
  TrafficArg* args = nullptr;

  if (connected >= 2) {
    if (g_rate_mbps > 0) {
      fprintf(stderr,
              "=== Phase 3: Traffic (%d active pairs, "
              "%ds, %dB msgs, %.0f Mbps cap) ===\n",
              g_active_pairs, g_duration_sec,
              g_msg_size, g_rate_mbps);
    } else {
      fprintf(stderr,
              "=== Phase 3: Traffic (%d active pairs, "
              "%ds, %dB msgs, unlimited) ===\n",
              g_active_pairs, g_duration_sec,
              g_msg_size);
    }

    args = new TrafficArg[g_active_pairs * 2]();
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
      args[si].pair_id = pair_idx;
      args[ri].sender_idx = i;
      args[ri].receiver_idx = i + 1;
      args[ri].is_sender = false;
      args[ri].pair_id = pair_idx;
      pair_idx++;
    }
    actual_pairs = pair_idx;

    g_stop.store(0);
    uint64_t t_traffic = NowNs();

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);

    auto* threads = new pthread_t[actual_pairs * 2];
    memset(threads, 0,
           actual_pairs * 2 * sizeof(pthread_t));

    // Start receivers first.
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

    // Wait for senders.
    for (int i = 0; i < actual_pairs; i++) {
      pthread_join(threads[i * 2], nullptr);
    }
    g_stop.store(1);

    // Wait for receivers.
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

  // Phase 4: Results.
  fprintf(stderr,
          "\n=== Results ===\n"
          "Peers:         %d / %d connected\n"
          "Connect time:  %.0f ms\n"
          "Rules set:     %d\n"
          "Active pairs:  %d\n"
          "Traffic time:  %.1f s\n"
          "Messages sent: %" PRIu64 "\n"
          "Messages recv: %" PRIu64 "\n"
          "Message loss:  %.2f%%\n"
          "Send errors:   %" PRIu64 "\n"
          "Throughput:    %.1f Mbps (recv)\n",
          connected, g_num_peers,
          connect_ms, rules_set,
          actual_pairs,
          traffic_secs,
          total_sent, total_recv, loss_pct,
          total_send_errors, throughput_mbps);

  if (g_json) {
    char ts[32];
    NowIso8601(ts, sizeof(ts));
    fprintf(stdout,
        "{\n"
        "  \"timestamp\": \"%s\",\n"
        "  \"relay\": \"hyper-derp-hd\",\n"
        "  \"total_peers\": %d,\n"
        "  \"connected_peers\": %d,\n"
        "  \"connect_failed\": %d,\n"
        "  \"connect_time_ms\": %.1f,\n"
        "  \"rules_set\": %d,\n"
        "  \"rules_failed\": %d,\n"
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
        connect_ms, rules_set, rules_failed,
        actual_pairs,
        g_duration_sec, g_msg_size, g_rate_mbps,
        total_sent, total_recv, loss_pct,
        total_send_errors, throughput_mbps);
  }

  // Cleanup.
  delete[] args;
  for (int i = 0; i < g_num_peers; i++) {
    if (g_peers[i].connected) {
      HdClientClose(&g_peers[i].client);
    }
  }
  delete[] g_peers;

  return 0;
}
