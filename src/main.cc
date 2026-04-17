/// @file main.cc
/// @brief Hyper-DERP relay server entry point.

#include <spdlog/spdlog.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <string_view>

#include "hyper_derp/config.h"
#include "hyper_derp/server.h"

static std::atomic<int> g_stop_flag{0};

static void SignalHandler(int /*sig*/) {
  g_stop_flag.store(1, std::memory_order_release);
}

static void PrintUsage(const char* prog) {
  std::println(
      "Usage: {} [options]\n"
      "\n"
      "Options:\n"
      "  --config <path>       YAML config file\n"
      "  --port <port>         Listen port (default: 3340)\n"
      "  --workers <n>         Worker threads (0=auto)\n"
      "  --pin-workers <list>  Pin workers to cores "
      "(e.g. 0,2,4,6)\n"
      "  --sockbuf <bytes>     Socket buffer size "
      "(0=OS default)\n"
      "  --metrics-port <port> Metrics HTTP port "
      "(0=disabled)\n"
      "  --tls-cert <path>     TLS certificate for metrics\n"
      "  --tls-key <path>      TLS key for metrics\n"
      "  --max-accept-rate <n> Max accepts/sec "
      "(0=unlimited)\n"
      "  --sqpoll              Use io_uring SQPOLL mode "
      "(needs CAP_SYS_NICE)\n"
      "  --debug-endpoints     Enable /debug/* metrics "
      "endpoints\n"
      "  --log-level <level>   Log level "
      "(debug|info|warn|error)\n"
      "  --hd-relay-key <hex>  HD relay shared secret "
      "(enables HD)\n"
      "  --hd-enroll-mode <m>  HD enrollment mode "
      "(manual|auto)\n"
      "  --level2              Enable Level 2 direct "
      "path (ICE/TURN/XDP)\n"
      "  --stun-port <port>    STUN listen port "
      "(default: 3478)\n"
      "  --xdp-interface <nic> Network interface for "
      "XDP attachment\n"
      "  --help                Show this help\n"
      "  --version             Show version",
      prog);
}

/// Parse comma-separated core list (e.g. "0,2,4,6")
/// into pin_cores array. Returns count parsed.
static int ParsePinCores(const char* spec, int* out,
                         int max) {
  int count = 0;
  const char* p = spec;
  while (*p && count < max) {
    char* end;
    errno = 0;
    long val = strtol(p, &end, 10);
    if (end == p || errno == ERANGE || val < 0 ||
        val > 1023) {
      break;
    }
    out[count++] = static_cast<int>(val);
    if (*end == ',') {
      p = end + 1;
    } else {
      break;
    }
  }
  return count;
}

/// Parse an integer from a string with validation.
/// Returns true on success.
static bool ParseInt(const char* str, int* out,
                     int min_val, int max_val) {
  char* end;
  errno = 0;
  long val = strtol(str, &end, 10);
  if (end == str || *end != '\0' || errno == ERANGE ||
      val < min_val || val > max_val) {
    return false;
  }
  *out = static_cast<int>(val);
  return true;
}

int main(int argc, char* argv[]) {
  using std::string_view_literals::operator""sv;

  const char* config_path = nullptr;
  int port = -1;
  int num_workers = -1;
  int sockbuf_size = -1;
  int max_accept_rate = -1;
  int metrics_port = -1;
  const char* tls_cert = nullptr;
  const char* tls_key = nullptr;
  const char* pin_spec = nullptr;
  const char* log_level = nullptr;
  const char* hd_relay_key = nullptr;
  const char* hd_enroll_mode = nullptr;
  const char* xdp_interface = nullptr;
  int stun_port = -1;
  bool debug_endpoints = false;
  bool sqpoll = false;
  bool level2 = false;
  bool debug_set = false;
  bool sqpoll_set = false;
  bool level2_set = false;

  for (int i = 1; i < argc; i++) {
    std::string_view arg = argv[i];
    if (arg == "--help"sv || arg == "-h"sv) {
      PrintUsage(argv[0]);
      return EXIT_SUCCESS;
    }
    if (arg == "--version"sv) {
      std::println("hyper-derp {}", HD_VERSION);
      return EXIT_SUCCESS;
    }
    if (arg == "--config"sv && i + 1 < argc) {
      config_path = argv[++i];
    } else if (arg == "--port"sv && i + 1 < argc) {
      if (!ParseInt(argv[++i], &port, 1, 65535)) {
        std::println(stderr, "error: invalid --port");
        return EXIT_FAILURE;
      }
    } else if (arg == "--workers"sv && i + 1 < argc) {
      if (!ParseInt(argv[++i], &num_workers, 0,
                    hyper_derp::kMaxWorkers)) {
        std::println(stderr, "error: invalid --workers "
                     "(0-{})", hyper_derp::kMaxWorkers);
        return EXIT_FAILURE;
      }
    } else if (arg == "--pin-workers"sv &&
               i + 1 < argc) {
      pin_spec = argv[++i];
    } else if (arg == "--sockbuf"sv && i + 1 < argc) {
      if (!ParseInt(argv[++i], &sockbuf_size, 0,
                    256 * 1024 * 1024)) {
        std::println(stderr,
                     "error: invalid --sockbuf");
        return EXIT_FAILURE;
      }
    } else if (arg == "--max-accept-rate"sv &&
               i + 1 < argc) {
      if (!ParseInt(argv[++i], &max_accept_rate, 0,
                    1000000)) {
        std::println(stderr,
                     "error: invalid --max-accept-rate "
                     "(0-1000000)");
        return EXIT_FAILURE;
      }
    } else if (arg == "--metrics-port"sv &&
               i + 1 < argc) {
      if (!ParseInt(argv[++i], &metrics_port, 0,
                    65535)) {
        std::println(stderr,
                     "error: invalid --metrics-port");
        return EXIT_FAILURE;
      }
    } else if (arg == "--tls-cert"sv && i + 1 < argc) {
      tls_cert = argv[++i];
    } else if (arg == "--tls-key"sv && i + 1 < argc) {
      tls_key = argv[++i];
    } else if (arg == "--debug-endpoints"sv) {
      debug_endpoints = true;
      debug_set = true;
    } else if (arg == "--sqpoll"sv) {
      sqpoll = true;
      sqpoll_set = true;
    } else if (arg == "--log-level"sv && i + 1 < argc) {
      log_level = argv[++i];
    } else if (arg == "--hd-relay-key"sv &&
               i + 1 < argc) {
      hd_relay_key = argv[++i];
    } else if (arg == "--hd-enroll-mode"sv &&
               i + 1 < argc) {
      hd_enroll_mode = argv[++i];
    } else if (arg == "--level2"sv) {
      level2 = true;
      level2_set = true;
    } else if (arg == "--stun-port"sv &&
               i + 1 < argc) {
      if (!ParseInt(argv[++i], &stun_port, 1, 65535)) {
        std::println(stderr,
                     "error: invalid --stun-port");
        return EXIT_FAILURE;
      }
    } else if (arg == "--xdp-interface"sv &&
               i + 1 < argc) {
      xdp_interface = argv[++i];
    } else {
      std::println(stderr,
                   "error: unknown option '{}'", arg);
      PrintUsage(argv[0]);
      return EXIT_FAILURE;
    }
  }

  // Configure log level.
  if (log_level) {
    std::string_view lv = log_level;
    if (lv == "debug"sv) {
      spdlog::set_level(spdlog::level::debug);
    } else if (lv == "info"sv) {
      spdlog::set_level(spdlog::level::info);
    } else if (lv == "warn"sv) {
      spdlog::set_level(spdlog::level::warn);
    } else if (lv == "error"sv) {
      spdlog::set_level(spdlog::level::err);
    } else {
      std::println(stderr,
                   "error: invalid --log-level "
                   "(debug|info|warn|error)");
      return EXIT_FAILURE;
    }
  }

  // Validate TLS config.
  if ((tls_cert && !tls_key) ||
      (!tls_cert && tls_key)) {
    std::println(stderr, "error: --tls-cert and "
                 "--tls-key must both be specified");
    return EXIT_FAILURE;
  }

  // Load config: YAML file first, CLI flags override.
  hyper_derp::ServerConfig config;

  if (config_path) {
    auto cfg = hyper_derp::LoadConfig(config_path,
                                      &config);
    if (!cfg) {
      std::println(stderr, "error: config: {} ({})",
                   cfg.error().message,
                   hyper_derp::ConfigErrorName(
                       cfg.error().code));
      return EXIT_FAILURE;
    }
    spdlog::info("loaded config from {}", config_path);
  }

  // CLI flags override config file values.
  if (port >= 0)
    config.port = static_cast<uint16_t>(port);
  if (num_workers >= 0)
    config.num_workers = num_workers;
  if (sockbuf_size >= 0)
    config.sockbuf_size = sockbuf_size;
  if (max_accept_rate >= 0)
    config.max_accept_per_sec = max_accept_rate;
  if (metrics_port >= 0)
    config.metrics.port =
        static_cast<uint16_t>(metrics_port);
  if (tls_cert) {
    config.tls_cert = tls_cert;
    config.tls_key = tls_key;
  }
  if (debug_set)
    config.metrics.enable_debug = debug_endpoints;
  if (sqpoll_set)
    config.sqpoll = sqpoll;

  if (hd_relay_key)
    config.hd_relay_key = hd_relay_key;
  if (hd_enroll_mode) {
    std::string_view mode = hd_enroll_mode;
    if (mode == "auto"sv) {
      config.hd_enroll_mode =
          hyper_derp::HdEnrollMode::kAutoApprove;
    } else if (mode == "manual"sv) {
      config.hd_enroll_mode =
          hyper_derp::HdEnrollMode::kManual;
    } else {
      std::println(stderr,
                   "error: invalid --hd-enroll-mode "
                   "(manual|auto)");
      return EXIT_FAILURE;
    }
  }

  if (level2_set)
    config.level2.enabled = level2;
  if (stun_port >= 0)
    config.level2.stun_port =
        static_cast<uint16_t>(stun_port);
  if (xdp_interface)
    config.level2.xdp_interface = xdp_interface;

  if (pin_spec) {
    int n = ParsePinCores(pin_spec,
                          config.pin_cores.data(),
                          hyper_derp::kMaxWorkers);
    if (config.num_workers == 0) {
      config.num_workers = n;
    }
    spdlog::info("pin-workers: {} cores specified", n);
  }

  spdlog::info("hyper-derp starting on port {}",
               config.port);

  hyper_derp::Server server;
  auto init = hyper_derp::ServerInit(&server, &config);
  if (!init) {
    spdlog::error("init failed: {} ({})",
                  init.error().message,
                  hyper_derp::ServerErrorName(
                      init.error().code));
    return EXIT_FAILURE;
  }

  // Install signal handlers. Only set the atomic flag;
  // ServerRun polls it and calls ServerStop from a safe
  // context.
  struct sigaction sa {};
  sa.sa_handler = SignalHandler;
  sa.sa_flags = 0;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  signal(SIGPIPE, SIG_IGN);

  auto run = hyper_derp::ServerRun(&server, &g_stop_flag);

  hyper_derp::ServerDestroy(&server);
  spdlog::info("hyper-derp exiting");
  return run.has_value() ? EXIT_SUCCESS : EXIT_FAILURE;
}
