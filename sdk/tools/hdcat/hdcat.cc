/// @file hdcat.cc
/// @brief hdcat: pipe stdin/stdout through an HD tunnel.
///
/// Like netcat/socat but over HD Protocol. Connects to a
/// relay, opens a tunnel to a peer, and bridges stdin/stdout
/// to the tunnel. Also supports listening on a local socket
/// (TCP, UDP, unix) and bridging connections to the tunnel.
///
/// Usage:
///   # Pipe stdin/stdout to a peer:
///   hdcat -r relay:3341 -k RELAY_KEY -p peer-name
///
///   # Listen on TCP, bridge to peer:
///   hdcat -r relay:3341 -k KEY -p peer -l tcp:8080
///
///   # Listen on unix socket:
///   hdcat -r relay:3341 -k KEY -p peer -l unix:/tmp/hd.sock
///
///   # Listen on UDP:
///   hdcat -r relay:3341 -k KEY -p peer -l udp:9000
///
///   # Connect to a TCP target and bridge to peer:
///   hdcat -r relay:3341 -k KEY -p peer -c tcp:host:port

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#include <ryml.hpp>
#include <ryml_std.hpp>

#include <spdlog/spdlog.h>

#include <hd/sdk.hpp>

using namespace std::string_view_literals;

static std::atomic<bool> g_stop{false};
static void SigHandler(int) { g_stop.store(true); }

// -- Socket helpers ----------------------------------------------------------

static int ListenTcp(uint16_t port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  int reuse = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
             &reuse, sizeof(reuse));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr),
           sizeof(addr)) < 0 ||
      listen(fd, 1) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static int ListenUnix(const char* path) {
  unlink(path);
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path,
          sizeof(addr.sun_path) - 1);
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr),
           sizeof(addr)) < 0 ||
      listen(fd, 1) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static int ListenUdp(uint16_t port) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return -1;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr),
           sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static int ConnectTcp(const char* host, uint16_t port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
    close(fd);
    return -1;
  }
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr),
              sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

// -- Bidirectional pump ------------------------------------------------------

/// Pump between a local fd and an HD tunnel until either
/// side closes or g_stop is set.
static void Pump(int local_fd, hd::sdk::Tunnel& tunnel,
                 bool is_udp = false) {
  std::atomic<bool> alive{true};
  sockaddr_in udp_peer{};
  bool udp_peer_known = false;

  // Tunnel → local.
  tunnel.SetDataCallback(
      [&](std::span<const uint8_t> data) {
    if (!alive.load()) return;
    if (is_udp && udp_peer_known) {
      sendto(local_fd, data.data(), data.size(), 0,
             reinterpret_cast<sockaddr*>(&udp_peer),
             sizeof(udp_peer));
    } else {
      int total = 0;
      while (total < static_cast<int>(data.size())) {
        int w = write(local_fd, data.data() + total,
                      data.size() - total);
        if (w <= 0) { alive.store(false); return; }
        total += w;
      }
    }
  });

  // Local → tunnel.
  uint8_t buf[65536];
  while (alive.load() && !g_stop.load()) {
    pollfd pfd{local_fd, POLLIN, 0};
    int ret = poll(&pfd, 1, 200);
    if (ret < 0 && errno != EINTR) break;
    if (ret <= 0) continue;
    if (pfd.revents & (POLLERR | POLLHUP)) break;

    int n;
    if (is_udp) {
      sockaddr_in from{};
      socklen_t flen = sizeof(from);
      n = recvfrom(local_fd, buf, sizeof(buf), 0,
                   reinterpret_cast<sockaddr*>(&from),
                   &flen);
      if (n > 0 && !udp_peer_known) {
        udp_peer = from;
        udp_peer_known = true;
      }
    } else {
      n = read(local_fd, buf, sizeof(buf));
    }
    if (n <= 0) break;

    auto sr = tunnel.Send(
        std::span<const uint8_t>(buf, n));
    if (!sr) break;
  }

  tunnel.Close();
}

// -- Stdio pump --------------------------------------------------------------

static void StdioPump(hd::sdk::Tunnel& tunnel) {
  // Tunnel → stdout.
  tunnel.SetDataCallback(
      [](std::span<const uint8_t> data) {
    fwrite(data.data(), 1, data.size(), stdout);
    fflush(stdout);
  });

  // Stdin → tunnel.
  uint8_t buf[65536];
  while (!g_stop.load()) {
    pollfd pfd{STDIN_FILENO, POLLIN, 0};
    int ret = poll(&pfd, 1, 200);
    if (ret < 0 && errno != EINTR) break;
    if (ret <= 0) continue;
    if (pfd.revents & (POLLERR | POLLHUP)) break;

    int n = read(STDIN_FILENO, buf, sizeof(buf));
    if (n <= 0) break;

    auto sr = tunnel.Send(
        std::span<const uint8_t>(buf, n));
    if (!sr) break;
  }
  tunnel.Close();
}

// -- YAML config loader ------------------------------------------------------

struct HdcatConfig {
  std::string relay_url;
  std::string relay_key;
  std::string peer;
  std::string listen;
  std::string connect;
  std::string key_path;
  bool tls = true;
};

static std::string YamlStr(ryml::ConstNodeRef node,
                           const char* key,
                           const char* def = "") {
  if (!node.has_child(ryml::to_csubstr(key))) return def;
  auto child = node[ryml::to_csubstr(key)];
  if (!child.has_val()) return def;
  std::string val;
  child >> val;
  return val;
}

static bool LoadConfig(const char* path,
                       HdcatConfig* out) {
  FILE* f = fopen(path, "r");
  if (!f) {
    fprintf(stderr, "error: cannot open %s\n", path);
    return false;
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size <= 0 || size > 65536) { fclose(f); return false; }
  std::string buf(size, '\0');
  if (fread(buf.data(), 1, size, f) !=
      static_cast<size_t>(size)) {
    fclose(f);
    return false;
  }
  fclose(f);

  ryml::Tree tree = ryml::parse_in_arena(
      ryml::to_csubstr(buf));
  auto root = tree.rootref();

  if (root.has_child("relay")) {
    auto r = root["relay"];
    out->relay_url = YamlStr(r, "url");
    if (out->relay_url.empty())
      out->relay_url = YamlStr(r, "host");
    out->relay_key = YamlStr(r, "key");
    out->key_path = YamlStr(r, "key_path");
    std::string tls_str = YamlStr(r, "tls", "true");
    out->tls = (tls_str != "false" && tls_str != "0");
  }

  out->peer = YamlStr(root, "peer");
  out->listen = YamlStr(root, "listen");
  out->connect = YamlStr(root, "connect");
  return true;
}

// -- Main --------------------------------------------------------------------

static void PrintUsage() {
  fprintf(stderr,
      "Usage: hdcat [options]\n"
      "  --config FILE   YAML config file\n"
      "  -r HOST:PORT    Relay address\n"
      "  -k KEY          Relay key (hex)\n"
      "  -p PEER         Peer name or ID\n"
      "  -l SPEC         Listen: tcp:PORT, udp:PORT, "
      "unix:PATH\n"
      "  -c SPEC         Connect: tcp:HOST:PORT\n"
      "  --no-tls        Disable TLS\n"
      "  -h              Help\n"
      "\n"
      "Config YAML:\n"
      "  relay:\n"
      "    url: \"hd://relay:3341\"\n"
      "    key: \"aabbcc...\"\n"
      "  peer: \"camera-01\"\n"
      "  listen: \"tcp:8080\"\n"
      "\n"
      "Modes:\n"
      "  (no -l/-c)      Pipe stdin/stdout to peer\n"
      "  -l tcp:8080     Accept TCP, bridge to peer\n"
      "  -l udp:9000     Receive UDP, bridge to peer\n"
      "  -l unix:/tmp/s  Accept unix, bridge to peer\n"
      "  -c tcp:h:p      Connect TCP, bridge to peer\n");
}

int main(int argc, char** argv) {
  HdcatConfig hc;

  // Load config file first if specified.
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
      if (!LoadConfig(argv[i + 1], &hc)) return 1;
      break;
    }
  }

  // CLI flags override config.
  for (int i = 1; i < argc; i++) {
    auto a = std::string_view(argv[i]);
    if (a == "--config"sv && i + 1 < argc) {
      i++;
    } else if (a == "-r"sv && i + 1 < argc) {
      hc.relay_url = argv[++i];
    } else if (a == "-k"sv && i + 1 < argc) {
      hc.relay_key = argv[++i];
    } else if (a == "-p"sv && i + 1 < argc) {
      hc.peer = argv[++i];
    } else if (a == "-l"sv && i + 1 < argc) {
      hc.listen = argv[++i];
    } else if (a == "-c"sv && i + 1 < argc) {
      hc.connect = argv[++i];
    } else if (a == "--no-tls"sv) {
      hc.tls = false;
    } else if (a == "-h"sv || a == "--help"sv) {
      PrintUsage();
      return 0;
    }
  }

  if (hc.relay_url.empty() || hc.relay_key.empty() ||
      hc.peer.empty()) {
    fprintf(stderr, "error: relay url, key, and peer "
            "required (via --config or -r/-k/-p)\n");
    PrintUsage();
    return 1;
  }

  const char* peer = hc.peer.c_str();
  const char* listen_spec =
      hc.listen.empty() ? nullptr : hc.listen.c_str();
  const char* connect_spec =
      hc.connect.empty() ? nullptr : hc.connect.c_str();

  signal(SIGINT, SigHandler);
  signal(SIGTERM, SigHandler);
  signal(SIGPIPE, SIG_IGN);

  // Suppress spdlog output in pipe mode to keep stdout
  // clean. Logging goes to stderr via the process.
  spdlog::set_level(spdlog::level::off);

  // Connect to relay.
  hd::sdk::ClientConfig cfg;
  cfg.relay_url = hc.relay_url;
  cfg.relay_key = hc.relay_key;
  cfg.key_path = hc.key_path;
  cfg.tls = hc.tls;

  auto result = hd::sdk::Client::Create(cfg);
  if (!result) {
    fprintf(stderr, "connect: %s\n",
            result.error().message.c_str());
    return 1;
  }
  auto& client = *result;
  client.Start();

  // Wait for peer discovery.
  // peer can be: "*" (first peer), numeric ID, name, or
  // hex key prefix.
  bool wildcard = (strcmp(peer, "*") == 0);
  fprintf(stderr, "hdcat: waiting for peer '%s'...\n",
          peer);
  std::string matched_name;
  for (int i = 0; i < 100 && !g_stop.load(); i++) {
    usleep(100000);
    auto peers = client.ListPeers();
    for (auto& p : peers) {
      if (wildcard ||
          p.name == peer ||
          std::to_string(p.peer_id) == peer ||
          p.name.starts_with(peer)) {
        matched_name = p.name.empty()
            ? std::to_string(p.peer_id) : p.name;
        peer = matched_name.c_str();
        goto found;
      }
    }
  }
  fprintf(stderr, "hdcat: peer '%s' not found\n", peer);
  client.Stop();
  return 1;

found:
  auto tr = client.Open(peer);
  if (!tr) {
    fprintf(stderr, "hdcat: open: %s\n",
            tr.error().message.c_str());
    client.Stop();
    return 1;
  }
  auto& tunnel = *tr;
  fprintf(stderr, "hdcat: tunnel open to %s (id=%d)\n",
          tunnel.PeerName().c_str(), tunnel.PeerId());

  if (listen_spec) {
    auto spec = std::string_view(listen_spec);

    if (spec.starts_with("tcp:"sv)) {
      uint16_t port = static_cast<uint16_t>(
          atoi(spec.data() + 4));
      int lfd = ListenTcp(port);
      if (lfd < 0) {
        fprintf(stderr, "hdcat: listen tcp:%d: %s\n",
                port, strerror(errno));
        client.Stop();
        return 1;
      }
      fprintf(stderr, "hdcat: listening on tcp:%d\n", port);
      while (!g_stop.load()) {
        pollfd pfd{lfd, POLLIN, 0};
        if (poll(&pfd, 1, 500) <= 0) continue;
        int cfd = accept(lfd, nullptr, nullptr);
        if (cfd < 0) continue;
        fprintf(stderr, "hdcat: connection accepted\n");
        Pump(cfd, tunnel);
        close(cfd);
      }
      close(lfd);

    } else if (spec.starts_with("udp:"sv)) {
      uint16_t port = static_cast<uint16_t>(
          atoi(spec.data() + 4));
      int ufd = ListenUdp(port);
      if (ufd < 0) {
        fprintf(stderr, "hdcat: listen udp:%d: %s\n",
                port, strerror(errno));
        client.Stop();
        return 1;
      }
      fprintf(stderr, "hdcat: listening on udp:%d\n", port);
      Pump(ufd, tunnel, true);
      close(ufd);

    } else if (spec.starts_with("unix:"sv)) {
      const char* path = spec.data() + 5;
      int lfd = ListenUnix(path);
      if (lfd < 0) {
        fprintf(stderr, "hdcat: listen unix:%s: %s\n",
                path, strerror(errno));
        client.Stop();
        return 1;
      }
      fprintf(stderr, "hdcat: listening on unix:%s\n",
              path);
      while (!g_stop.load()) {
        pollfd pfd{lfd, POLLIN, 0};
        if (poll(&pfd, 1, 500) <= 0) continue;
        int cfd = accept(lfd, nullptr, nullptr);
        if (cfd < 0) continue;
        fprintf(stderr, "hdcat: connection accepted\n");
        Pump(cfd, tunnel);
        close(cfd);
      }
      close(lfd);
      unlink(path);
    }

  } else if (connect_spec) {
    auto spec = std::string_view(connect_spec);
    if (spec.starts_with("tcp:"sv)) {
      auto rest = spec.substr(4);
      auto colon = rest.rfind(':');
      if (colon == std::string_view::npos) {
        fprintf(stderr, "hdcat: -c tcp:HOST:PORT\n");
        client.Stop();
        return 1;
      }
      auto host = std::string(rest.substr(0, colon));
      uint16_t port = static_cast<uint16_t>(
          atoi(rest.data() + colon + 1));
      int cfd = ConnectTcp(host.c_str(), port);
      if (cfd < 0) {
        fprintf(stderr, "hdcat: connect tcp:%s:%d: %s\n",
                host.c_str(), port, strerror(errno));
        client.Stop();
        return 1;
      }
      fprintf(stderr, "hdcat: connected to tcp:%s:%d\n",
              host.c_str(), port);
      Pump(cfd, tunnel);
      close(cfd);
    }

  } else {
    // Stdio mode.
    fprintf(stderr, "hdcat: piping stdin/stdout\n");
    StdioPump(tunnel);
  }

  client.Stop();
  return 0;
}
