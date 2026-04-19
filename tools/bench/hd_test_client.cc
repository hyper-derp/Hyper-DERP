/// @file hd_test_client.cc
/// @brief Standalone HD protocol test client for integration
///   and bare metal testing with benchmark instrumentation.
///
/// Modes:
///   send  — Send N HD Data frames, report throughput.
///   recv  — Receive N HD Data frames, report throughput.
///   echo  — Echo received HD Data frames back.
///   ping  — Send data, wait for echo, measure RTT.
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
#include <unistd.h>

#include "hyper_derp/bench.h"
#include "hyper_derp/handshake.h"
#include "hyper_derp/hd_client.h"
#include "hyper_derp/hd_protocol.h"
#include "hyper_derp/protocol.h"

using hyper_derp::BenchResult;
using hyper_derp::HdClient;
using hyper_derp::HdClientClose;
using hyper_derp::HdClientConnect;
using hyper_derp::HdClientEnroll;
using hyper_derp::HdClientInit;
using hyper_derp::HdClientRecvFrame;
using hyper_derp::HdClientSendData;
using hyper_derp::HdClientSetTimeout;
using hyper_derp::HdClientTlsConnect;
using hyper_derp::HdClientUpgrade;
using hyper_derp::HdFrameType;
using hyper_derp::Key;
using hyper_derp::KeyToHex;
using hyper_derp::kHdMaxFramePayload;
using hyper_derp::kKeySize;
using hyper_derp::LatencyRecorder;
using hyper_derp::NowNs;
using hyper_derp::WriteBenchJson;

static void Usage() {
  fprintf(stderr,
      "Usage: hd-test-client [options]\n"
      "\n"
      "Modes:\n"
      "  --mode send     Send N HD Data frames\n"
      "  --mode recv     Receive N HD Data frames\n"
      "  --mode echo     Echo received data back\n"
      "  --mode ping     RTT measurement (needs echo)\n"
      "\n"
      "Options:\n"
      "  --host HOST     Server host (default: 127.0.0.1)\n"
      "  --port PORT     Server port (default: 3340)\n"
      "  --relay-key HEX Relay key (64 hex chars)\n"
      "  --count N       Number of packets (default: 10)\n"
      "  --size N        Payload size bytes (default: 1400)\n"
      "  --timeout MS    Recv timeout ms (default: 5000)\n"
      "  --tls           TLS 1.3 connect\n"
      "  --core N        Pin to CPU core\n"
      "  --ready-fd N    Write 'R' to this fd when ready\n"
      "  --json          JSON results to stdout\n"
      "  --raw-latency   Include raw samples in JSON\n"
      "  --output FILE   Write JSON to file\n"
      "  --label NAME    Test label for JSON output\n"
      "  --workers N     Relay worker count (metadata)\n"
      "  --help          Show usage\n");
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

// Ping payload layout: [4B seq][8B send_ts_ns][padding]
static constexpr int kPingHeaderSize = 12;

static int RunSend(HdClient* client, int count,
                   int size, BenchResult* result) {
  auto* payload = new uint8_t[size];
  for (int i = 0; i < size; i++) {
    payload[i] = static_cast<uint8_t>(i & 0xff);
  }

  uint64_t t0 = NowNs();
  int sent = 0;

  for (int i = 0; i < count; i++) {
    if (size >= 4) {
      payload[0] = static_cast<uint8_t>(i >> 24);
      payload[1] = static_cast<uint8_t>(i >> 16);
      payload[2] = static_cast<uint8_t>(i >> 8);
      payload[3] = static_cast<uint8_t>(i);
    }
    auto r = HdClientSendData(client, payload, size);
    if (!r) {
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

  fprintf(stderr,
          "Sent %d packets (%d bytes each) in %.3f ms\n",
          sent, size, (t1 - t0) / 1e6);
  return 0;
}

static int RunRecv(HdClient* client, int count,
                   BenchResult* result) {
  uint8_t buf[kHdMaxFramePayload];
  int buf_len;
  HdFrameType ftype;

  uint64_t t0 = NowNs();
  int received = 0;
  uint64_t total_bytes = 0;

  while (received < count) {
    auto r = HdClientRecvFrame(client, &ftype, buf,
                               &buf_len, sizeof(buf));
    if (!r) {
      fprintf(stderr, "Recv failed after %d packets\n",
              received);
      break;
    }
    if (ftype == HdFrameType::kData) {
      total_bytes += buf_len;
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

static int RunEcho(HdClient* client, int count) {
  uint8_t buf[kHdMaxFramePayload];
  int buf_len;
  HdFrameType ftype;
  int echoed = 0;

  while (echoed < count) {
    auto r = HdClientRecvFrame(client, &ftype, buf,
                               &buf_len, sizeof(buf));
    if (!r) {
      break;
    }
    if (ftype == HdFrameType::kData) {
      auto s = HdClientSendData(client, buf, buf_len);
      if (!s) {
        break;
      }
      echoed++;
    }
  }

  fprintf(stderr, "Echoed %d packets\n", echoed);
  return 0;
}

static int RunPing(HdClient* client, int count,
                   int size, LatencyRecorder* latency,
                   BenchResult* result) {
  if (size < kPingHeaderSize) {
    size = kPingHeaderSize;
  }

  auto* payload = new uint8_t[size];
  memset(payload, 0, size);

  uint8_t recv_buf[kHdMaxFramePayload];
  int recv_len;
  HdFrameType ftype;

  uint64_t t0 = NowNs();
  int completed = 0;

  for (int i = 0; i < count; i++) {
    // Embed sequence number (big-endian).
    payload[0] = static_cast<uint8_t>(i >> 24);
    payload[1] = static_cast<uint8_t>(i >> 16);
    payload[2] = static_cast<uint8_t>(i >> 8);
    payload[3] = static_cast<uint8_t>(i);

    // Embed send timestamp.
    uint64_t send_ts = NowNs();
    memcpy(payload + 4, &send_ts, 8);

    auto s = HdClientSendData(client, payload, size);
    if (!s) {
      fprintf(stderr, "Ping send failed at %d\n", i);
      break;
    }

    // Wait for echo response.
    bool got_echo = false;
    for (int a = 0; a < 50; a++) {
      auto r = HdClientRecvFrame(client, &ftype, recv_buf,
                                 &recv_len,
                                 sizeof(recv_buf));
      if (!r) {
        break;
      }
      if (ftype == HdFrameType::kData &&
          recv_len >= kPingHeaderSize) {
        got_echo = true;
        break;
      }
    }

    if (!got_echo) {
      fprintf(stderr, "Ping echo timeout at %d\n", i);
      break;
    }

    uint64_t recv_ts = NowNs();

    // Extract original send timestamp from echoed payload.
    uint64_t orig_ts;
    memcpy(&orig_ts, recv_buf + 4, 8);

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
  const char* relay_key_hex = nullptr;
  const char* label = "bench";
  const char* output_file = nullptr;
  int count = 10;
  int size = 1400;
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
    } else if (strcmp(argv[i], "--relay-key") == 0 &&
               i + 1 < argc) {
      relay_key_hex = argv[++i];
    } else if (strcmp(argv[i], "--count") == 0 &&
               i + 1 < argc) {
      count = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--size") == 0 &&
               i + 1 < argc) {
      size = atoi(argv[++i]);
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

  if (!relay_key_hex) {
    fprintf(stderr, "Error: --relay-key is required\n");
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

  // Decode relay key from hex.
  Key relay_key{};
  int hlen = static_cast<int>(strlen(relay_key_hex));
  if (hlen != kKeySize * 2 ||
      HexDecode(relay_key_hex, relay_key.data(),
                kKeySize) != 0) {
    fprintf(stderr, "Invalid --relay-key "
            "(need 64 hex chars)\n");
    return 1;
  }

  HdClient client;
  auto init_r = HdClientInit(&client);
  if (!init_r) {
    fprintf(stderr, "HdClientInit failed\n");
    return 1;
  }

  // Set the relay key for HMAC enrollment.
  client.relay_key = relay_key;

  // Print public key to stderr (stdout is for JSON).
  char hex[kKeySize * 2 + 1];
  KeyToHex(client.public_key, hex);
  fprintf(stderr, "pubkey: %s\n", hex);

  // Also print to stdout for key exchange if not JSON.
  if (!json) {
    printf("%s\n", hex);
    fflush(stdout);
  }

  // Connect.
  auto conn_r = HdClientConnect(&client, host, port);
  if (!conn_r) {
    fprintf(stderr, "Connect failed\n");
    return 1;
  }
  if (use_tls) {
    auto tls_r = HdClientTlsConnect(&client);
    if (!tls_r) {
      fprintf(stderr, "TLS connect failed\n");
      return 1;
    }
    fprintf(stderr, "TLS 1.3 connected\n");
  }

  // HTTP upgrade to /hd.
  auto up_r = HdClientUpgrade(&client);
  if (!up_r) {
    fprintf(stderr, "HD upgrade failed\n");
    return 1;
  }

  // Enrollment handshake.
  auto enroll_r = HdClientEnroll(&client);
  if (!enroll_r) {
    fprintf(stderr, "HD enrollment failed\n");
    return 1;
  }

  fprintf(stderr, "Connected and enrolled\n");

  if (ready_fd >= 0) {
    write(ready_fd, "R", 1);
    close(ready_fd);
  }

  auto to_r = HdClientSetTimeout(&client, timeout_ms);
  if (!to_r) {
    fprintf(stderr, "SetTimeout failed\n");
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
    rc = RunSend(&client, count, size, &result);

  } else if (strcmp(mode, "recv") == 0) {
    rc = RunRecv(&client, count, &result);

  } else if (strcmp(mode, "echo") == 0) {
    rc = RunEcho(&client, count);

  } else if (strcmp(mode, "ping") == 0) {
    latency.Init(count);
    result.latency = &latency;
    rc = RunPing(&client, count, size, &latency,
                 &result);

  } else {
    fprintf(stderr, "Unknown mode: %s\n", mode);
    HdClientClose(&client);
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

  HdClientClose(&client);
  return rc;
}
