/// @file harness.cc
/// @brief Integration test harness implementation.

#include "harness.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sched.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstring>

#include "hyper_derp/hd_peers.h"
#include "hyper_derp/server.h"

namespace hyper_derp {
namespace test {

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

    // Re-install SIGTERM to stop cleanly.
    static Server* g_srv = &server;
    struct sigaction sa = {};
    sa.sa_handler = [](int) { ServerStop(g_srv); };
    sigaction(SIGTERM, &sa, nullptr);

    (void)ServerRun(&server);
    ServerDestroy(&server);
    _exit(0);
  }

  return pid;
}

pid_t StartHdRelay(uint16_t port, int num_workers,
                   const Key& relay_key) {
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

    Server server;
    if (!ServerInit(&server, &config)) {
      _exit(1);
    }

    static Server* g_srv = &server;
    struct sigaction sa = {};
    sa.sa_handler = [](int) { ServerStop(g_srv); };
    sigaction(SIGTERM, &sa, nullptr);

    (void)ServerRun(&server);
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
