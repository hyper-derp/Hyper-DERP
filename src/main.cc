/// @file main.cc
/// @brief Hyper-DERP relay server entry point.

#include <spdlog/spdlog.h>

#include <csignal>
#include <cstdlib>

#include "hyper_derp/server.h"

static hyper_derp::Server g_server;

static void SignalHandler(int sig) {
  (void)sig;
  hyper_derp::ServerStop(&g_server);
}

int main(int argc, char* argv[]) {
  uint16_t port = 3340;
  int num_workers = 0;  // auto

  // Minimal arg parsing.
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      port = static_cast<uint16_t>(atoi(argv[++i]));
    } else if (strcmp(argv[i], "--workers") == 0 &&
               i + 1 < argc) {
      num_workers = atoi(argv[++i]);
    }
  }

  spdlog::info("hyper-derp starting on port {}", port);

  hyper_derp::ServerConfig config{};
  config.port = port;
  config.num_workers = num_workers;

  if (hyper_derp::ServerInit(&g_server, &config) < 0) {
    return EXIT_FAILURE;
  }

  signal(SIGINT, SignalHandler);
  signal(SIGTERM, SignalHandler);
  signal(SIGPIPE, SIG_IGN);

  int rc = hyper_derp::ServerRun(&g_server);

  hyper_derp::ServerDestroy(&g_server);
  spdlog::info("hyper-derp exiting");
  return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
