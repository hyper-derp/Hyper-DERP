/// @file harness.cc
/// @brief Integration test harness implementation.

#include "harness.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sched.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "hyper_derp/hd_peers.h"
#include "hyper_derp/server.h"

namespace hyper_derp {
namespace test {

// HD mode requires TLS (server.cc enforces it). The harness
// generates a throwaway self-signed cert once per process
// the first time StartHdRelay is called.
struct TestCertPaths {
  std::string cert;
  std::string key;
};

static const TestCertPaths& EnsureTestCert() {
  static TestCertPaths paths = [] {
    TestCertPaths p;
    std::string dir = "/tmp/hyper-derp-test-" +
                      std::to_string(getpid());
    std::filesystem::create_directories(dir);
    p.cert = dir + "/relay.crt";
    p.key = dir + "/relay.key";
    std::string cmd =
        "openssl req -x509 -newkey rsa:2048 "
        "-keyout " + p.key + " -out " + p.cert + " "
        "-days 1 -nodes -subj '/CN=hd-test' "
        ">/dev/null 2>&1";
    if (std::system(cmd.c_str()) != 0) {
      std::fprintf(stderr,
                   "harness: openssl cert gen failed\n");
      std::abort();
    }
    return p;
  }();
  return paths;
}

auto ConnectHdClient(HdClient* c, const char* host,
                     uint16_t port)
    -> std::expected<void, Error<HdClientError>> {
  auto tcp = HdClientConnect(c, host, port);
  if (!tcp) return tcp;
  return HdClientTlsConnect(c);
}

uint16_t FindFreePort() {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return 0;
  }

  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;

  if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr),
           sizeof(addr)) < 0) {
    close(fd);
    return 0;
  }

  socklen_t len = sizeof(addr);
  if (getsockname(
          fd, reinterpret_cast<struct sockaddr*>(&addr),
          &len) < 0) {
    close(fd);
    return 0;
  }

  uint16_t port = ntohs(addr.sin_port);
  close(fd);
  return port;
}

pid_t StartRelay(uint16_t port, int num_workers,
                 const char* tls_cert,
                 const char* tls_key) {
  pid_t pid = fork();
  if (pid < 0) {
    return -1;
  }

  if (pid == 0) {
    // Child: reset signal handlers inherited from GTest.
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);

    ServerConfig config;
    config.port = port;
    config.num_workers = num_workers;
    if (tls_cert && tls_key) {
      config.tls_cert = tls_cert;
      config.tls_key = tls_key;
    }

    Server server;
    if (!ServerInit(&server, &config)) {
      _exit(1);
    }

    // Use stop_flag pattern: signal handler sets flag,
    // ServerRun's stop_poller drains + stops cleanly.
    static std::atomic<int> stop_flag{0};
    struct sigaction sa = {};
    sa.sa_handler = [](int) {
      stop_flag.store(1, std::memory_order_release);
    };
    sigaction(SIGTERM, &sa, nullptr);

    (void)ServerRun(&server, &stop_flag);
    ServerDestroy(&server);
    _exit(0);
  }

  return pid;
}

pid_t StartHdRelay(uint16_t port, int num_workers,
                   const Key& relay_key,
                   uint16_t metrics_port,
                   uint16_t hd_relay_id,
                   const char* seed_host,
                   uint16_t seed_port,
                   const char* local_fleet_id,
                   const char* accept_fleet_id,
                   const char* einheit_ctl) {
  // Generate once in the parent so every forked child
  // reads the same paths (static locals don't survive the
  // fork, but the files on disk do).
  const auto& cert = EnsureTestCert();
  pid_t pid = fork();
  if (pid < 0) {
    return -1;
  }

  if (pid == 0) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);

    ServerConfig config;
    config.port = port;
    config.num_workers = num_workers;

    // Convert relay key to hex string for config.
    char hex[kKeySize * 2 + 1];
    for (int i = 0; i < kKeySize; i++) {
      static const char digits[] = "0123456789abcdef";
      hex[i * 2] = digits[relay_key[i] >> 4];
      hex[i * 2 + 1] = digits[relay_key[i] & 0x0f];
    }
    hex[kKeySize * 2] = '\0';
    config.hd_relay_key = hex;
    config.hd_enroll_mode = HdEnrollMode::kAutoApprove;
    config.tls_cert = cert.cert;
    config.tls_key = cert.key;
    if (metrics_port != 0) {
      config.metrics.port = metrics_port;
      config.metrics.enable_debug = true;
    }
    config.hd_relay_id = hd_relay_id;
    if (seed_host && seed_host[0] != '\0' &&
        seed_port != 0) {
      char seed[128];
      std::snprintf(seed, sizeof(seed), "%s:%u",
                    seed_host, seed_port);
      config.seed_relays.emplace_back(seed);
    }
    if (local_fleet_id && local_fleet_id[0] != '\0') {
      config.hd_federation_policy.local_fleet_id =
          local_fleet_id;
    }
    if (accept_fleet_id && accept_fleet_id[0] != '\0') {
      HdFederationAccept rule;
      rule.fleet_id = accept_fleet_id;
      config.hd_federation_policy.accept_from.push_back(
          std::move(rule));
    }
    if (einheit_ctl && einheit_ctl[0] != '\0') {
      config.einheit_ctl_endpoint = einheit_ctl;
    }

    Server server;
    if (!ServerInit(&server, &config)) {
      _exit(1);
    }

    // Use stop_flag pattern: signal handler sets flag,
    // ServerRun's stop_poller drains + stops cleanly.
    static std::atomic<int> stop_flag{0};
    struct sigaction sa = {};
    sa.sa_handler = [](int) {
      stop_flag.store(1, std::memory_order_release);
    };
    sigaction(SIGTERM, &sa, nullptr);

    (void)ServerRun(&server, &stop_flag);
    ServerDestroy(&server);
    _exit(0);
  }

  return pid;
}

int WaitRelayReady(uint16_t port, int timeout_ms) {
  int attempts = timeout_ms / 10;
  for (int i = 0; i < attempts; i++) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      return -1;
    }

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    if (connect(
            fd,
            reinterpret_cast<struct sockaddr*>(&addr),
            sizeof(addr)) == 0) {
      close(fd);
      return 0;
    }
    close(fd);
    usleep(10000);
  }
  return -1;
}

void StopRelay(pid_t pid) {
  kill(pid, SIGTERM);

  // Wait up to 5 seconds for clean exit.
  for (int i = 0; i < 500; i++) {
    int status;
    pid_t w = waitpid(pid, &status, WNOHANG);
    if (w > 0) {
      return;
    }
    usleep(10000);
  }

  // Force kill if still alive.
  kill(pid, SIGKILL);
  waitpid(pid, nullptr, 0);
}

int PinToCore(int core) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core, &cpuset);
  return sched_setaffinity(0, sizeof(cpuset), &cpuset);
}

}  // namespace test
}  // namespace hyper_derp
