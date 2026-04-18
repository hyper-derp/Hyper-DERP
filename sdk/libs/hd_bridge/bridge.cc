/// @file bridge.cc
/// @brief TCP/unix socket ↔ HD tunnel bridge.

#include "hd/bridge/bridge.h"

#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

namespace hd::bridge {

// -- Address parsing ---------------------------------------------------------

struct ParsedAddr {
  enum Type { kTcp, kUnix } type;
  std::string host;
  uint16_t port = 0;
  std::string path;  // Unix socket path.
};

static bool ParseAddress(const std::string& addr,
                         ParsedAddr* out) {
  if (addr.starts_with("tcp://")) {
    out->type = ParsedAddr::kTcp;
    auto rest = addr.substr(6);
    auto colon = rest.rfind(':');
    if (colon == std::string::npos) return false;
    out->host = rest.substr(0, colon);
    out->port = static_cast<uint16_t>(
        std::stoi(rest.substr(colon + 1)));
    return true;
  }
  if (addr.starts_with("unix://")) {
    out->type = ParsedAddr::kUnix;
    out->path = addr.substr(7);
    return true;
  }
  return false;
}

// -- Socket helpers ----------------------------------------------------------

static int BindTcp(const char* host, uint16_t port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  int reuse = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
             &reuse, sizeof(reuse));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  inet_pton(AF_INET, host, &addr.sin_addr);

  if (bind(fd, reinterpret_cast<sockaddr*>(&addr),
           sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  if (listen(fd, 16) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static int BindUnix(const char* path) {
  unlink(path);
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path,
          sizeof(addr.sun_path) - 1);

  if (bind(fd, reinterpret_cast<sockaddr*>(&addr),
           sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  if (listen(fd, 16) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

// -- Connection pump ---------------------------------------------------------

/// Bidirectional byte pump between a local socket fd and
/// an HD tunnel. Runs until either side closes.
static void ConnectionPump(int local_fd,
                           hd::sdk::Tunnel tunnel) {
  // Set up tunnel → local forwarding via callback.
  std::atomic<bool> running{true};
  int write_fd = local_fd;

  tunnel.SetDataCallback(
      [&](std::span<const uint8_t> data) {
    if (!running.load()) return;
    int total = 0;
    while (total < static_cast<int>(data.size())) {
      int w = write(write_fd, data.data() + total,
                    data.size() - total);
      if (w <= 0) {
        running.store(false);
        return;
      }
      total += w;
    }
  });

  // Local → tunnel: read and send.
  uint8_t buf[4096];
  while (running.load()) {
    pollfd pfd{local_fd, POLLIN, 0};
    int ret = poll(&pfd, 1, 200);
    if (ret < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (ret == 0) continue;
    if (pfd.revents & (POLLERR | POLLHUP)) break;

    int n = read(local_fd, buf, sizeof(buf));
    if (n <= 0) break;

    auto sr = tunnel.Send(
        std::span<const uint8_t>(buf, n));
    if (!sr) break;
  }

  tunnel.Close();
  close(local_fd);
}

// -- Accept loop -------------------------------------------------------------

static void AcceptLoop(Listener* l,
                       hd::sdk::Client* client) {
  spdlog::info("bridge listening on {} -> {}",
               l->address, l->peer_name);

  while (l->running.load()) {
    pollfd pfd{l->fd, POLLIN, 0};
    int ret = poll(&pfd, 1, 500);
    if (ret <= 0) continue;

    int conn_fd = accept(l->fd, nullptr, nullptr);
    if (conn_fd < 0) continue;

    // Open tunnel for this connection.
    auto tr = client->Open(l->peer_name);
    if (!tr) {
      spdlog::warn("bridge: cannot open tunnel to {}: {}",
                   l->peer_name, tr.error().message);
      close(conn_fd);
      continue;
    }

    // Spawn connection thread.
    l->conn_threads.emplace_back(
        ConnectionPump, conn_fd, std::move(*tr));
  }

  close(l->fd);
  l->fd = -1;
}

// -- Bridge ------------------------------------------------------------------

Bridge::Bridge(hd::sdk::Client& client)
    : client_(client) {}

Bridge::~Bridge() {
  Stop();
}

hd::sdk::Result<> Bridge::Listen(
    const std::string& address,
    const std::string& peer_name) {
  ParsedAddr pa;
  if (!ParseAddress(address, &pa)) {
    return std::unexpected(hd::sdk::MakeError(
        hd::sdk::ErrorCode::kInitFailed,
        "invalid address: " + address));
  }

  int fd = -1;
  if (pa.type == ParsedAddr::kTcp) {
    fd = BindTcp(pa.host.c_str(), pa.port);
  } else {
    fd = BindUnix(pa.path.c_str());
  }
  if (fd < 0) {
    return std::unexpected(hd::sdk::MakeError(
        hd::sdk::ErrorCode::kInitFailed,
        "bind failed: " + address + ": " +
            strerror(errno)));
  }

  auto l = std::make_unique<Listener>();
  l->address = address;
  l->peer_name = peer_name;
  l->fd = fd;
  l->running.store(true);
  l->accept_thread = std::thread(
      AcceptLoop, l.get(), &client_);
  listeners_.push_back(std::move(l));
  return {};
}

void Bridge::Stop() {
  for (auto& l : listeners_) {
    l->running.store(false);
    if (l->accept_thread.joinable())
      l->accept_thread.join();
    for (auto& t : l->conn_threads) {
      if (t.joinable()) t.join();
    }
    if (l->fd >= 0) {
      close(l->fd);
      l->fd = -1;
    }
  }
  listeners_.clear();
}

}  // namespace hd::bridge
