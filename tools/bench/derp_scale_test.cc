/// @file derp_scale_test.cc
/// @brief Multi-peer connection scaling test for DERP relay.
///
/// Connects N peers, then runs traffic between active pairs.
/// Measures connection capacity, throughput, and message loss.
///
/// Supports distributed mode: multiple instances across VMs,
/// each controlling a subset of peers, synchronized by
/// wall-clock start time.
///
/// Usage (single instance, legacy):
///   derp-scale-test --peers 20 [--active-pairs 10]
///     [--msg-size 1400] [--duration 15] [--json]
///
/// Usage (distributed, 4 instances):
///   derp-scale-test --pair-file pairs.json --instance-id 0
///     --instance-count 4 --start-at 1711800000000
///     --rate-mbps 5000 --duration 15 --tls
///     --json --output /tmp/result.json

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

using hyper_derp::ClientClose;
using hyper_derp::ClientConnect;
using hyper_derp::ClientHandshake;
using hyper_derp::ClientInit;
using hyper_derp::ClientInitWithKeys;
using hyper_derp::ClientRecvFrame;
using hyper_derp::ClientSendPacket;
using hyper_derp::ClientSetTimeout;
using hyper_derp::ClientTlsConnect;
using hyper_derp::ClientUpgrade;
using hyper_derp::DerpClient;
using hyper_derp::FrameType;
using hyper_derp::kFrameHeaderSize;
using hyper_derp::Key;
using hyper_derp::kKeySize;
using hyper_derp::kMaxFramePayload;
using hyper_derp::NowIso8601;
using hyper_derp::NowNs;
using hyper_derp::WriteFrameHeader;

// --- Pair file structures ---

struct PeerDef {
  int id;
  uint8_t pub[32];
  uint8_t priv[32];
};

struct PairDef {
  int sender;
  int receiver;
};

struct PairFile {
  int total_peers;
  int total_pairs;
  int instance_count;
  PeerDef* peers;
  PairDef* pairs;
  // Per-instance peer ID lists.
  int** instance_peer_ids;
  int* instance_peer_counts;
};

// --- Peer and traffic state ---

struct PeerState {
  DerpClient client;
  bool connected;
  int peer_id;  // ID from pair file (-1 if legacy mode).
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
static double g_rate_mbps = 100;
static bool g_json = false;
static bool g_use_tls = false;

// Distributed mode.
static const char* g_pair_file = nullptr;
static int g_instance_id = -1;
static int g_instance_count = 1;
static int64_t g_start_at_ms = 0;
static const char* g_output_file = nullptr;
static const char* g_run_id = "";

// Traffic thread state.
static std::atomic<int> g_stop{0};
static PeerState* g_peers = nullptr;
static PairFile g_pf = {};

struct TrafficArg {
  int sender_idx;    // Index into g_peers[].
  int receiver_idx;  // Index into g_peers[].
  int pair_id;       // Pair file pair index.
  PairResult result;
  bool is_sender;
};

// --- Minimal JSON pair file parser ---

/// Reads a hex string into a byte array. Returns false on error.
static bool HexDecode(const char* hex, uint8_t* out, int len) {
  for (int i = 0; i < len; i++) {
    unsigned byte;
    if (sscanf(hex + i * 2, "%2x", &byte) != 1) return false;
    out[i] = static_cast<uint8_t>(byte);
  }
  return true;
}

/// Finds the next occurrence of a JSON key in the string.
static const char* FindKey(const char* json,
                           const char* key) {
  char pattern[128];
  snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  return strstr(json, pattern);
}

/// Extracts an integer value after "key": .
static bool ReadInt(const char* json, const char* key,
                    int* out) {
  const char* p = FindKey(json, key);
  if (!p) return false;
  p = strchr(p, ':');
  if (!p) return false;
  *out = atoi(p + 1);
  return true;
}

/// Extracts a quoted string value after "key": "...".
static bool ReadStr(const char* json, const char* key,
                    char* out, int out_size) {
  const char* p = FindKey(json, key);
  if (!p) return false;
  p = strchr(p, ':');
  if (!p) return false;
  p = strchr(p, '"');
  if (!p) return false;
  p++;
  const char* end = strchr(p, '"');
  if (!end) return false;
  int len = static_cast<int>(end - p);
  if (len >= out_size) return false;
  memcpy(out, p, len);
  out[len] = '\0';
  return true;
}

/// Parses the pair file JSON into g_pf. Minimal parser —
/// relies on the known structure from gen_pairs.py.
static bool LoadPairFile(const char* path) {
  FILE* f = fopen(path, "r");
  if (!f) {
    fprintf(stderr, "Cannot open pair file: %s\n", path);
    return false;
  }

  // Read entire file.
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  char* buf = new char[sz + 1];
  fread(buf, 1, sz, f);
  buf[sz] = '\0';
  fclose(f);

  // Read top-level fields.
  if (!ReadInt(buf, "total_peers", &g_pf.total_peers) ||
      !ReadInt(buf, "total_pairs", &g_pf.total_pairs) ||
      !ReadInt(buf, "instance_count", &g_pf.instance_count)) {
    fprintf(stderr, "Pair file missing required fields\n");
    delete[] buf;
    return false;
  }

  // Parse peers array.
  g_pf.peers = new PeerDef[g_pf.total_peers]();
  const char* peers_start = FindKey(buf, "peers");
  if (!peers_start) {
    fprintf(stderr, "Pair file missing \"peers\" array\n");
    delete[] buf;
    return false;
  }

  // Walk through each peer object.
  const char* cursor = peers_start;
  for (int i = 0; i < g_pf.total_peers; i++) {
    // Find next "id" after current position.
    cursor = FindKey(cursor + 1, "id");
    if (!cursor) {
      fprintf(stderr, "Pair file: missing peer %d\n", i);
      delete[] buf;
      return false;
    }

    int id;
    const char* colon = strchr(cursor, ':');
    if (!colon) { delete[] buf; return false; }
    id = atoi(colon + 1);

    char hex[65];
    // Find "pub" after current "id".
    const char* pub_pos = FindKey(cursor, "pub");
    if (!pub_pos || !ReadStr(pub_pos - 1, "pub", hex, 65)) {
      fprintf(stderr, "Pair file: bad pub for peer %d\n", id);
      delete[] buf;
      return false;
    }
    if (!HexDecode(hex, g_pf.peers[id].pub, 32)) {
      fprintf(stderr, "Pair file: bad pub hex for peer %d\n",
              id);
      delete[] buf;
      return false;
    }

    const char* priv_pos = FindKey(pub_pos, "priv");
    if (!priv_pos ||
        !ReadStr(priv_pos - 1, "priv", hex, 65)) {
      fprintf(stderr, "Pair file: bad priv for peer %d\n",
              id);
      delete[] buf;
      return false;
    }
    if (!HexDecode(hex, g_pf.peers[id].priv, 32)) {
      fprintf(stderr,
              "Pair file: bad priv hex for peer %d\n", id);
      delete[] buf;
      return false;
    }
    g_pf.peers[id].id = id;
    cursor = priv_pos;
  }

  // Parse pairs array.
  g_pf.pairs = new PairDef[g_pf.total_pairs]();
  const char* pairs_start = strstr(cursor, "\"pairs\"");
  if (!pairs_start) {
    fprintf(stderr, "Pair file missing \"pairs\" array\n");
    delete[] buf;
    return false;
  }

  cursor = pairs_start;
  for (int i = 0; i < g_pf.total_pairs; i++) {
    cursor = FindKey(cursor + 1, "sender");
    if (!cursor) {
      fprintf(stderr, "Pair file: missing pair %d\n", i);
      delete[] buf;
      return false;
    }
    const char* sc = strchr(cursor, ':');
    if (!sc) { delete[] buf; return false; }
    g_pf.pairs[i].sender = atoi(sc + 1);

    const char* rc = FindKey(cursor, "receiver");
    if (!rc) { delete[] buf; return false; }
    const char* rc2 = strchr(rc, ':');
    if (!rc2) { delete[] buf; return false; }
    g_pf.pairs[i].receiver = atoi(rc2 + 1);
    cursor = rc;
  }

  // Parse instances array.
  g_pf.instance_peer_ids = new int*[g_pf.instance_count];
  g_pf.instance_peer_counts = new int[g_pf.instance_count]();

  const char* inst_start = strstr(cursor, "\"instances\"");
  if (!inst_start) {
    fprintf(stderr,
            "Pair file missing \"instances\" array\n");
    delete[] buf;
    return false;
  }

  cursor = inst_start;
  for (int inst = 0; inst < g_pf.instance_count; inst++) {
    cursor = FindKey(cursor + 1, "peer_ids");
    if (!cursor) {
      fprintf(stderr,
              "Pair file: missing instance %d\n", inst);
      delete[] buf;
      return false;
    }

    // Find the array brackets.
    const char* arr_start = strchr(cursor, '[');
    const char* arr_end = strchr(cursor, ']');
    if (!arr_start || !arr_end) {
      delete[] buf;
      return false;
    }

    // Count and parse integers.
    int ids[256];
    int count = 0;
    const char* p = arr_start + 1;
    while (p < arr_end && count < 256) {
      while (p < arr_end && (*p == ' ' || *p == ','))
        p++;
      if (p >= arr_end) break;
      ids[count++] = atoi(p);
      while (p < arr_end && *p != ',' && *p != ']')
        p++;
    }

    g_pf.instance_peer_counts[inst] = count;
    g_pf.instance_peer_ids[inst] = new int[count];
    memcpy(g_pf.instance_peer_ids[inst], ids,
           count * sizeof(int));
    cursor = arr_end;
  }

  delete[] buf;
  return true;
}

// --- Utility ---

/// Returns wall-clock time in milliseconds since epoch.
static int64_t WallClockMs() {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000 +
         ts.tv_nsec / 1000000;
}

// --- Traffic threads (unchanged logic) ---

static void* SenderThread(void* arg) {
  auto* ta = static_cast<TrafficArg*>(arg);
  PeerState* s = &g_peers[ta->sender_idx];
  PeerState* r = &g_peers[ta->receiver_idx];

  struct timeval tv = {0, 100000};
  setsockopt(s->client.fd, SOL_SOCKET, SO_SNDTIMEO,
             &tv, sizeof(tv));

  int payload_len = kKeySize + g_msg_size;
  int frame_len = kFrameHeaderSize + payload_len;
  auto* frame = new uint8_t[frame_len];
  WriteFrameHeader(frame, FrameType::kSendPacket,
                   static_cast<uint32_t>(payload_len));
  std::memcpy(frame + kFrameHeaderSize,
              r->client.public_key.data(), kKeySize);
  for (int i = 0; i < g_msg_size; i++) {
    frame[kFrameHeaderSize + kKeySize + i] =
        static_cast<uint8_t>(i & 0xff);
  }

  int send_sec =
      g_duration_sec > 2 ? g_duration_sec - 1 : 1;
  uint64_t deadline_ns =
      NowNs() +
      static_cast<uint64_t>(send_sec) * 1000000000ULL;

  // Rate limiter: total rate / total pairs (not per-instance).
  double bytes_per_ns = 0;
  int total_pairs = g_pf.total_pairs > 0
      ? g_pf.total_pairs : g_active_pairs;
  if (g_rate_mbps > 0 && total_pairs > 0) {
    double per_sender_mbps = g_rate_mbps / total_pairs;
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
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
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

  (void)ClientSetTimeout(&r->client, 100);

  uint8_t buf[kMaxFramePayload];
  int buf_len;
  FrameType ftype;
  int idle = 0;
  uint64_t drain_deadline = 0;

  while (true) {
    auto result = ClientRecvFrame(
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
  auto conn = ClientConnect(&p->client, g_host, g_port);
  if (!conn) {
    fprintf(stderr, "    connect: %s\n",
            conn.error().message.c_str());
    return false;
  }
  if (g_use_tls) {
    auto tls = ClientTlsConnect(&p->client);
    if (!tls) {
      fprintf(stderr, "    tls: %s\n",
              tls.error().message.c_str());
      ClientClose(&p->client);
      return false;
    }
  }
  auto upgrade = ClientUpgrade(&p->client);
  if (!upgrade) {
    fprintf(stderr, "    upgrade: %s\n",
            upgrade.error().message.c_str());
    ClientClose(&p->client);
    return false;
  }
  auto hs = ClientHandshake(&p->client);
  if (!hs) {
    fprintf(stderr, "    handshake: %s\n",
            hs.error().message.c_str());
    ClientClose(&p->client);
    return false;
  }
  p->connected = true;
  return true;
}

// --- JSON output ---

/// Writes distributed-mode JSON output to a file or stdout.
static void WriteDistributedJson(
    FILE* out,
    int connected, int failed, double connect_ms,
    int actual_pairs,
    double traffic_secs,
    TrafficArg* args,
    uint64_t total_sent, uint64_t total_recv,
    uint64_t total_send_errors, double throughput_mbps,
    double loss_pct) {
  char ts[32];
  NowIso8601(ts, sizeof(ts));

  fprintf(out,
    "{\n"
    "  \"instance_id\": %d,\n"
    "  \"instance_count\": %d,\n"
    "  \"run_id\": \"%s\",\n"
    "  \"timestamp\": \"%s\",\n"
    "  \"relay\": \"hyper-derp\",\n"
    "  \"total_peers\": %d,\n"
    "  \"instance_peers\": %d,\n"
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
    "  \"throughput_mbps\": %.1f,\n",
    g_instance_id, g_instance_count, g_run_id, ts,
    g_pf.total_peers, g_num_peers,
    connected, failed, connect_ms,
    actual_pairs, g_duration_sec, g_msg_size,
    g_rate_mbps,
    total_sent, total_recv, loss_pct,
    total_send_errors, throughput_mbps);

  // Per-pair breakdown.
  fprintf(out, "  \"per_pair\": [\n");
  for (int i = 0; i < actual_pairs; i++) {
    auto& s = args[i * 2].result;
    auto& r = args[i * 2 + 1].result;
    double pair_loss = s.msgs_sent > 0
        ? 100.0 * (1.0 - (double)r.msgs_recv / s.msgs_sent)
        : 0;
    double pair_tp = traffic_secs > 0
        ? r.bytes_recv * 8.0 / 1e6 / traffic_secs : 0;
    fprintf(out,
      "    {\"pair_id\": %d, \"sender_id\": %d, "
      "\"receiver_id\": %d, "
      "\"messages_sent\": %" PRIu64 ", "
      "\"messages_recv\": %" PRIu64 ", "
      "\"throughput_mbps\": %.1f, "
      "\"loss_pct\": %.4f}%s\n",
      args[i * 2].pair_id,
      g_peers[args[i * 2].sender_idx].peer_id,
      g_peers[args[i * 2 + 1].receiver_idx].peer_id,
      s.msgs_sent, r.msgs_recv, pair_tp, pair_loss,
      (i + 1 < actual_pairs) ? "," : "");
  }
  fprintf(out, "  ],\n");
  fprintf(out, "  \"latency_ns\": null\n");
  fprintf(out, "}\n");
}

// --- Main ---

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
    } else if (strcmp(argv[i], "--tls") == 0) {
      g_use_tls = true;
    } else if (strcmp(argv[i], "--insecure") == 0) {
      // No-op.
    } else if (strcmp(argv[i], "--pair-file") == 0 &&
               i + 1 < argc) {
      g_pair_file = argv[++i];
    } else if (strcmp(argv[i], "--instance-id") == 0 &&
               i + 1 < argc) {
      g_instance_id = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--instance-count") == 0 &&
               i + 1 < argc) {
      g_instance_count = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--start-at") == 0 &&
               i + 1 < argc) {
      g_start_at_ms = atoll(argv[++i]);
    } else if (strcmp(argv[i], "--output") == 0 &&
               i + 1 < argc) {
      g_output_file = argv[++i];
    } else if (strcmp(argv[i], "--run-id") == 0 &&
               i + 1 < argc) {
      g_run_id = argv[++i];
    } else {
      fprintf(stderr,
          "Usage: derp-scale-test [options]\n"
          "  --peers N             Total peers (20)\n"
          "  --active-pairs K      Pairs with traffic\n"
          "  --msg-size N          Message size (1024)\n"
          "  --duration N          Traffic seconds (5)\n"
          "  --rate-mbps F         Aggregate rate (100)\n"
          "  --host/--port         Relay address\n"
          "  --tls                 TLS 1.3 (kTLS)\n"
          "  --json                JSON output\n"
          "  --pair-file PATH      Distributed mode\n"
          "  --instance-id N       This instance (0-N)\n"
          "  --instance-count N    Total instances\n"
          "  --start-at EPOCH_MS   Sync start time\n"
          "  --output FILE         JSON output file\n"
          "  --run-id STRING       Run identifier\n");
      return 1;
    }
  }

  if (sodium_init() < 0) {
    fprintf(stderr, "sodium_init failed\n");
    return 1;
  }
  signal(SIGPIPE, SIG_IGN);

  // --- Distributed mode: load pair file ---
  bool distributed = g_pair_file != nullptr;

  if (distributed) {
    if (g_instance_id < 0) {
      fprintf(stderr,
              "--instance-id required with --pair-file\n");
      return 1;
    }
    if (!LoadPairFile(g_pair_file)) {
      return 1;
    }
    if (g_instance_id >= g_pf.instance_count) {
      fprintf(stderr,
              "instance-id %d >= instance_count %d\n",
              g_instance_id, g_pf.instance_count);
      return 1;
    }

    // This instance's peers.
    int my_count =
        g_pf.instance_peer_counts[g_instance_id];
    int* my_ids = g_pf.instance_peer_ids[g_instance_id];

    g_num_peers = my_count;
    g_peers = new PeerState[my_count]();

    for (int i = 0; i < my_count; i++) {
      int pid = my_ids[i];
      ClientInitWithKeys(&g_peers[i].client,
                         g_pf.peers[pid].pub,
                         g_pf.peers[pid].priv);
      g_peers[i].peer_id = pid;
    }

    fprintf(stderr,
            "Distributed mode: instance %d/%d, "
            "%d peers\n",
            g_instance_id, g_instance_count, my_count);
  } else {
    // Legacy mode.
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
      (void)ClientInit(&g_peers[i].client);
      g_peers[i].peer_id = -1;
    }
  }

  // Phase 1: Connect peers.
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
        fprintf(stderr, "  peer %d: connect failed\n",
                distributed ? g_peers[i].peer_id : i);
      } else if (failed == 6) {
        fprintf(stderr, "  ... (suppressing further)\n");
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

  // --- Wait for synchronized start ---
  if (g_start_at_ms > 0) {
    int64_t now_ms = WallClockMs();
    int64_t wait_ms = g_start_at_ms - now_ms;
    if (wait_ms > 0) {
      fprintf(stderr,
              "Waiting %.1f s for synchronized start...\n",
              wait_ms / 1000.0);
      struct timespec ts_wait;
      ts_wait.tv_sec = wait_ms / 1000;
      ts_wait.tv_nsec = (wait_ms % 1000) * 1000000L;
      nanosleep(&ts_wait, nullptr);
    } else {
      fprintf(stderr,
              "WARNING: start_at is %.1f s in the past\n",
              -wait_ms / 1000.0);
    }
  }

  // Phase 2: Traffic.
  uint64_t total_sent = 0, total_recv = 0;
  uint64_t total_bytes_sent = 0, total_bytes_recv = 0;
  uint64_t total_send_errors = 0;
  double traffic_secs = 0;
  int actual_pairs = 0;
  TrafficArg* args = nullptr;

  if (connected >= 2) {
    // Build pair list.
    if (distributed) {
      // Find pairs where this instance controls the sender.
      // Build a local peer_id -> local index map.
      int my_count =
          g_pf.instance_peer_counts[g_instance_id];
      int* my_ids = g_pf.instance_peer_ids[g_instance_id];

      // Map: global peer_id -> local g_peers[] index.
      // Use a simple scan (peers count is small).
      auto local_idx = [&](int peer_id) -> int {
        for (int i = 0; i < my_count; i++) {
          if (my_ids[i] == peer_id) return i;
        }
        return -1;
      };

      // Count pairs this instance participates in.
      int my_pairs = 0;
      for (int p = 0; p < g_pf.total_pairs; p++) {
        int si = local_idx(g_pf.pairs[p].sender);
        int ri = local_idx(g_pf.pairs[p].receiver);
        // This instance has the sender, receiver, or both.
        if (si >= 0 || ri >= 0) my_pairs++;
      }

      // Allocate args: 2 per pair (sender + receiver).
      args = new TrafficArg[my_pairs * 2]();

      int pair_idx = 0;
      for (int p = 0; p < g_pf.total_pairs; p++) {
        int si = local_idx(g_pf.pairs[p].sender);
        int ri = local_idx(g_pf.pairs[p].receiver);

        if (si < 0 && ri < 0) continue;

        int s_slot = pair_idx * 2;
        int r_slot = pair_idx * 2 + 1;

        args[s_slot].pair_id = p;
        args[r_slot].pair_id = p;
        args[s_slot].is_sender = true;
        args[r_slot].is_sender = false;

        if (si >= 0 && g_peers[si].connected) {
          args[s_slot].sender_idx = si;
        } else {
          args[s_slot].sender_idx = -1;
        }

        if (ri >= 0 && g_peers[ri].connected) {
          args[r_slot].receiver_idx = ri;
        } else {
          args[r_slot].receiver_idx = -1;
        }

        // Cross-reference: sender needs receiver key,
        // receiver doesn't need sender.
        if (si >= 0 && ri >= 0) {
          args[s_slot].receiver_idx = ri;
          args[r_slot].sender_idx = si;
        } else if (si >= 0) {
          // Receiver is on another instance. Sender still
          // needs the receiver's public key for the DERP
          // SendPacket frame. Create a temporary peer with
          // only the public key populated.
          int rpid = g_pf.pairs[p].receiver;
          // We need a stable PeerState for the receiver key.
          // Use a static extra array (allocated once below).
          args[s_slot].receiver_idx = -1;
          // Store receiver peer_id for frame construction.
          // We'll handle this specially in the sender.
          (void)rpid;
        }

        pair_idx++;
      }
      actual_pairs = pair_idx;
      g_active_pairs = actual_pairs;

      fprintf(stderr,
              "=== Phase 2: Traffic (%d local pairs, "
              "%ds, %dB msgs, %.0f Mbps total) ===\n",
              actual_pairs, g_duration_sec,
              g_msg_size, g_rate_mbps);

      // For distributed mode, senders that target receivers
      // on other instances need the receiver's public key.
      // Pre-build the frame with the receiver's key from
      // the pair file. Override the sender thread to use
      // pair file keys when receiver_idx == -1.
      // We solve this by creating stub PeerState entries
      // for remote receivers with only the public key set.
      // Append them to a separate array.
      int remote_count = 0;
      for (int i = 0; i < actual_pairs; i++) {
        if (args[i * 2].sender_idx >= 0 &&
            args[i * 2].receiver_idx < 0) {
          remote_count++;
        }
      }

      if (remote_count > 0) {
        // Grow g_peers to include stub entries for remote
        // receivers (sender needs their public key for the
        // DERP SendPacket frame destination field).
        int new_total = g_num_peers + remote_count;
        auto* new_peers = new PeerState[new_total]();
        // Move-assign to avoid memcpy of non-trivial types.
        for (int j = 0; j < g_num_peers; j++) {
          new_peers[j].client = g_peers[j].client;
          new_peers[j].connected = g_peers[j].connected;
          new_peers[j].peer_id = g_peers[j].peer_id;
          // Zero the old entry so ClientClose won't
          // double-close the fd.
          g_peers[j].client.fd = -1;
          g_peers[j].client.ssl = nullptr;
          g_peers[j].client.ssl_ctx = nullptr;
          g_peers[j].connected = false;
        }
        delete[] g_peers;
        g_peers = new_peers;

        int ri_slot = g_num_peers;
        for (int i = 0; i < actual_pairs; i++) {
          if (args[i * 2].sender_idx >= 0 &&
              args[i * 2].receiver_idx < 0) {
            int rpid = g_pf.pairs[args[i * 2].pair_id]
                           .receiver;
            ClientInitWithKeys(
                &g_peers[ri_slot].client,
                g_pf.peers[rpid].pub,
                g_pf.peers[rpid].priv);
            g_peers[ri_slot].peer_id = rpid;
            g_peers[ri_slot].connected = false;
            args[i * 2].receiver_idx = ri_slot;
            ri_slot++;
          }
        }
        g_num_peers = new_total;
      }

      // Similarly for remote senders (receiver-only pairs).
      for (int i = 0; i < actual_pairs; i++) {
        if (args[i * 2 + 1].receiver_idx >= 0 &&
            args[i * 2].sender_idx < 0) {
          // This instance only has the receiver.
          // Sender is on another instance. No sender thread
          // needed — just a receiver thread.
          args[i * 2 + 1].sender_idx = -1;
        }
      }

    } else {
      // Legacy pair assignment.
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
    }

    g_stop.store(0);
    uint64_t t_traffic = NowNs();

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);

    auto* threads = new pthread_t[actual_pairs * 2];
    memset(threads, 0, actual_pairs * 2 * sizeof(pthread_t));

    // Start receivers (only if this instance has them).
    for (int i = 0; i < actual_pairs; i++) {
      if (args[i * 2 + 1].receiver_idx >= 0 &&
          g_peers[args[i * 2 + 1].receiver_idx].connected) {
        pthread_create(&threads[i * 2 + 1], &attr,
                       ReceiverThread, &args[i * 2 + 1]);
      }
    }
    // Start senders (only if this instance has them).
    for (int i = 0; i < actual_pairs; i++) {
      if (args[i * 2].sender_idx >= 0 &&
          g_peers[args[i * 2].sender_idx].connected) {
        pthread_create(&threads[i * 2], &attr,
                       SenderThread, &args[i * 2]);
      }
    }
    pthread_attr_destroy(&attr);

    // Wait for senders.
    for (int i = 0; i < actual_pairs; i++) {
      if (threads[i * 2]) {
        pthread_join(threads[i * 2], nullptr);
      }
    }
    g_stop.store(1);

    // Wait for receivers.
    for (int i = 0; i < actual_pairs; i++) {
      if (threads[i * 2 + 1]) {
        pthread_join(threads[i * 2 + 1], nullptr);
      }
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

  // --- Output ---
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
    FILE* out = stdout;
    if (g_output_file) {
      out = fopen(g_output_file, "w");
      if (!out) {
        fprintf(stderr,
                "Cannot open output file: %s\n",
                g_output_file);
        out = stdout;
      }
    }

    if (distributed) {
      WriteDistributedJson(
          out, connected, failed, connect_ms,
          actual_pairs, traffic_secs, args,
          total_sent, total_recv, total_send_errors,
          throughput_mbps, loss_pct);
    } else {
      char ts[32];
      NowIso8601(ts, sizeof(ts));
      fprintf(out,
          "{\n"
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

    if (g_output_file && out != stdout) {
      fclose(out);
    }
  }

  // Cleanup.
  delete[] args;
  for (int i = 0; i < g_num_peers; i++) {
    if (g_peers[i].connected) {
      ClientClose(&g_peers[i].client);
    }
  }
  delete[] g_peers;

  if (g_pf.peers) delete[] g_pf.peers;
  if (g_pf.pairs) delete[] g_pf.pairs;
  if (g_pf.instance_peer_ids) {
    for (int i = 0; i < g_pf.instance_count; i++) {
      delete[] g_pf.instance_peer_ids[i];
    }
    delete[] g_pf.instance_peer_ids;
  }
  delete[] g_pf.instance_peer_counts;

  return 0;
}
