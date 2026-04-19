/// @file hdctl.cc
/// @brief HD relay management CLI via ZMQ IPC.
///
/// Usage:
///   hdctl status
///   hdctl peers
///   hdctl peer approve KEY
///   hdctl peer deny KEY
///   hdctl peer remove KEY
///   hdctl workers
///   hdctl rules add SRC_KEY DST_KEY
///   hdctl connect --relay-host HOST --relay-key KEY
///
/// Uses ZMQ DEALER socket to ipc:///tmp/hyper-derp.sock.
/// The connect subcommand uses HD protocol directly (TCP).

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <zmq.hpp>

#include <string>
#include <string_view>

using namespace std::string_view_literals;

static const char* g_ipc = "ipc:///tmp/hyper-derp.sock";

// -- ZMQ client --------------------------------------------------------------

static std::string ZmqRequest(const char* ipc_path,
                              const std::string& req) {
  try {
    zmq::context_t ctx(1);
    zmq::socket_t sock(ctx, zmq::socket_type::dealer);
    sock.set(zmq::sockopt::rcvtimeo, 2000);
    sock.set(zmq::sockopt::sndtimeo, 2000);
    sock.set(zmq::sockopt::linger, 0);
    sock.connect(ipc_path);

    // DEALER sends: [empty delimiter][payload]
    sock.send(zmq::message_t(), zmq::send_flags::sndmore);
    sock.send(zmq::buffer(req), zmq::send_flags::none);

    // Receive: [empty delimiter][response]
    zmq::message_t delim, resp;
    auto r1 = sock.recv(delim, zmq::recv_flags::none);
    if (!r1) return "{\"error\":\"timeout\"}";
    auto r2 = sock.recv(resp, zmq::recv_flags::none);
    if (!r2) return "{\"error\":\"timeout\"}";

    return std::string(
        static_cast<char*>(resp.data()), resp.size());
  } catch (const zmq::error_t& e) {
    return std::string("{\"error\":\"") + e.what() + "\"}";
  }
}

// -- Pretty print JSON (minimal) --------------------------------------------

static void PrettyPrint(const std::string& json) {
  int indent = 0;
  bool in_str = false;
  for (char c : json) {
    if (c == '"') in_str = !in_str;
    if (in_str) {
      putchar(c);
      continue;
    }
    if (c == '{' || c == '[') {
      putchar(c);
      putchar('\n');
      indent += 2;
      for (int i = 0; i < indent; i++) putchar(' ');
    } else if (c == '}' || c == ']') {
      putchar('\n');
      indent -= 2;
      if (indent < 0) indent = 0;
      for (int i = 0; i < indent; i++) putchar(' ');
      putchar(c);
    } else if (c == ',') {
      putchar(c);
      putchar('\n');
      for (int i = 0; i < indent; i++) putchar(' ');
    } else if (c == ':') {
      putchar(':');
      putchar(' ');
    } else {
      putchar(c);
    }
  }
  putchar('\n');
}

// -- Commands ----------------------------------------------------------------

static void CmdStatus() {
  auto resp = ZmqRequest(g_ipc, "{\"cmd\":\"status\"}");
  PrettyPrint(resp);
}

static void CmdPeers() {
  auto resp = ZmqRequest(g_ipc, "{\"cmd\":\"peers\"}");
  PrettyPrint(resp);
}

static void CmdWorkers() {
  auto resp = ZmqRequest(g_ipc, "{\"cmd\":\"workers\"}");
  PrettyPrint(resp);
}

static void CmdPeerAction(const char* action,
                          const char* key) {
  std::string req = "{\"cmd\":\"peer_";
  req += action;
  req += "\",\"key\":\"";
  req += key;
  req += "\"}";
  auto resp = ZmqRequest(g_ipc, req);
  PrettyPrint(resp);
}

static void CmdRulesAdd(const char* src, const char* dst) {
  std::string req = "{\"cmd\":\"rules_add\",\"src\":\"";
  req += src;
  req += "\",\"dst\":\"";
  req += dst;
  req += "\"}";
  auto resp = ZmqRequest(g_ipc, req);
  PrettyPrint(resp);
}

// -- Interactive connect (same as before, via HD protocol) ---

static void CmdConnect(const char* host,
                       uint16_t relay_port,
                       const char* relay_key_hex,
                       bool use_tls);

// -- Main --------------------------------------------------------------------

static void PrintUsage() {
  fprintf(stderr,
      "Usage: hdctl [--sock PATH] <command> [args]\n"
      "\n"
      "Options:\n"
      "  --sock PATH     IPC socket path\n"
      "                  (default: ipc:///tmp/hyper-derp.sock)\n"
      "\n"
      "Commands:\n"
      "  status                    Relay health + info\n"
      "  peers                     List HD peers\n"
      "  peer approve KEY          Approve pending peer\n"
      "  peer deny KEY             Deny pending peer\n"
      "  peer remove KEY           Remove peer\n"
      "  workers                   Per-worker stats\n"
      "  rules add SRC_KEY DST_KEY Add forwarding rule\n"
      "  connect --relay-host HOST Interactive HD client\n"
      "    --relay-key KEY [--tls]\n");
}

int main(int argc, char** argv) {
  int cmd_start = 1;

  // Parse global options.
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--sock") == 0 && i + 1 < argc) {
      g_ipc = argv[++i];
      cmd_start = i + 1;
    } else if (strcmp(argv[i], "--help") == 0) {
      PrintUsage();
      return 0;
    } else {
      cmd_start = i;
      break;
    }
  }

  if (cmd_start >= argc) {
    PrintUsage();
    return 1;
  }

  auto cmd = std::string_view(argv[cmd_start]);

  if (cmd == "status"sv) {
    CmdStatus();
  } else if (cmd == "peers"sv) {
    CmdPeers();
  } else if (cmd == "workers"sv) {
    CmdWorkers();
  } else if (cmd == "peer"sv && cmd_start + 2 < argc) {
    auto action = std::string_view(argv[cmd_start + 1]);
    const char* key = argv[cmd_start + 2];
    if (action == "approve"sv || action == "deny"sv ||
        action == "remove"sv) {
      CmdPeerAction(
          std::string(action).c_str(), key);
    } else {
      fprintf(stderr, "unknown peer action: %s\n",
              argv[cmd_start + 1]);
      return 1;
    }
  } else if (cmd == "rules"sv && cmd_start + 3 < argc) {
    auto action = std::string_view(argv[cmd_start + 1]);
    if (action == "add"sv) {
      CmdRulesAdd(argv[cmd_start + 2],
                  argv[cmd_start + 3]);
    } else {
      fprintf(stderr, "unknown rules action: %s\n",
              argv[cmd_start + 1]);
      return 1;
    }
  } else if (cmd == "connect"sv) {
    const char* host = nullptr;
    const char* relay_key_hex = nullptr;
    bool use_tls = true;
    uint16_t hd_port = 3341;
    for (int i = cmd_start + 1; i < argc; i++) {
      auto a = std::string_view(argv[i]);
      if (a == "--relay-host"sv && i + 1 < argc)
        host = argv[++i];
      else if (a == "--relay-key"sv && i + 1 < argc)
        relay_key_hex = argv[++i];
      else if (a == "--port"sv && i + 1 < argc)
        hd_port = static_cast<uint16_t>(atoi(argv[++i]));
      else if (a == "--no-tls"sv)
        use_tls = false;
    }
    if (!host || !relay_key_hex) {
      fprintf(stderr, "connect requires --relay-host "
              "and --relay-key\n");
      return 1;
    }
    CmdConnect(host, hd_port, relay_key_hex, use_tls);
  } else {
    fprintf(stderr, "unknown command: %s\n",
            argv[cmd_start]);
    PrintUsage();
    return 1;
  }

  return 0;
}

// -- Interactive connect (HD protocol, not ZMQ) --------------

#include <arpa/inet.h>
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
         "  send <text>       Send HD Data frame\n"
         "  mesh <id> <text>  Send MeshData to peer\n"
         "  ping              Send Ping\n"
         "  quit              Disconnect\n\n");

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
        input == "q"sv)
      break;

    if (input.starts_with("send "sv)) {
      auto text = input.substr(5);
      auto r = HdClientSendData(
          &hd,
          reinterpret_cast<const uint8_t*>(text.data()),
          static_cast<int>(text.size()));
      if (!r)
        fprintf(stderr, "send: %s\n",
                r.error().message.c_str());
      else
        printf("sent %zu bytes\n", text.size());
    } else if (input.starts_with("mesh "sv)) {
      auto rest = input.substr(5);
      auto sp = rest.find(' ');
      if (sp == std::string_view::npos) {
        printf("usage: mesh <id> <text>\n");
        continue;
      }
      uint16_t dst = static_cast<uint16_t>(
          atoi(std::string(rest.substr(0, sp)).c_str()));
      auto text = rest.substr(sp + 1);
      auto r = HdClientSendMeshData(
          &hd, dst,
          reinterpret_cast<const uint8_t*>(text.data()),
          static_cast<int>(text.size()));
      if (!r)
        fprintf(stderr, "mesh: %s\n",
                r.error().message.c_str());
      else
        printf("mesh %zu bytes -> peer %d\n",
               text.size(), dst);
    } else if (input == "ping"sv) {
      auto r = HdClientSendPing(&hd);
      if (!r)
        fprintf(stderr, "ping: %s\n",
                r.error().message.c_str());
      else
        printf("ping sent\n");
    }

    // Poll incoming.
    uint8_t buf[65536];
    int buf_len;
    HdFrameType ftype;
    while (true) {
      auto rv = HdClientRecvFrame(
          &hd, &ftype, buf, &buf_len, sizeof(buf));
      if (!rv) break;

      switch (ftype) {
        case HdFrameType::kData:
          printf("[data %d] %.*s\n",
                 buf_len, buf_len, buf);
          break;
        case HdFrameType::kMeshData:
          if (buf_len >= 2) {
            uint16_t src =
                static_cast<uint16_t>(buf[0]) << 8 |
                buf[1];
            printf("[mesh from=%d] %.*s\n",
                   src, buf_len - 2, buf + 2);
          }
          break;
        case HdFrameType::kPong:
          printf("[pong]\n");
          break;
        case HdFrameType::kPeerInfo:
          if (buf_len >= 34) {
            char khex[65];
            sodium_bin2hex(khex, sizeof(khex), buf, 32);
            uint16_t pid =
                static_cast<uint16_t>(buf[32]) << 8 |
                buf[33];
            printf("[peer_info %s id=%d]\n", khex, pid);
          }
          break;
        case HdFrameType::kPeerGone:
          if (buf_len >= 32) {
            char khex[65];
            sodium_bin2hex(khex, sizeof(khex), buf, 32);
            printf("[peer_gone %s]\n", khex);
          }
          break;
        default:
          printf("[frame 0x%02x %d bytes]\n",
                 static_cast<int>(ftype), buf_len);
      }
    }
  }

  HdClientClose(&hd);
  printf("disconnected\n");
}
