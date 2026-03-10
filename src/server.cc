/// @file server.cc
/// @brief DERP relay server: TCP accept loop, HTTP upgrade,
///   handshake, and data plane hand-off.

#include "hyper_derp/server.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "hyper_derp/http.h"

namespace hyper_derp {

static void SetTcpNodelay(int fd) {
  int flag = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag,
             sizeof(flag));
}

// Read an HTTP request from a connected socket. Handles
// probe and generate_204 endpoints inline. Returns the
// parsed request on success.
static int ReadHttpRequest(int fd, HttpRequest* req) {
  uint8_t buf[kMaxHttpRequestSize];
  int total = 0;

  while (total < kMaxHttpRequestSize) {
    int n = read(fd, buf + total,
                 kMaxHttpRequestSize - total);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (n == 0) {
      return -1;
    }
    total += n;

    int rc = ParseHttpRequest(buf, total, req);
    if (rc > 0) {
      return 0;
    }
    if (rc == -2) {
      return -1;
    }
    // rc == -1: incomplete, keep reading.
  }
  return -1;
}

// Send an HTTP response and close the fd.
static void SendAndClose(int fd, const uint8_t* data,
                         int len) {
  int total = 0;
  while (total < len) {
    int w = write(fd, data + total, len - total);
    if (w <= 0) {
      break;
    }
    total += w;
  }
  close(fd);
}

// Handle a single accepted connection.
static void HandleConnection(Server* server, int fd) {
  SetTcpNodelay(fd);

  // Set a 10-second deadline for the HTTP + handshake phase.
  struct timeval tv;
  tv.tv_sec = 10;
  tv.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv,
             sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv,
             sizeof(tv));

  HttpRequest req;
  if (ReadHttpRequest(fd, &req) < 0) {
    close(fd);
    return;
  }

  // Route by path.
  if (strcmp(req.path, "/derp/probe") == 0 ||
      strcmp(req.path, "/derp/latency-check") == 0) {
    uint8_t resp[256];
    int n = WriteProbeResponse(resp, sizeof(resp));
    if (n > 0) {
      SendAndClose(fd, resp, n);
    } else {
      close(fd);
    }
    return;
  }

  if (strcmp(req.path, "/generate_204") == 0) {
    uint8_t resp[512];
    int n = WriteNoContentResponse(
        resp, sizeof(resp), nullptr);
    if (n > 0) {
      SendAndClose(fd, resp, n);
    } else {
      close(fd);
    }
    return;
  }

  if (strcmp(req.path, "/derp") != 0) {
    uint8_t resp[256];
    int n = WriteErrorResponse(resp, sizeof(resp), 404,
                               "not found");
    if (n > 0) {
      SendAndClose(fd, resp, n);
    } else {
      close(fd);
    }
    return;
  }

  // Must be an upgrade request.
  if (!req.has_upgrade) {
    uint8_t resp[256];
    int n = WriteErrorResponse(
        resp, sizeof(resp), 426,
        "DERP requires connection upgrade");
    if (n > 0) {
      SendAndClose(fd, resp, n);
    } else {
      close(fd);
    }
    return;
  }

  // Send HTTP 101 Switching Protocols.
  if (!req.fast_start) {
    char hex[kKeySize * 2 + 1];
    KeyToHex(server->keys.public_key, hex);

    uint8_t resp[512];
    int n = WriteUpgradeResponse(resp, sizeof(resp), hex);
    if (n < 0) {
      close(fd);
      return;
    }
    int total = 0;
    while (total < n) {
      int w = write(fd, resp + total, n - total);
      if (w <= 0) {
        close(fd);
        return;
      }
      total += w;
    }
  }

  // Perform DERP handshake (ServerKey → ClientInfo →
  // ServerInfo).
  ClientInfo info;
  if (PerformHandshake(fd, &server->keys, &info) < 0) {
    spdlog::warn("handshake failed for fd {}", fd);
    close(fd);
    return;
  }

  char hex[kKeySize * 2 + 1];
  KeyToHex(info.public_key, hex);
  spdlog::info("peer connected: {} (v{}, ack_pings={})",
               hex, info.version,
               info.can_ack_pings);

  // Clear the handshake timeouts before handing to the
  // data plane.
  tv.tv_sec = 0;
  tv.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv,
             sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv,
             sizeof(tv));

  // Hand the authenticated fd to the data plane.
  DpAddPeer(&server->data_plane, fd, info.public_key);
}

static void* AcceptLoop(void* arg) {
  auto* server = static_cast<Server*>(arg);

  spdlog::info("accept loop started on port {}",
               server->config.port);

  while (__atomic_load_n(&server->running,
                         __ATOMIC_ACQUIRE)) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int fd = accept(server->listen_fd,
                    reinterpret_cast<struct sockaddr*>(
                        &addr),
                    &addr_len);
    if (fd < 0) {
      if (errno == EINTR || errno == EAGAIN) {
        continue;
      }
      if (__atomic_load_n(&server->running,
                          __ATOMIC_ACQUIRE)) {
        spdlog::error("accept failed: {}",
                      strerror(errno));
      }
      break;
    }

    HandleConnection(server, fd);
  }

  return nullptr;
}

int ServerInit(Server* server,
               const ServerConfig* config) {
  memset(server, 0, sizeof(*server));
  server->config = *config;
  server->listen_fd = -1;

  if (GenerateServerKeys(&server->keys) < 0) {
    spdlog::error("failed to generate server keys");
    return -1;
  }

  char hex[kKeySize * 2 + 1];
  KeyToHex(server->keys.public_key, hex);
  spdlog::info("server public key: {}", hex);

  int num_workers = config->num_workers;
  if (num_workers <= 0) {
    num_workers = static_cast<int>(
        sysconf(_SC_NPROCESSORS_ONLN));
    if (num_workers < 1) {
      num_workers = 1;
    }
  }

  if (DpInit(&server->data_plane, num_workers) < 0) {
    spdlog::error("failed to initialize data plane");
    return -1;
  }

  // Create TCP listener.
  int lfd = socket(AF_INET, SOCK_STREAM, 0);
  if (lfd < 0) {
    spdlog::error("socket: {}", strerror(errno));
    DpDestroy(&server->data_plane);
    return -1;
  }

  int opt = 1;
  setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt,
             sizeof(opt));
  setsockopt(lfd, SOL_SOCKET, SO_REUSEPORT, &opt,
             sizeof(opt));

  struct sockaddr_in bind_addr;
  memset(&bind_addr, 0, sizeof(bind_addr));
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_addr.s_addr = INADDR_ANY;
  bind_addr.sin_port = htons(config->port);

  if (bind(lfd,
           reinterpret_cast<struct sockaddr*>(&bind_addr),
           sizeof(bind_addr)) < 0) {
    spdlog::error("bind: {}", strerror(errno));
    close(lfd);
    DpDestroy(&server->data_plane);
    return -1;
  }

  if (listen(lfd, 128) < 0) {
    spdlog::error("listen: {}", strerror(errno));
    close(lfd);
    DpDestroy(&server->data_plane);
    return -1;
  }

  server->listen_fd = lfd;
  server->running = 1;
  return 0;
}

int ServerRun(Server* server) {
  // Start accept thread.
  int rc = pthread_create(&server->accept_thread, nullptr,
                          AcceptLoop, server);
  if (rc != 0) {
    spdlog::error("failed to start accept thread");
    return -1;
  }

  spdlog::info("data plane running with {} workers",
               server->data_plane.num_workers);

  // Run data plane (blocks until DpStop is called).
  rc = DpRun(&server->data_plane);

  // Wait for accept thread to finish.
  __atomic_store_n(&server->running, 0,
                   __ATOMIC_RELEASE);
  // Closing listen_fd unblocks accept().
  if (server->listen_fd >= 0) {
    shutdown(server->listen_fd, SHUT_RDWR);
    close(server->listen_fd);
    server->listen_fd = -1;
  }
  pthread_join(server->accept_thread, nullptr);

  return rc;
}

void ServerStop(Server* server) {
  __atomic_store_n(&server->running, 0,
                   __ATOMIC_RELEASE);
  DpStop(&server->data_plane);
  if (server->listen_fd >= 0) {
    shutdown(server->listen_fd, SHUT_RDWR);
  }
}

void ServerDestroy(Server* server) {
  DpDestroy(&server->data_plane);
  if (server->listen_fd >= 0) {
    close(server->listen_fd);
    server->listen_fd = -1;
  }
}

}  // namespace hyper_derp
