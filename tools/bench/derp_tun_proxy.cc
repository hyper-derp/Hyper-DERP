/// @file derp_tun_proxy.cc
/// @brief DERP TUN proxy — bridges a Linux TUN device to
///   a DERP relay connection for real IP traffic testing.
///
/// Creates a TUN interface, connects to the relay, and
/// runs two threads:
///   - TUN → relay: read IP packets, SendPacket to peer
///   - relay → TUN: RecvPacket from relay, write to TUN
///
/// Usage:
///   sudo ./derp-tun-proxy --host 127.0.0.1 --port 3340 \
///     --tun-ip 10.99.0.1/24 --dst-key <hex>

#include <spdlog/spdlog.h>
#include <sodium.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <unistd.h>

#include "hyper_derp/client.h"
#include "hyper_derp/protocol.h"
#include "hyper_derp/tun.h"

using namespace hyper_derp;

// -- Globals -------------------------------------------------

static volatile int g_running = 1;

static void SignalHandler(int sig) {
  (void)sig;
  g_running = 0;
}

struct ProxyCtx {
  DerpClient* client;
  TunDevice* tun;
  Key dst_key{};
};

// -- Hex key parsing -----------------------------------------

static int HexToByte(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int ParseHexKey(const char* hex,
                       uint8_t key[kKeySize]) {
  if (strlen(hex) != kKeySize * 2) {
    return -1;
  }
  for (int i = 0; i < kKeySize; i++) {
    int hi = HexToByte(hex[2 * i]);
    int lo = HexToByte(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) {
      return -1;
    }
    key[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return 0;
}

static void PrintHexKey(FILE* f, const uint8_t* key) {
  for (int i = 0; i < kKeySize; i++) {
    fprintf(f, "%02x", key[i]);
  }
}

// -- Worker threads ------------------------------------------

static void* TunToRelay(void* arg) {
  auto* ctx = static_cast<ProxyCtx*>(arg);
  uint8_t buf[1500];

  spdlog::info("tun→relay thread started");
  while (g_running) {
    int n = read(ctx->tun->fd, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (g_running) {
        spdlog::error("tun read: {}", strerror(errno));
      }
      break;
    }
    if (n == 0) {
      break;
    }
    if (!ClientSendPacket(ctx->client, ctx->dst_key,
                          buf, n)) {
      if (g_running) {
        spdlog::error("send to relay failed");
      }
      break;
    }
  }
  spdlog::info("tun→relay thread exiting");
  return nullptr;
}

static void* RelayToTun(void* arg) {
  auto* ctx = static_cast<ProxyCtx*>(arg);
  uint8_t payload[kMaxFramePayload];
  FrameType type;
  int payload_len;

  spdlog::info("relay→tun thread started");
  while (g_running) {
    if (!ClientRecvFrame(ctx->client, &type, payload,
                         &payload_len,
                         sizeof(payload))) {
      if (g_running) {
        spdlog::error("recv from relay failed");
      }
      break;
    }

    if (type == FrameType::kRecvPacket) {
      // RecvPacket payload: [32B src_key][packet_data]
      if (payload_len <= kKeySize) {
        continue;
      }
      const uint8_t* pkt = payload + kKeySize;
      int pkt_len = payload_len - kKeySize;
      int w = write(ctx->tun->fd, pkt, pkt_len);
      if (w < 0 && g_running) {
        spdlog::error("tun write: {}", strerror(errno));
        break;
      }
    } else if (type == FrameType::kKeepAlive) {
      // Ignore keepalives.
    } else if (type == FrameType::kPeerGone) {
      spdlog::warn("peer gone notification");
    } else {
      spdlog::debug("ignored frame type 0x{:02x}",
                    static_cast<int>(type));
    }
  }
  spdlog::info("relay→tun thread exiting");
  return nullptr;
}

// -- Usage ---------------------------------------------------

static void Usage(const char* prog) {
  fprintf(stderr,
          "Usage: %s --host HOST --port PORT "
          "--tun-ip CIDR --dst-key HEX\n"
          "\n"
          "Options:\n"
          "  --host      Relay server IP\n"
          "  --port      Relay server port\n"
          "  --tun-ip    TUN IP in CIDR (e.g. 10.99.0.1/24)\n"
          "  --dst-key   Destination peer's public key (hex)\n"
          "  --tun-name  TUN interface name (default: derp%%d)\n"
          "\n"
          "Requires root or CAP_NET_ADMIN.\n",
          prog);
}

// -- Main ----------------------------------------------------

int main(int argc, char* argv[]) {
  const char* host = nullptr;
  uint16_t port = 3340;
  const char* tun_ip = nullptr;
  const char* dst_key_hex = nullptr;
  const char* tun_name = nullptr;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
      host = argv[++i];
    } else if (strcmp(argv[i], "--port") == 0 &&
               i + 1 < argc) {
      port = static_cast<uint16_t>(atoi(argv[++i]));
    } else if (strcmp(argv[i], "--tun-ip") == 0 &&
               i + 1 < argc) {
      tun_ip = argv[++i];
    } else if (strcmp(argv[i], "--dst-key") == 0 &&
               i + 1 < argc) {
      dst_key_hex = argv[++i];
    } else if (strcmp(argv[i], "--tun-name") == 0 &&
               i + 1 < argc) {
      tun_name = argv[++i];
    } else if (strcmp(argv[i], "--help") == 0 ||
               strcmp(argv[i], "-h") == 0) {
      Usage(argv[0]);
      return 0;
    }
  }

  if (!host || !tun_ip || !dst_key_hex) {
    Usage(argv[0]);
    return 1;
  }

  if (sodium_init() < 0) {
    spdlog::error("sodium_init failed");
    return 1;
  }

  // Parse destination key.
  Key dst_key{};
  if (ParseHexKey(dst_key_hex, dst_key.data()) < 0) {
    spdlog::error("invalid --dst-key (need {} hex chars)",
                  kKeySize * 2);
    return 1;
  }

  // Parse TUN IP.
  uint32_t addr;
  int prefix_len;
  if (ParseCidr(tun_ip, &addr, &prefix_len) < 0) {
    spdlog::error("invalid --tun-ip CIDR: {}", tun_ip);
    return 1;
  }

  // Connect to relay.
  DerpClient client;
  if (!ClientInit(&client).has_value()) {
    spdlog::error("ClientInit failed");
    return 1;
  }

  fprintf(stderr, "pubkey: ");
  PrintHexKey(stderr, client.public_key.data());
  fprintf(stderr, "\n");

  spdlog::info("connecting to {}:{}", host, port);
  if (!ClientConnect(&client, host, port)) {
    spdlog::error("connect failed");
    return 1;
  }
  if (!ClientUpgrade(&client)) {
    spdlog::error("HTTP upgrade failed");
    return 1;
  }
  if (!ClientHandshake(&client)) {
    spdlog::error("handshake failed");
    return 1;
  }
  spdlog::info("relay connected");

  // Open TUN device.
  TunDevice tun;
  if (TunOpen(&tun, tun_name) < 0) {
    ClientClose(&client);
    return 1;
  }
  if (TunSetAddr(&tun, addr, prefix_len) < 0) {
    TunClose(&tun);
    ClientClose(&client);
    return 1;
  }
  if (TunBringUp(&tun) < 0) {
    TunClose(&tun);
    ClientClose(&client);
    return 1;
  }

  // Set up signal handlers.
  signal(SIGINT, SignalHandler);
  signal(SIGTERM, SignalHandler);
  signal(SIGPIPE, SIG_IGN);

  // Launch proxy threads.
  ProxyCtx ctx;
  ctx.client = &client;
  ctx.tun = &tun;
  ctx.dst_key = dst_key;

  pthread_t t2r, r2t;
  pthread_create(&t2r, nullptr, TunToRelay, &ctx);
  pthread_create(&r2t, nullptr, RelayToTun, &ctx);

  spdlog::info("proxy running ({} ↔ relay ↔ peer)",
               tun.name);

  // Wait for signal.
  while (g_running) {
    pause();
  }

  spdlog::info("shutting down");

  // Closing fds unblocks the threads.
  TunClose(&tun);
  ClientClose(&client);

  pthread_join(t2r, nullptr);
  pthread_join(r2t, nullptr);

  spdlog::info("done");
  return 0;
}
