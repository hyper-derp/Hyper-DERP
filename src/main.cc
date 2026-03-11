/// @file main.cc
/// @brief Hyper-DERP relay server entry point.

#include <spdlog/spdlog.h>

#include <csignal>
#include <cstdlib>
#include <string_view>

#include "hyper_derp/server.h"

static hyper_derp::Server g_server;

static void SignalHandler(int sig) {
  (void)sig;
  hyper_derp::ServerStop(&g_server);
}

/// Parse comma-separated core list (e.g. "0,2,4,6")
/// into pin_cores array. Returns count parsed.
static int ParsePinCores(const char* spec, int* out,
                         int max) {
  int count = 0;
  const char* p = spec;
  while (*p && count < max) {
    char* end;
    long val = strtol(p, &end, 10);
    if (end == p) break;
    out[count++] = static_cast<int>(val);
    if (*end == ',') {
      p = end + 1;
    } else {
      break;
    }
  }
  return count;
}

int main(int argc, char* argv[]) {
  using namespace std::string_view_literals;

  uint16_t port = 3340;
  int num_workers = 0;
  int sockbuf_size = 0;
  uint16_t metrics_port = 0;
  const char* tls_cert = nullptr;
  const char* tls_key = nullptr;
  const char* pin_spec = nullptr;

  for (int i = 1; i < argc; i++) {
    std::string_view arg = argv[i];
    if (arg == "--port"sv && i + 1 < argc) {
      port = static_cast<uint16_t>(
          std::atoi(argv[++i]));
    } else if (arg == "--workers"sv && i + 1 < argc) {
      num_workers = std::atoi(argv[++i]);
    } else if (arg == "--pin-workers"sv &&
               i + 1 < argc) {
      pin_spec = argv[++i];
    } else if (arg == "--sockbuf"sv && i + 1 < argc) {
      sockbuf_size = std::atoi(argv[++i]);
    } else if (arg == "--metrics-port"sv &&
               i + 1 < argc) {
      metrics_port = static_cast<uint16_t>(
          std::atoi(argv[++i]));
    } else if (arg == "--tls-cert"sv && i + 1 < argc) {
      tls_cert = argv[++i];
    } else if (arg == "--tls-key"sv && i + 1 < argc) {
      tls_key = argv[++i];
    }
  }

  spdlog::info("hyper-derp starting on port {}", port);

  hyper_derp::ServerConfig config;
  config.port = port;
  config.num_workers = num_workers;
  config.sockbuf_size = sockbuf_size;
  config.metrics.port = metrics_port;
  if (tls_cert) config.metrics.tls_cert = tls_cert;
  if (tls_key) config.metrics.tls_key = tls_key;

  if (pin_spec) {
    int n = ParsePinCores(pin_spec,
                          config.pin_cores.data(),
                          hyper_derp::kMaxWorkers);
    if (num_workers == 0) {
      config.num_workers = n;
    }
    spdlog::info("pin-workers: {} cores specified", n);
  }

  auto init = hyper_derp::ServerInit(&g_server, &config);
  if (!init) {
    spdlog::error("init failed: {} ({})",
                  init.error().message,
                  hyper_derp::ServerErrorName(
                      init.error().code));
    return EXIT_FAILURE;
  }

  signal(SIGINT, SignalHandler);
  signal(SIGTERM, SignalHandler);
  signal(SIGPIPE, SIG_IGN);

  auto run = hyper_derp::ServerRun(&g_server);

  hyper_derp::ServerDestroy(&g_server);
  spdlog::info("hyper-derp exiting");
  return run.has_value() ? EXIT_SUCCESS : EXIT_FAILURE;
}
