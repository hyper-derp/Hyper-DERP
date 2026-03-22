/// @file derp_test_client.cc
/// @brief Standalone DERP test client for integration and
///   bare metal testing with benchmark instrumentation.
///
/// Modes:
///   send  — Send N packets to --dst-key, report throughput.
///   recv  — Receive N RecvPacket frames, report throughput.
///   echo  — Echo received packets back to sender.
///   ping  — Send packets to echo peer, measure RTT.
///
/// Prints own public key (hex) to stderr on startup.
/// With --json, writes structured results to stdout.

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sched.h>
#include <signal.h>
#include <sodium.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "hyper_derp/bench.h"
#include "hyper_derp/client.h"
#include "hyper_derp/handshake.h"
#include "hyper_derp/protocol.h"

using hyper_derp::BenchResult;
using hyper_derp::ClientClose;
using hyper_derp::ClientConnect;
using hyper_derp::ClientHandshake;
using hyper_derp::ClientInit;
using hyper_derp::ClientRecvFrame;
using hyper_derp::ClientSendPacket;
using hyper_derp::ClientSetTimeout;
using hyper_derp::ClientTlsConnect;
using hyper_derp::ClientUpgrade;
using hyper_derp::DerpClient;
using hyper_derp::FrameType;
using hyper_derp::Key;
using hyper_derp::KeyToHex;
using hyper_derp::kKeySize;
using hyper_derp::kMaxFramePayload;
using hyper_derp::LatencyRecorder;
using hyper_derp::NowNs;
using hyper_derp::ToKey;
using hyper_derp::WriteBenchJson;

static void Usage() {
  fprintf(stderr,
      "Usage: derp-test-client [options]\n"
      "\n"
      "Modes:\n"
      "  --mode send     Send N packets to --dst-key\n"
      "  --mode recv     Receive N packets\n"
      "  --mode echo     Echo received packets back\n"
      "  --mode ping     RTT measurement (needs echo peer)\n"
      "  --mode flood    Sustained send for --duration sec\n"
      "  --mode sink     Sustained recv for --duration sec\n"
      "\n"
      "Options:\n"
      "  --host HOST     Server host (default: 127.0.0.1)\n"
      "  --port PORT     Server port (default: 3340)\n"
      "  --dst-key HEX   Destination key (64 hex chars)\n"
      "  --count N       Number of packets (default: 1)\n"
      "  --size N        Payload size bytes (default: 64)\n"
      "  --duration N    Test duration seconds (flood/sink)\n"
      "  --warmup N      Discard first N from stats (0)\n"
      "  --tls           TLS 1.3 connect (kTLS offload)\n"
      "  --core N        Pin to CPU core\n"
      "  --timeout MS    Recv timeout ms (default: 5000)\n"
      "  --ready-fd N    Write 'R' to this fd when ready\n"
      "  --json          JSON results to stdout\n"
      "  --raw-latency   Include raw samples in JSON\n"
      "  --output FILE   Write JSON to file instead of "
      "stdout\n"
      "  --label NAME    Test label for JSON output\n"
      "  --workers N     Relay worker count (metadata)\n"
      "\n"
      "Key exchange:\n"
      "  Prints own pubkey (hex) to stderr.\n"
      "  In send/ping/flood mode without --dst-key, reads\n"
      "  peer key from stdin.\n");
}

static int HexDecode(const char* hex,
                     uint8_t* out, int len) {
  for (int i = 0; i < len; i++) {
    unsigned int byte;
    if (sscanf(hex + i * 2, "%2x", &byte) != 1) {
      return -1;
    }
    out[i] = static_cast<uint8_t>(byte);
  }
  return 0;
}

static int ReadDstKey(const char* mode,
                      const char* dst_key_hex,
                      uint8_t* dst_key) {
  if (dst_key_hex) {
    int hlen = static_cast<int>(strlen(dst_key_hex));
    if (hlen != kKeySize * 2 ||
        HexDecode(dst_key_hex, dst_key, kKeySize) != 0) {
      fprintf(stderr, "Invalid --dst-key\n");
      return -1;
    }
    return 0;
  }
  if (strcmp(mode, "send") == 0 ||
      strcmp(mode, "ping") == 0 ||
      strcmp(mode, "flood") == 0) {
    char line[128];
    if (!fgets(line, sizeof(line), stdin)) {
      fprintf(stderr, "Failed to read peer key\n");
      return -1;
    }
    int len = static_cast<int>(strlen(line));
    while (len > 0 && (line[len - 1] == '\n' ||
                       line[len - 1] == '\r')) {
      line[--len] = '\0';
    }
    if (len != kKeySize * 2 ||
        HexDecode(line, dst_key, kKeySize) != 0) {
      fprintf(stderr, "Invalid peer key from stdin\n");
      return -1;
    }
    return 0;
  }
  return 0;
}

static int RunSend(DerpClient* client,
                   const Key& dst_key,
                   int count, int size, int warmup,
                   BenchResult* result) {
  auto* payload = new uint8_t[size];
  for (int i = 0; i < size; i++) {
    payload[i] = static_cast<uint8_t>(i & 0xff);
  }

  // Warmup phase.
  for (int i = 0; i < warmup; i++) {
    if (size >= 4) {
      payload[0] = static_cast<uint8_t>(i >> 24);
      payload[1] = static_cast<uint8_t>(i >> 16);
      payload[2] = static_cast<uint8_t>(i >> 8);
      payload[3] = static_cast<uint8_t>(i);
    }
    if (!ClientSendPacket(client, dst_key,
                          payload, size)) {
      fprintf(stderr, "Warmup send failed at %d\n", i);
      delete[] payload;
      return -1;
    }
  }

  uint64_t t0 = NowNs();
  int sent = 0;

  for (int i = 0; i < count; i++) {
    if (size >= 4) {
      int seq = warmup + i;
      payload[0] = static_cast<uint8_t>(seq >> 24);
      payload[1] = static_cast<uint8_t>(seq >> 16);
      payload[2] = static_cast<uint8_t>(seq >> 8);
      payload[3] = static_cast<uint8_t>(seq);
    }
    if (!ClientSendPacket(client, dst_key,
                          payload, size)) {
      fprintf(stderr, "Send failed at packet %d\n", i);
      break;
    }
    sent++;
  }

  uint64_t t1 = NowNs();
  delete[] payload;

  result->elapsed_ns = t1 - t0;
  result->packets_completed = sent;
  result->bytes_total =
      static_cast<uint64_t>(sent) * size;

  fprintf(stderr, "Sent %d packets (%d bytes each) in "
          "%.3f ms\n", sent, size,
          (t1 - t0) / 1e6);
  return 0;
}

static int RunRecv(DerpClient* client,
                   int count, int warmup,
                   BenchResult* result) {
  uint8_t buf[kMaxFramePayload];
  int buf_len;
  FrameType ftype;

  // Warmup phase.
  for (int i = 0; i < warmup; i++) {
    if (!ClientRecvFrame(client, &ftype, buf,
                         &buf_len, sizeof(buf))) {
      fprintf(stderr, "Warmup recv failed at %d\n", i);
      return -1;
    }
  }

  uint64_t t0 = NowNs();
  int received = 0;
  uint64_t total_bytes = 0;

  while (received < count) {
    if (!ClientRecvFrame(client, &ftype, buf,
                         &buf_len, sizeof(buf))) {
      fprintf(stderr, "Recv failed after %d packets\n",
              received);
      break;
    }
    if (ftype == FrameType::kRecvPacket) {
      int data_len = buf_len - kKeySize;
      if (data_len > 0) {
        total_bytes += data_len;
      }
      received++;
    }
  }

  uint64_t t1 = NowNs();

  result->elapsed_ns = t1 - t0;
  result->packets_completed = received;
  result->bytes_total = total_bytes;

  fprintf(stderr, "Received %d packets in %.3f ms\n",
          received, (t1 - t0) / 1e6);
  return 0;
}

static int RunEcho(DerpClient* client, int count) {
  uint8_t buf[kMaxFramePayload];
  int buf_len;
  FrameType ftype;
  int echoed = 0;

  while (echoed < count) {
    if (!ClientRecvFrame(client, &ftype, buf,
                         &buf_len, sizeof(buf))) {
      break;
    }
    if (ftype == FrameType::kRecvPacket &&
        buf_len > kKeySize) {
      if (!ClientSendPacket(client, ToKey(buf),
                            buf + kKeySize,
                            buf_len - kKeySize)) {
        break;
      }
      echoed++;
    }
  }

  fprintf(stderr, "Echoed %d packets\n", echoed);
  return 0;
}

// -- Duration-based throughput modes -------------------------

static int RunFlood(DerpClient* client,
                    const Key& dst_key,
                    int size, int duration_sec,
                    BenchResult* result) {
  auto* payload = new uint8_t[size];
  for (int i = 0; i < size; i++) {
    payload[i] = static_cast<uint8_t>(i & 0xff);
  }

  uint64_t deadline_ns =
      NowNs() +
      static_cast<uint64_t>(duration_sec) * 1000000000ULL;
  uint64_t t0 = NowNs();
  int sent = 0;

  while (NowNs() < deadline_ns) {
    if (size >= 4) {
      payload[0] = static_cast<uint8_t>(sent >> 24);
      payload[1] = static_cast<uint8_t>(sent >> 16);
      payload[2] = static_cast<uint8_t>(sent >> 8);
      payload[3] = static_cast<uint8_t>(sent);
    }
    if (!ClientSendPacket(client, dst_key,
                          payload, size)) {
      fprintf(stderr, "Flood send failed at %d\n", sent);
      break;
    }
    sent++;
  }

  uint64_t t1 = NowNs();
  delete[] payload;

  // Graceful drain: shut down write side and block until
  // the relay processes all buffered data and closes.
  shutdown(client->fd, SHUT_WR);
  struct timeval tv;
  tv.tv_sec = 10;
  tv.tv_usec = 0;
  setsockopt(client->fd, SOL_SOCKET, SO_RCVTIMEO,
             &tv, sizeof(tv));
  uint8_t drain[4096];
  while (read(client->fd, drain, sizeof(drain)) > 0) {}
  // Wait for relay to forward all queued sends.
  sleep(5);

  result->elapsed_ns = t1 - t0;
  result->packets_completed = sent;
  result->bytes_total =
      static_cast<uint64_t>(sent) * size;

  double secs = (t1 - t0) / 1e9;
  double pps = sent / secs;
  double mbps = result->bytes_total * 8.0 / 1e6 / secs;
  fprintf(stderr,
          "Flood: %d packets in %.3f s "
          "(%.0f PPS, %.1f Mbps)\n",
          sent, secs, pps, mbps);
  return 0;
}

static int RunSink(DerpClient* client,
                   int duration_sec, int size,
                   BenchResult* result) {
  uint8_t buf[kMaxFramePayload];
  int buf_len;
  FrameType ftype;

  // Short recv timeout for deadline checking. On
  // loopback, TCP segments arrive in microseconds, so
  // 100ms is long enough to never fire mid-frame.
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 100000;
  setsockopt(client->fd, SOL_SOCKET, SO_RCVTIMEO,
             &tv, sizeof(tv));

  uint64_t deadline_ns =
      NowNs() +
      static_cast<uint64_t>(duration_sec) * 1000000000ULL;
  uint64_t t0 = 0;
  int received = 0;
  uint64_t total_bytes = 0;

  while (NowNs() < deadline_ns) {
    if (!ClientRecvFrame(client, &ftype, buf,
                         &buf_len, sizeof(buf))) {
      if (NowNs() >= deadline_ns) break;
      continue;
    }
    if (ftype == FrameType::kRecvPacket) {
      if (t0 == 0) t0 = NowNs();
      int data_len = buf_len - kKeySize;
      if (data_len > 0) {
        total_bytes += data_len;
      }
      received++;
    }
  }

  // Drain remaining data from TCP buffers. Read until
  // 2 seconds of idle (no data arriving).
  int idle_count = 0;
  while (idle_count < 20) {
    if (!ClientRecvFrame(client, &ftype, buf,
                         &buf_len, sizeof(buf))) {
      idle_count++;
      continue;
    }
    idle_count = 0;
    if (ftype == FrameType::kRecvPacket) {
      int data_len = buf_len - kKeySize;
      if (data_len > 0) {
        total_bytes += data_len;
      }
      received++;
    }
  }

  uint64_t t1 = NowNs();
  if (t0 == 0) t0 = t1;

  result->elapsed_ns = t1 - t0;
  result->packets_completed = received;
  result->bytes_total = total_bytes;
  result->packet_size = size;

  double secs = (t1 - t0) / 1e9;
  double pps = secs > 0 ? received / secs : 0;
  double mbps = secs > 0
      ? total_bytes * 8.0 / 1e6 / secs : 0;
  fprintf(stderr,
          "Sink: %d packets (%" PRIu64 " bytes) in "
          "%.3f s (%.0f PPS, %.1f Mbps)\n",
          received, total_bytes, secs, pps, mbps);
  return 0;
}

// Ping payload layout: [4B seq][8B send_ts_ns][padding]
static constexpr int kPingHeaderSize = 12;

static int RunPing(DerpClient* client,
                   const Key& dst_key,
                   int count, int size, int warmup,
                   LatencyRecorder* latency,
                   BenchResult* result) {
  if (size < kPingHeaderSize) {
    size = kPingHeaderSize;
  }

  auto* payload = new uint8_t[size];
  memset(payload, 0, size);

  uint8_t recv_buf[kMaxFramePayload];
  int recv_len;
  FrameType ftype;

  // Warmup phase (synchronous ping-pong).
  for (int i = 0; i < warmup; i++) {
    // Embed sequence number.
    payload[0] = static_cast<uint8_t>(i >> 24);
    payload[1] = static_cast<uint8_t>(i >> 16);
    payload[2] = static_cast<uint8_t>(i >> 8);
    payload[3] = static_cast<uint8_t>(i);

    uint64_t ts = NowNs();
    memcpy(payload + 4, &ts, 8);

    if (!ClientSendPacket(client, dst_key,
                          payload, size)) {
      fprintf(stderr, "Warmup ping send failed\n");
      delete[] payload;
      return -1;
    }

    // Wait for echo.
    bool got_echo = false;
    for (int a = 0; a < 50; a++) {
      if (!ClientRecvFrame(client, &ftype, recv_buf,
                           &recv_len,
                           sizeof(recv_buf))) {
        break;
      }
      if (ftype == FrameType::kRecvPacket) {
        got_echo = true;
        break;
      }
    }
    if (!got_echo) {
      fprintf(stderr, "Warmup ping echo timeout\n");
      delete[] payload;
      return -1;
    }
  }

  // Measured phase.
  uint64_t t0 = NowNs();
  int completed = 0;

  for (int i = 0; i < count; i++) {
    int seq = warmup + i;
    payload[0] = static_cast<uint8_t>(seq >> 24);
    payload[1] = static_cast<uint8_t>(seq >> 16);
    payload[2] = static_cast<uint8_t>(seq >> 8);
    payload[3] = static_cast<uint8_t>(seq);

    uint64_t send_ts = NowNs();
    memcpy(payload + 4, &send_ts, 8);

    if (!ClientSendPacket(client, dst_key,
                          payload, size)) {
      fprintf(stderr, "Ping send failed at %d\n", i);
      break;
    }

    // Wait for echo response.
    bool got_echo = false;
    for (int a = 0; a < 50; a++) {
      if (!ClientRecvFrame(client, &ftype, recv_buf,
                           &recv_len,
                           sizeof(recv_buf))) {
        break;
      }
      if (ftype == FrameType::kRecvPacket &&
          recv_len > kKeySize + 4) {
        got_echo = true;
        break;
      }
    }

    if (!got_echo) {
      fprintf(stderr, "Ping echo timeout at %d\n", i);
      break;
    }

    uint64_t recv_ts = NowNs();

    // Extract send timestamp from echoed payload.
    const uint8_t* echo_data = recv_buf + kKeySize;
    uint64_t orig_ts;
    memcpy(&orig_ts, echo_data + 4, 8);

    uint64_t rtt = recv_ts - orig_ts;
    latency->Record(rtt);
    completed++;
  }

  uint64_t t1 = NowNs();
  delete[] payload;

  latency->Sort();

  result->elapsed_ns = t1 - t0;
  result->packets_completed = completed;
  result->bytes_total =
      static_cast<uint64_t>(completed) * size * 2;

  fprintf(stderr,
          "Ping: %d/%d completed in %.3f ms\n"
          "  RTT min=%" PRIu64 " ns  "
          "p50=%" PRIu64 " ns  "
          "p99=%" PRIu64 " ns  "
          "max=%" PRIu64 " ns\n",
          completed, count, (t1 - t0) / 1e6,
          latency->Min(),
          latency->Percentile(0.50),
          latency->Percentile(0.99),
          latency->Max());
  return 0;
}

int main(int argc, char** argv) {
  const char* host = "127.0.0.1";
  uint16_t port = 3340;
  const char* mode = nullptr;
  const char* dst_key_hex = nullptr;
  const char* label = "bench";
  const char* output_file = nullptr;
  int count = 1;
  int size = 64;
  int duration_sec = 10;
  int warmup = 0;
  int core = -1;
  int timeout_ms = 5000;
  int ready_fd = -1;
  int num_workers = 0;
  bool json = false;
  bool raw_latency = false;
  bool use_tls = false;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
      host = argv[++i];
    } else if (strcmp(argv[i], "--port") == 0 &&
               i + 1 < argc) {
      port = static_cast<uint16_t>(atoi(argv[++i]));
    } else if (strcmp(argv[i], "--mode") == 0 &&
               i + 1 < argc) {
      mode = argv[++i];
    } else if (strcmp(argv[i], "--dst-key") == 0 &&
               i + 1 < argc) {
      dst_key_hex = argv[++i];
    } else if (strcmp(argv[i], "--count") == 0 &&
               i + 1 < argc) {
      count = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--size") == 0 &&
               i + 1 < argc) {
      size = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--duration") == 0 &&
               i + 1 < argc) {
      duration_sec = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--warmup") == 0 &&
               i + 1 < argc) {
      warmup = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--core") == 0 &&
               i + 1 < argc) {
      core = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--timeout") == 0 &&
               i + 1 < argc) {
      timeout_ms = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--ready-fd") == 0 &&
               i + 1 < argc) {
      ready_fd = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--tls") == 0) {
      use_tls = true;
    } else if (strcmp(argv[i], "--json") == 0) {
      json = true;
    } else if (strcmp(argv[i], "--raw-latency") == 0) {
      raw_latency = true;
    } else if (strcmp(argv[i], "--output") == 0 &&
               i + 1 < argc) {
      output_file = argv[++i];
    } else if (strcmp(argv[i], "--label") == 0 &&
               i + 1 < argc) {
      label = argv[++i];
    } else if (strcmp(argv[i], "--workers") == 0 &&
               i + 1 < argc) {
      num_workers = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--help") == 0 ||
               strcmp(argv[i], "-h") == 0) {
      Usage();
      return 0;
    } else {
      fprintf(stderr, "Unknown option: %s\n", argv[i]);
      Usage();
      return 1;
    }
  }

  if (!mode) {
    fprintf(stderr, "Error: --mode is required\n");
    Usage();
    return 1;
  }

  if (sodium_init() < 0) {
    fprintf(stderr, "sodium_init failed\n");
    return 1;
  }

  signal(SIGPIPE, SIG_IGN);

  if (core >= 0) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    if (sched_setaffinity(0, sizeof(cpuset),
                          &cpuset) < 0) {
      perror("sched_setaffinity");
      return 1;
    }
  }

  DerpClient client;
  (void)ClientInit(&client);

  // Print public key to stderr (stdout is for JSON).
  char hex[kKeySize * 2 + 1];
  KeyToHex(client.public_key, hex);
  fprintf(stderr, "pubkey: %s\n", hex);

  // Also print to stdout for key exchange if not JSON.
  if (!json) {
    printf("%s\n", hex);
    fflush(stdout);
  }

  Key dst_key{};
  if (ReadDstKey(mode, dst_key_hex, dst_key.data()) != 0) {
    return 1;
  }

  if (!ClientConnect(&client, host, port)) {
    fprintf(stderr, "Connect failed\n");
    return 1;
  }
  if (use_tls) {
    if (!ClientTlsConnect(&client)) {
      fprintf(stderr, "TLS connect failed\n");
      return 1;
    }
    fprintf(stderr, "TLS 1.3 connected (kTLS)\n");
  }
  if (!ClientUpgrade(&client)) {
    fprintf(stderr, "Upgrade failed\n");
    return 1;
  }
  if (!ClientHandshake(&client)) {
    fprintf(stderr, "Handshake failed\n");
    return 1;
  }

  fprintf(stderr, "Connected and handshaked\n");

  if (ready_fd >= 0) {
    write(ready_fd, "R", 1);
    close(ready_fd);
  }

  // Sink needs a short timeout to check deadline.
  if (strcmp(mode, "sink") == 0) {
    (void)ClientSetTimeout(&client, 1000);
  } else {
    (void)ClientSetTimeout(&client, timeout_ms);
  }

  BenchResult result = {};
  result.test_name = label;
  result.mode = mode;
  result.packet_count = count;
  result.packet_size = size;
  result.num_workers = num_workers;
  result.core = core;
  result.include_raw_samples = raw_latency;

  LatencyRecorder latency = {};
  int rc = 0;

  if (strcmp(mode, "send") == 0) {
    rc = RunSend(&client, dst_key, count, size,
                 warmup, &result);

  } else if (strcmp(mode, "recv") == 0) {
    rc = RunRecv(&client, count, warmup, &result);

  } else if (strcmp(mode, "echo") == 0) {
    // Echo has no warmup — it just reflects.
    rc = RunEcho(&client, count + warmup);

  } else if (strcmp(mode, "ping") == 0) {
    latency.Init(count);
    result.latency = &latency;
    rc = RunPing(&client, dst_key, count, size,
                 warmup, &latency, &result);

  } else if (strcmp(mode, "flood") == 0) {
    rc = RunFlood(&client, dst_key, size,
                  duration_sec, &result);

  } else if (strcmp(mode, "sink") == 0) {
    rc = RunSink(&client, duration_sec, size, &result);

  } else {
    fprintf(stderr, "Unknown mode: %s\n", mode);
    ClientClose(&client);
    return 1;
  }

  // Write JSON results.
  if (json && rc == 0) {
    FILE* out = stdout;
    if (output_file) {
      out = fopen(output_file, "w");
      if (!out) {
        fprintf(stderr, "Cannot open %s\n", output_file);
        out = stdout;
      }
    }
    WriteBenchJson(out, &result);
    if (out != stdout) {
      fclose(out);
    }
  }

  ClientClose(&client);
  return rc;
}
