/// @file server.cc
/// @brief DERP relay server: TCP accept loop, HTTP upgrade,
///   handshake, and data plane hand-off.

#include "hyper_derp/server.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <expected>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

#include "hyper_derp/control_plane.h"
#include "hyper_derp/data_plane.h"
#include "hyper_derp/error.h"
#include "hyper_derp/http.h"

namespace hyper_derp {

// Rate-limited warning: at most once per second.
static std::chrono::steady_clock::time_point
    g_last_hs_warn;
static uint64_t g_hs_warn_suppressed = 0;
static std::chrono::steady_clock::time_point
    g_last_rl_warn;
static uint64_t g_rl_warn_suppressed = 0;

static void WarnHandshake(int fd,
                          const std::string& msg,
                          const std::string_view& code) {
  auto now = std::chrono::steady_clock::now();
  if (now - g_last_hs_warn < std::chrono::seconds(1)) {
    g_hs_warn_suppressed++;
    return;
  }
  g_last_hs_warn = now;
  if (g_hs_warn_suppressed > 0) {
    spdlog::warn(
        "handshake failed for fd {}: {} ({}) "
        "[{} similar warnings suppressed]",
        fd, msg, code, g_hs_warn_suppressed);
    g_hs_warn_suppressed = 0;
  } else {
    spdlog::warn("handshake failed for fd {}: {} ({})",
                 fd, msg, code);
  }
}

static void WarnRateLimit(uint64_t rejected) {
  auto now = std::chrono::steady_clock::now();
  if (now - g_last_rl_warn < std::chrono::seconds(1)) {
    g_rl_warn_suppressed++;
    return;
  }
  g_last_rl_warn = now;
  if (g_rl_warn_suppressed > 0) {
    spdlog::warn(
        "rate limit: rejected {} connections "
        "[{} similar warnings suppressed]",
        rejected, g_rl_warn_suppressed);
    g_rl_warn_suppressed = 0;
  } else {
    spdlog::warn("rate limit: rejected {} connections",
                 rejected);
  }
}

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

    auto result = ParseHttpRequest(buf, total, req);
    if (result.has_value()) {
      return 0;
    }
    if (result.error().code ==
        HttpParseError::BadRequest) {
      return -1;
    }
    // Incomplete: keep reading.
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

using std::string_view_literals::operator""sv;

// Handle a single accepted connection.
static void HandleConnection(Server* server, int fd) {
  SetTcpNodelay(fd);

  // Set a 10-second deadline for the HTTP + handshake
  // phase.
  timeval tv{.tv_sec = 10, .tv_usec = 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv,
             sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv,
             sizeof(tv));

  // TLS handshake + kTLS installation (if enabled).
  // OpenSSL auto-installs kTLS during SSL_accept.
  // After this, read()/write() on fd operate on
  // plaintext — the kernel handles AES-GCM.
  if (server->ktls_enabled) {
    auto tls = KtlsAccept(&server->ktls_ctx, fd);
    if (!tls) {
      spdlog::debug("kTLS failed for fd {}: {} ({})",
                    fd, tls.error().message,
                    KtlsErrorName(tls.error().code));
      close(fd);
      return;
    }
  }

  HttpRequest req;
  if (ReadHttpRequest(fd, &req) < 0) {
    close(fd);
    return;
  }

  // Route by path.
  if (req.path == "/derp/probe"sv ||
      req.path == "/derp/latency-check"sv) {
    uint8_t resp[256];
    int n = WriteProbeResponse(resp, sizeof(resp));
    if (n > 0) {
      SendAndClose(fd, resp, n);
    } else {
      close(fd);
    }
    return;
  }

  if (req.path == "/generate_204"sv) {
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

  if (req.path != "/derp"sv) {
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

  // Perform DERP handshake (ServerKey -> ClientInfo ->
  // ServerInfo).
  ClientInfo info;
  auto hs = PerformHandshake(fd, &server->keys, &info);
  if (!hs) {
    WarnHandshake(fd, hs.error().message,
                  HandshakeErrorName(hs.error().code));
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
  tv = {.tv_sec = 0, .tv_usec = 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv,
             sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv,
             sizeof(tv));

  // Hand the authenticated fd to the data plane.
  DpAddPeer(&server->data_plane, fd, info.public_key);

  // Register with the control plane (notifies watchers).
  CpOnPeerConnect(&server->control_plane,
                  info.public_key, fd);
}

static void AcceptLoop(Server* server) {
  spdlog::info("accept loop started on port {}",
               server->config.port);

  const int limit = server->config.max_accept_per_sec;
  auto window_start = std::chrono::steady_clock::now();
  int window_count = 0;
  uint64_t total_rejected = 0;

  while (server->running.load(
             std::memory_order_acquire)) {
    sockaddr_in addr{};
    socklen_t addr_len = sizeof(addr);
    int fd = accept(server->listen_fd,
                    reinterpret_cast<sockaddr*>(&addr),
                    &addr_len);
    if (fd < 0) {
      if (errno == EINTR || errno == EAGAIN) {
        continue;
      }
      if (server->running.load(
              std::memory_order_acquire)) {
        spdlog::error("accept failed: {}",
                      strerror(errno));
      }
      break;
    }

    // Token bucket rate limiting.
    if (limit > 0) {
      auto now = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<
          std::chrono::seconds>(now - window_start);
      if (elapsed.count() >= 1) {
        window_start = now;
        window_count = 0;
      }
      if (window_count >= limit) {
        close(fd);
        total_rejected++;
        WarnRateLimit(total_rejected);
        // Sleep 1ms to avoid busy-spinning on a flood.
        struct timespec ts{.tv_sec = 0,
                           .tv_nsec = 1000000};
        nanosleep(&ts, nullptr);
        continue;
      }
      window_count++;
    }

    HandleConnection(server, fd);
  }
}

auto ServerInit(Server* server,
                const ServerConfig* config)
    -> std::expected<void, Error<ServerError>> {
  server->config = *config;
  server->listen_fd = -1;
  server->running.store(0, std::memory_order_relaxed);

  auto keys = GenerateServerKeys(&server->keys);
  if (!keys) {
    spdlog::error("failed to generate server keys: {}",
                  keys.error().message);
    return MakeError(ServerError::KeyGenFailed,
                     keys.error().message);
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
    return MakeError(ServerError::DataPlaneInitFailed,
                     "DpInit failed");
  }

  // Apply configuration to data plane context.
  server->data_plane.sockbuf_size = config->sockbuf_size;
  server->data_plane.sqpoll = config->sqpoll ? 1 : 0;
  for (int i = 0; i < num_workers && i < kMaxWorkers;
       i++) {
    server->data_plane.pin_cores[i] =
        config->pin_cores[i];
  }

  CpInit(&server->control_plane, &server->data_plane);

  // Initialize kTLS if cert + key are configured.
  if (!config->tls_cert.empty() &&
      !config->tls_key.empty()) {
    auto probe = ProbeKtls();
    if (!probe) {
      spdlog::error("kTLS not available: {}",
                    probe.error().message);
      DpDestroy(&server->data_plane);
      return MakeError(ServerError::KtlsInitFailed,
                       probe.error().message);
    }
    auto init = KtlsCtxInit(
        &server->ktls_ctx,
        config->tls_cert.c_str(),
        config->tls_key.c_str());
    if (!init) {
      spdlog::error("kTLS ctx init failed: {}",
                    init.error().message);
      DpDestroy(&server->data_plane);
      return MakeError(ServerError::KtlsInitFailed,
                       init.error().message);
    }
    server->ktls_enabled = true;
    spdlog::info("kTLS enabled (TLS 1.3 AES-GCM)");
  }

  // Create TCP listener.
  int lfd = socket(AF_INET, SOCK_STREAM, 0);
  if (lfd < 0) {
    spdlog::error("socket: {}", strerror(errno));
    DpDestroy(&server->data_plane);
    return MakeError(ServerError::SocketFailed,
                     strerror(errno));
  }

  int opt = 1;
  setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt,
             sizeof(opt));
  setsockopt(lfd, SOL_SOCKET, SO_REUSEPORT, &opt,
             sizeof(opt));

  sockaddr_in bind_addr{};
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_addr.s_addr = INADDR_ANY;
  bind_addr.sin_port = htons(config->port);

  if (bind(lfd,
           reinterpret_cast<sockaddr*>(&bind_addr),
           sizeof(bind_addr)) < 0) {
    spdlog::error("bind: {}", strerror(errno));
    close(lfd);
    DpDestroy(&server->data_plane);
    return MakeError(ServerError::BindFailed,
                     strerror(errno));
  }

  if (listen(lfd, 4096) < 0) {
    spdlog::error("listen: {}", strerror(errno));
    close(lfd);
    DpDestroy(&server->data_plane);
    return MakeError(ServerError::ListenFailed,
                     strerror(errno));
  }

  server->listen_fd = lfd;
  server->running.store(1, std::memory_order_release);
  return {};
}

auto ServerRun(Server* server,
               std::atomic<int>* stop_flag)
    -> std::expected<void, Error<ServerError>> {
  // Start control plane thread.
  try {
    server->control_thread = std::thread(
        CpRunLoop, &server->control_plane);
  } catch (const std::system_error&) {
    spdlog::error("failed to start control thread");
    return MakeError(ServerError::ThreadCreateFailed,
                     "control thread");
  }

  // Start accept thread.
  try {
    server->accept_thread = std::thread(
        AcceptLoop, server);
  } catch (const std::system_error&) {
    spdlog::error("failed to start accept thread");
    CpStop(&server->control_plane);
    server->control_thread.join();
    return MakeError(ServerError::ThreadCreateFailed,
                     "accept thread");
  }

  spdlog::info("data plane running with {} workers",
               server->data_plane.num_workers);

  // Start metrics server (if configured).
  server->metrics_server = MetricsStart(
      server->config.metrics, &server->data_plane);

  // Poll for external stop signal in a background
  // thread. This bridges the async-signal-safe flag
  // into the normal shutdown path.
  std::thread stop_poller;
  if (stop_flag) {
    stop_poller = std::thread([server, stop_flag]() {
      while (!stop_flag->load(
                 std::memory_order_acquire)) {
        struct timespec ts {
          .tv_sec = 0, .tv_nsec = 100000000
        };
        nanosleep(&ts, nullptr);
      }
      ServerStop(server);
    });
  }

  // Run data plane (blocks until DpStop is called).
  int rc = DpRun(&server->data_plane);

  // Ensure the stop poller exits.
  if (stop_flag) {
    stop_flag->store(1, std::memory_order_release);
  }
  if (stop_poller.joinable()) {
    stop_poller.join();
  }

  // Wait for accept thread to finish.
  server->running.store(0, std::memory_order_release);
  // Closing listen_fd unblocks accept().
  if (server->listen_fd >= 0) {
    shutdown(server->listen_fd, SHUT_RDWR);
    close(server->listen_fd);
    server->listen_fd = -1;
  }
  if (server->accept_thread.joinable()) {
    server->accept_thread.join();
  }

  // Stop and join the control thread.
  CpStop(&server->control_plane);
  if (server->control_thread.joinable()) {
    server->control_thread.join();
  }

  if (rc < 0) {
    return MakeError(ServerError::DataPlaneRunFailed,
                     "DpRun returned error");
  }
  return {};
}

void ServerStop(Server* server) {
  // Guard against double-stop.
  int expected = 1;
  if (!server->running.compare_exchange_strong(
          expected, 0, std::memory_order_acq_rel)) {
    return;
  }

  // Dump stats before stopping.
  uint64_t sd, xd, se, rb, sb;
  DpGetStats(&server->data_plane,
             &sd, &xd, &se, &rb, &sb);
  spdlog::info(
      "stats: send_drops={} xfer_drops={} "
      "slab_exhausts={} recv_bytes={} send_bytes={}",
      sd, xd, se, rb, sb);

  // Detailed send error breakdown.
  uint64_t ep = 0, ecr = 0, eag = 0, eoth = 0;
  for (int i = 0; i < server->data_plane.num_workers;
       i++) {
    Worker* w = server->data_plane.workers[i];
    if (!w) continue;
    ep += __atomic_load_n(&w->stats.send_epipe,
                          __ATOMIC_RELAXED);
    ecr += __atomic_load_n(&w->stats.send_econnreset,
                           __ATOMIC_RELAXED);
    eag += __atomic_load_n(&w->stats.send_eagain,
                           __ATOMIC_RELAXED);
    eoth += __atomic_load_n(&w->stats.send_other_err,
                            __ATOMIC_RELAXED);
  }
  spdlog::info(
      "send errors: epipe={} econnreset={} eagain={} "
      "other={}",
      ep, ecr, eag, eoth);

  uint64_t fph = 0, fpm = 0;
  for (int i = 0; i < server->data_plane.num_workers;
       i++) {
    Worker* w = server->data_plane.workers[i];
    if (!w) continue;
    fph += __atomic_load_n(&w->stats.frame_pool_hits,
                           __ATOMIC_RELAXED);
    fpm += __atomic_load_n(&w->stats.frame_pool_misses,
                           __ATOMIC_RELAXED);
  }
  spdlog::info("frame pool: hits={} misses={}", fph, fpm);

  // Stop metrics server before data plane teardown.
  MetricsStop(server->metrics_server);
  server->metrics_server = nullptr;

  CpStop(&server->control_plane);
  DpStop(&server->data_plane);
  if (server->listen_fd >= 0) {
    shutdown(server->listen_fd, SHUT_RDWR);
  }
}

void ServerDestroy(Server* server) {
  MetricsStop(server->metrics_server);
  server->metrics_server = nullptr;
  if (server->ktls_enabled) {
    KtlsCtxDestroy(&server->ktls_ctx);
    server->ktls_enabled = false;
  }
  CpDestroy(&server->control_plane);
  DpDestroy(&server->data_plane);
  if (server->listen_fd >= 0) {
    close(server->listen_fd);
    server->listen_fd = -1;
  }
}

}  // namespace hyper_derp
