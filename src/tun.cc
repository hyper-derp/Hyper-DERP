/// @file tun.cc
/// @brief Linux TUN device implementation.
///
/// Uses the kernel TUN/TAP interface (/dev/net/tun) with
/// IFF_TUN | IFF_NO_PI for raw IP packet I/O. Address
/// assignment and link-up use ioctl (SIOCSIFADDR,
/// SIOCSIFFLAGS).

#include "hyper_derp/tun.h"

#include <spdlog/spdlog.h>

#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace hyper_derp {

int TunOpen(TunDevice* tun, const char* name_hint) {
  memset(tun, 0, sizeof(*tun));
  tun->fd = -1;

  int fd = open("/dev/net/tun", O_RDWR);
  if (fd < 0) {
    spdlog::error("open /dev/net/tun: {}",
                  strerror(errno));
    return -1;
  }

  struct ifreq ifr = {};
  ifr.ifr_flags = IFF_TUN | IFF_NO_PI;

  if (name_hint) {
    strncpy(ifr.ifr_name, name_hint,
            sizeof(ifr.ifr_name) - 1);
  } else {
    strncpy(ifr.ifr_name, "derp%d",
            sizeof(ifr.ifr_name) - 1);
  }

  if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
    spdlog::error("ioctl TUNSETIFF: {}",
                  strerror(errno));
    close(fd);
    return -1;
  }

  tun->fd = fd;
  strncpy(tun->name, ifr.ifr_name,
          sizeof(tun->name) - 1);
  tun->name[sizeof(tun->name) - 1] = '\0';

  spdlog::info("TUN device {} opened (fd={})",
               tun->name, tun->fd);
  return 0;
}

int TunSetAddr(TunDevice* tun, uint32_t addr,
               int prefix_len) {
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    spdlog::error("socket: {}", strerror(errno));
    return -1;
  }

  struct ifreq ifr = {};
  strncpy(ifr.ifr_name, tun->name,
          sizeof(ifr.ifr_name) - 1);

  // Set address.
  auto* sin = reinterpret_cast<struct sockaddr_in*>(
      &ifr.ifr_addr);
  sin->sin_family = AF_INET;
  sin->sin_addr.s_addr = addr;
  if (ioctl(sock, SIOCSIFADDR, &ifr) < 0) {
    spdlog::error("ioctl SIOCSIFADDR: {}",
                  strerror(errno));
    close(sock);
    return -1;
  }

  // Set netmask.
  uint32_t mask = 0;
  if (prefix_len > 0) {
    mask = htonl(~((1u << (32 - prefix_len)) - 1));
  }
  sin->sin_addr.s_addr = mask;
  if (ioctl(sock, SIOCSIFNETMASK, &ifr) < 0) {
    spdlog::error("ioctl SIOCSIFNETMASK: {}",
                  strerror(errno));
    close(sock);
    return -1;
  }

  close(sock);

  char ip_str[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &addr, ip_str, sizeof(ip_str));
  spdlog::info("TUN {} addr {}/{}", tun->name, ip_str,
               prefix_len);
  return 0;
}

int TunBringUp(TunDevice* tun) {
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    spdlog::error("socket: {}", strerror(errno));
    return -1;
  }

  struct ifreq ifr = {};
  strncpy(ifr.ifr_name, tun->name,
          sizeof(ifr.ifr_name) - 1);

  // Get current flags.
  if (ioctl(sock, SIOCGIFFLAGS, &ifr) < 0) {
    spdlog::error("ioctl SIOCGIFFLAGS: {}",
                  strerror(errno));
    close(sock);
    return -1;
  }

  // Set IFF_UP | IFF_RUNNING.
  ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
  if (ioctl(sock, SIOCSIFFLAGS, &ifr) < 0) {
    spdlog::error("ioctl SIOCSIFFLAGS: {}",
                  strerror(errno));
    close(sock);
    return -1;
  }

  close(sock);
  spdlog::info("TUN {} up", tun->name);
  return 0;
}

void TunClose(TunDevice* tun) {
  if (tun->fd >= 0) {
    close(tun->fd);
    tun->fd = -1;
  }
}

int ParseCidr(const char* cidr, uint32_t* addr,
              int* prefix_len) {
  // Copy to mutable buffer for strtok.
  char buf[64];
  strncpy(buf, cidr, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  char* slash = strchr(buf, '/');
  if (!slash) {
    return -1;
  }
  *slash = '\0';

  struct in_addr in;
  if (inet_pton(AF_INET, buf, &in) != 1) {
    return -1;
  }
  *addr = in.s_addr;

  char* endptr;
  long pl = strtol(slash + 1, &endptr, 10);
  if (*endptr != '\0' || pl < 0 || pl > 32) {
    return -1;
  }
  *prefix_len = static_cast<int>(pl);
  return 0;
}

}  // namespace hyper_derp
