/// @file hdctl.cc
/// @brief HD relay management CLI.
///
/// Usage:
///   hdctl --relay HOST:PORT status
///   hdctl --relay HOST:PORT peers
///   hdctl --relay HOST:PORT peer approve KEY
///   hdctl --relay HOST:PORT peer deny KEY
///   hdctl --relay HOST:PORT peer remove KEY
///   hdctl --relay HOST:PORT workers
///   hdctl --relay HOST:PORT rules add SRC_KEY DST_KEY
///   hdctl --relay HOST:PORT rules remove SRC_KEY DST_KEY
///   hdctl --relay HOST:PORT connect --relay-key KEY [--tls]

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>
#include <string_view>
#include <vector>

using namespace std::string_view_literals;

// -- HTTP client (minimal, connects to relay metrics port) ---

static std::string HttpRequest(const char* host,
                               uint16_t port,
                               const char* method,
                               const char* path,
                               const char* body = nullptr) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return "";

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
    close(fd);
    return "";
  }
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr),
              sizeof(addr)) < 0) {
    close(fd);
    return "";
  }

  // Build request.
  char req[2048];
  int req_len;
  if (body) {
    int body_len = static_cast<int>(strlen(body));
    req_len = snprintf(req, sizeof(req),
        "%s %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n%s",
        method, path, host, port, body_len, body);
  } else {
    req_len = snprintf(req, sizeof(req),
        "%s %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Connection: close\r\n\r\n",
        method, path, host, port);
  }

  int total = 0;
  while (total < req_len) {
    int w = write(fd, req + total, req_len - total);
    if (w <= 0) break;
    total += w;
  }

  // Read response.
  std::string resp;
  char buf[4096];
  while (true) {
    int n = read(fd, buf, sizeof(buf));
    if (n <= 0) break;
    resp.append(buf, n);
  }
  close(fd);

  // Strip HTTP headers.
  auto body_start = resp.find("\r\n\r\n");
  if (body_start != std::string::npos) {
    return resp.substr(body_start + 4);
  }
  return resp;
}

// -- Commands ----------------------------------------------------------------

static void CmdStatus(const char* host, uint16_t port) {
  auto health = HttpRequest(host, port, "GET", "/health");
  auto relay = HttpRequest(host, port, "GET",
                           "/api/v1/relay");
  if (health.empty()) {
    fprintf(stderr, "error: cannot connect to %s:%d\n",
            host, port);
    return;
  }
  printf("Health:\n%s\n\nRelay:\n%s\n",
         health.c_str(), relay.c_str());
}

static void CmdPeers(const char* host, uint16_t port) {
  auto resp = HttpRequest(host, port, "GET",
                          "/api/v1/peers");
  if (resp.empty()) {
    fprintf(stderr, "error: cannot connect to %s:%d\n",
            host, port);
    return;
  }
  printf("%s\n", resp.c_str());
}

static void CmdWorkers(const char* host, uint16_t port) {
  auto resp = HttpRequest(host, port, "GET",
                          "/debug/workers");
  if (resp.empty()) {
    fprintf(stderr, "error: cannot connect (need "
            "--debug-endpoints on relay)\n");
    return;
  }
  printf("%s\n", resp.c_str());
}

static void CmdPeerAction(const char* host, uint16_t port,
                          const char* action,
                          const char* key) {
  char path[256];
  snprintf(path, sizeof(path), "/api/v1/peers/%s/%s",
           key, action);
  auto resp = HttpRequest(host, port, "POST", path);
  printf("%s\n", resp.c_str());
}

static void CmdPeerRemove(const char* host, uint16_t port,
                          const char* key) {
  char path[256];
  snprintf(path, sizeof(path), "/api/v1/peers/%s", key);
  auto resp = HttpRequest(host, port, "DELETE", path);
  printf("%s\n", resp.c_str());
}

static void CmdRulesAdd(const char* host, uint16_t port,
                        const char* src_key,
                        const char* dst_key) {
  char path[256];
  snprintf(path, sizeof(path), "/api/v1/peers/%s/rules",
           src_key);
  char body[256];
  snprintf(body, sizeof(body),
           "{\"dst_key\":\"%s\"}", dst_key);
  auto resp = HttpRequest(host, port, "POST", path, body);
  printf("%s\n", resp.c_str());
}

static void CmdRulesRemove(const char* host, uint16_t port,
                           const char* src_key,
                           const char* dst_key) {
  char path[256];
  snprintf(path, sizeof(path),
           "/api/v1/peers/%s/rules/%s", src_key, dst_key);
  auto resp = HttpRequest(host, port, "DELETE", path);
  printf("%s\n", resp.c_str());
}

// -- Interactive connect mode ------------------------------------------------

static void CmdConnect(const char* host, uint16_t relay_port,
                       const char* relay_key_hex,
                       bool use_tls);

// -- Main --------------------------------------------------------------------

static void PrintUsage() {
  fprintf(stderr,
      "Usage: hdctl --relay HOST:PORT <command> [args]\n"
      "\n"
      "Commands:\n"
      "  status                    Relay health + info\n"
      "  peers                     List HD peers\n"
      "  peer approve KEY          Approve pending peer\n"
      "  peer deny KEY             Deny pending peer\n"
      "  peer remove KEY           Remove peer\n"
      "  workers                   Per-worker stats\n"
      "  rules add SRC_KEY DST_KEY Add forwarding rule\n"
      "  rules remove SRC DST      Remove forwarding rule\n"
      "  connect --relay-key KEY   Interactive HD client\n"
      "    [--tls]\n");
}

int main(int argc, char** argv) {
  const char* relay_arg = nullptr;

  // Find --relay.
  int cmd_start = 1;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--relay") == 0 && i + 1 < argc) {
      relay_arg = argv[++i];
      cmd_start = i + 1;
      break;
    }
  }

  if (!relay_arg || cmd_start >= argc) {
    PrintUsage();
    return 1;
  }

  // Parse host:port.
  std::string relay_str = relay_arg;
  std::string host;
  uint16_t port = 9191;
  auto colon = relay_str.rfind(':');
  if (colon != std::string::npos) {
    host = relay_str.substr(0, colon);
    port = static_cast<uint16_t>(
        atoi(relay_str.c_str() + colon + 1));
  } else {
    host = relay_str;
  }

  auto cmd = std::string_view(argv[cmd_start]);

  if (cmd == "status"sv) {
    CmdStatus(host.c_str(), port);
  } else if (cmd == "peers"sv) {
    CmdPeers(host.c_str(), port);
  } else if (cmd == "workers"sv) {
    CmdWorkers(host.c_str(), port);
  } else if (cmd == "peer"sv && cmd_start + 2 < argc) {
    auto action = std::string_view(argv[cmd_start + 1]);
    const char* key = argv[cmd_start + 2];
    if (action == "approve"sv) {
      CmdPeerAction(host.c_str(), port, "approve", key);
    } else if (action == "deny"sv) {
      CmdPeerAction(host.c_str(), port, "deny", key);
    } else if (action == "remove"sv) {
      CmdPeerRemove(host.c_str(), port, key);
    } else {
      fprintf(stderr, "unknown peer action: %s\n",
              argv[cmd_start + 1]);
      return 1;
    }
  } else if (cmd == "rules"sv && cmd_start + 3 < argc) {
    auto action = std::string_view(argv[cmd_start + 1]);
    const char* src = argv[cmd_start + 2];
    const char* dst = argv[cmd_start + 3];
    if (action == "add"sv) {
      CmdRulesAdd(host.c_str(), port, src, dst);
    } else if (action == "remove"sv) {
      CmdRulesRemove(host.c_str(), port, src, dst);
    } else {
      fprintf(stderr, "unknown rules action: %s\n",
              argv[cmd_start + 1]);
      return 1;
    }
  } else if (cmd == "connect"sv) {
    // Parse connect-specific args.
    const char* relay_key_hex = nullptr;
    bool use_tls = true;
    // The relay port for HD protocol (not metrics).
    uint16_t hd_port = 3341;
    for (int i = cmd_start + 1; i < argc; i++) {
      auto a = std::string_view(argv[i]);
      if (a == "--relay-key"sv && i + 1 < argc) {
        relay_key_hex = argv[++i];
      } else if (a == "--tls"sv) {
        use_tls = true;
      } else if (a == "--no-tls"sv) {
        use_tls = false;
      } else if (a == "--port"sv && i + 1 < argc) {
        hd_port = static_cast<uint16_t>(atoi(argv[++i]));
      }
    }
    if (!relay_key_hex) {
      fprintf(stderr, "error: connect requires "
              "--relay-key\n");
      return 1;
    }
    CmdConnect(host.c_str(), hd_port, relay_key_hex,
               use_tls);
  } else {
    fprintf(stderr, "unknown command: %s\n",
            argv[cmd_start]);
    PrintUsage();
    return 1;
  }

  return 0;
}

// -- Interactive connect implementation --------------------------------------

#include <sodium.h>
#include "hyper_derp/hd_client.h"
#include "hyper_derp/hd_protocol.h"
#include "hyper_derp/protocol.h"

using namespace hyper_derp;

static void CmdConnect(const char* host,
                       uint16_t relay_port,
                       const char* relay_key_hex,
                       bool use_tls) {
  Key relay_key{};
  if (strlen(relay_key_hex) != 64 ||
      sodium_hex2bin(relay_key.data(), 32,
                     relay_key_hex, 64,
                     nullptr, nullptr, nullptr) != 0) {
    fprintf(stderr, "error: invalid relay key\n");
    return;
  }

  HdClient hd{};
  auto init = HdClientInit(&hd);
  if (!init) {
    fprintf(stderr, "init: %s\n",
            init.error().message.c_str());
    return;
  }
  hd.relay_key = relay_key;

  // Print our key.
  char hex[65];
  sodium_bin2hex(hex, sizeof(hex),
                 hd.public_key.data(), 32);
  printf("public_key: %s\n", hex);

  auto conn = HdClientConnect(&hd, host, relay_port);
  if (!conn) {
    fprintf(stderr, "connect: %s\n",
            conn.error().message.c_str());
    return;
  }

  if (use_tls) {
    auto tls = HdClientTlsConnect(&hd);
    if (!tls) {
      fprintf(stderr, "tls: %s\n",
              tls.error().message.c_str());
      HdClientClose(&hd);
      return;
    }
    printf("tls: connected\n");
  }

  auto up = HdClientUpgrade(&hd);
  if (!up) {
    fprintf(stderr, "upgrade: %s\n",
            up.error().message.c_str());
    HdClientClose(&hd);
    return;
  }

  auto enr = HdClientEnroll(&hd);
  if (!enr) {
    fprintf(stderr, "enroll: %s\n",
            enr.error().message.c_str());
    HdClientClose(&hd);
    return;
  }
  printf("enrolled: peer_id=%d\n", hd.peer_id);
  printf("\nCommands:\n"
         "  send <text>     Send HD Data frame\n"
         "  mesh <id> <text> Send MeshData to peer id\n"
         "  ping            Send Ping\n"
         "  quit            Disconnect\n"
         "  (empty)         Poll for incoming frames\n\n");

  HdClientSetTimeout(&hd, 200);

  char line[2048];
  while (true) {
    printf("> ");
    fflush(stdout);

    if (!fgets(line, sizeof(line), stdin)) break;
    int len = static_cast<int>(strlen(line));
    if (len > 0 && line[len - 1] == '\n')
      line[--len] = '\0';

    auto input = std::string_view(line, len);

    if (input == "quit"sv || input == "exit"sv ||
        input == "q"sv) {
      break;
    } else if (input.starts_with("send "sv)) {
      auto text = input.substr(5);
      auto r = HdClientSendData(
          &hd,
          reinterpret_cast<const uint8_t*>(text.data()),
          static_cast<int>(text.size()));
      if (!r) {
        fprintf(stderr, "send error: %s\n",
                r.error().message.c_str());
      } else {
        printf("sent %zu bytes\n", text.size());
      }
    } else if (input.starts_with("mesh "sv)) {
      // "mesh 2 hello world"
      auto rest = input.substr(5);
      auto sp = rest.find(' ');
      if (sp == std::string_view::npos) {
        printf("usage: mesh <peer_id> <text>\n");
        continue;
      }
      uint16_t dst_id = static_cast<uint16_t>(
          atoi(std::string(rest.substr(0, sp)).c_str()));
      auto text = rest.substr(sp + 1);
      auto r = HdClientSendMeshData(
          &hd, dst_id,
          reinterpret_cast<const uint8_t*>(text.data()),
          static_cast<int>(text.size()));
      if (!r) {
        fprintf(stderr, "mesh error: %s\n",
                r.error().message.c_str());
      } else {
        printf("mesh sent %zu bytes to peer %d\n",
               text.size(), dst_id);
      }
    } else if (input == "ping"sv) {
      auto r = HdClientSendPing(&hd);
      if (!r) {
        fprintf(stderr, "ping error: %s\n",
                r.error().message.c_str());
      } else {
        printf("ping sent\n");
      }
    } else {
      // Empty or unknown: poll for frames.
    }

    // Always poll for incoming frames.
    uint8_t buf[65536];
    int buf_len;
    HdFrameType ftype;
    while (true) {
      auto rv = HdClientRecvFrame(
          &hd, &ftype, buf, &buf_len, sizeof(buf));
      if (!rv) break;

      switch (ftype) {
        case HdFrameType::kData:
          printf("[data %d bytes] %.*s\n",
                 buf_len, buf_len, buf);
          break;
        case HdFrameType::kMeshData:
          if (buf_len >= 2) {
            uint16_t src =
                static_cast<uint16_t>(buf[0]) << 8 |
                buf[1];
            printf("[mesh from=%d %d bytes] %.*s\n",
                   src, buf_len - 2,
                   buf_len - 2, buf + 2);
          }
          break;
        case HdFrameType::kPong:
          printf("[pong]\n");
          break;
        case HdFrameType::kPeerInfo: {
          if (buf_len >= 34) {
            char khex[65];
            sodium_bin2hex(khex, sizeof(khex), buf, 32);
            uint16_t pid =
                static_cast<uint16_t>(buf[32]) << 8 |
                buf[33];
            printf("[peer_info key=%s id=%d]\n",
                   khex, pid);
          }
          break;
        }
        case HdFrameType::kPeerGone: {
          if (buf_len >= 32) {
            char khex[65];
            sodium_bin2hex(khex, sizeof(khex), buf, 32);
            printf("[peer_gone key=%s]\n", khex);
          }
          break;
        }
        default:
          printf("[frame type=0x%02x len=%d]\n",
                 static_cast<int>(ftype), buf_len);
          break;
      }
    }
  }

  HdClientClose(&hd);
  printf("disconnected\n");
}
