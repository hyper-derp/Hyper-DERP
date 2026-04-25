/// @file wg_relay.cc
/// @brief Transparent UDP relay for stock WireGuard.

#include "hyper_derp/wg_relay.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <linux/if_link.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include <spdlog/spdlog.h>

namespace hyper_derp {

namespace {

uint64_t NowNs() {
  return std::chrono::duration_cast<
             std::chrono::nanoseconds>(
             std::chrono::steady_clock::now()
                 .time_since_epoch())
      .count();
}

bool ParseHostPort(const std::string& s,
                    sockaddr_storage* out, socklen_t* len) {
  auto colon = s.rfind(':');
  if (colon == std::string::npos) return false;
  std::string host = s.substr(0, colon);
  std::string port = s.substr(colon + 1);
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_flags = AI_NUMERICSERV;
  addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), port.c_str(), &hints,
                   &res) != 0) {
    return false;
  }
  std::memcpy(out, res->ai_addr, res->ai_addrlen);
  *len = res->ai_addrlen;
  freeaddrinfo(res);
  return true;
}

bool SockaddrEqual(const sockaddr_storage& a, socklen_t la,
                    const sockaddr_storage& b,
                    socklen_t lb) {
  if (la != lb) return false;
  return std::memcmp(&a, &b, la) == 0;
}

// Compare against an IPv4 endpoint when the incoming
// packet arrived as v4-mapped over an AF_INET6 socket.
bool EndpointMatches(const WgRelayPeer& p,
                      const sockaddr_storage& src,
                      socklen_t src_len) {
  if (SockaddrEqual(p.endpoint, p.endpoint_len, src,
                     src_len)) {
    return true;
  }
  // V4-mapped fallback: peer registered as v4 but socket
  // is v6-dual-stack so packets show up as v4-mapped v6.
  if (p.endpoint.ss_family == AF_INET &&
      src.ss_family == AF_INET6) {
    const auto* p4 = reinterpret_cast<const sockaddr_in*>(
        &p.endpoint);
    const auto* s6 = reinterpret_cast<const sockaddr_in6*>(
        &src);
    if (IN6_IS_ADDR_V4MAPPED(&s6->sin6_addr) &&
        s6->sin6_port == p4->sin_port) {
      uint32_t v4 = 0;
      std::memcpy(&v4, &s6->sin6_addr.s6_addr[12], 4);
      return v4 == p4->sin_addr.s_addr;
    }
  }
  return false;
}

// -- Roster persistence --------------------------------

// Format: each non-comment line is one record.
//   peer <name> <ip:port> [pubkey-base64] [label]
//   link <name-a> <name-b>
// Tab-tolerant; whitespace-separated. Reload-safe order:
// peers come first so links can reference them.
std::string SerializeRoster(
    const std::vector<WgRelayPeer>& peers,
    const std::vector<WgRelayLink>& links) {
  std::string out;
  out += "# wg-relay roster — managed by `wg peer add` "
         "/ `wg link add`\n";
  out += "# format:\n";
  out += "#   peer <name> <ip:port> [pubkey-b64] [label]\n";
  out += "#   link <name-a> <name-b>\n";
  out += "#   nic <peer-name> <iface>\n";
  for (const auto& p : peers) {
    out += "peer ";
    out += p.name;
    out += ' ';
    out += p.endpoint_str;
    if (!p.pubkey_b64.empty()) {
      out += ' ';
      out += p.pubkey_b64;
      if (!p.label.empty()) {
        out += ' ';
        out += p.label;
      }
    }
    out += '\n';
  }
  for (const auto& l : links) {
    out += "link ";
    out += l.a;
    out += ' ';
    out += l.b;
    out += '\n';
  }
  // NIC bindings on a separate line — keeps the `peer`
  // line backward-compatible with iteration-1 rosters.
  for (const auto& p : peers) {
    if (!p.nic.empty()) {
      out += "nic ";
      out += p.name;
      out += ' ';
      out += p.nic;
      out += '\n';
    }
  }
  return out;
}

bool WriteRosterAtomic(const std::string& path,
                        const std::string& body) {
  if (path.empty()) return true;
  std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    if (!f) return false;
    f << body;
  }
  return ::rename(tmp.c_str(), path.c_str()) == 0;
}

// In-place add/remove operating on already-locked tables.
// Return false on duplicate name / unknown peer / link
// invariant violations.

bool PeerAddLocked(WgRelay* r, const std::string& name,
                    const std::string& endpoint,
                    const std::string& label) {
  for (const auto& p : r->peers) {
    if (p.name == name) return false;
  }
  WgRelayPeer p;
  p.name = name;
  if (!ParseHostPort(endpoint, &p.endpoint,
                      &p.endpoint_len)) {
    return false;
  }
  p.endpoint_str = endpoint;
  p.label = label;
  r->peers.push_back(std::move(p));
  return true;
}

bool PeerKeyLocked(WgRelay* r, const std::string& name,
                    const std::string& pubkey_b64) {
  for (auto& p : r->peers) {
    if (p.name == name) {
      p.pubkey_b64 = pubkey_b64;
      return true;
    }
  }
  return false;
}

bool PeerNicLocked(WgRelay* r, const std::string& name,
                    const std::string& nic) {
  for (auto& p : r->peers) {
    if (p.name == name) {
      p.nic = nic;
      return true;
    }
  }
  return false;
}

bool PeerRemoveLocked(WgRelay* r,
                       const std::string& name) {
  for (auto it = r->links.begin();
       it != r->links.end();) {
    if (it->a == name || it->b == name) {
      it = r->links.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = r->peers.begin();
       it != r->peers.end(); ++it) {
    if (it->name == name) {
      r->peers.erase(it);
      return true;
    }
  }
  return false;
}

bool PeerHasLink(const std::vector<WgRelayLink>& links,
                  const std::string& name) {
  for (const auto& l : links) {
    if (l.a == name || l.b == name) return true;
  }
  return false;
}

bool PeerKnown(const std::vector<WgRelayPeer>& peers,
                const std::string& name) {
  for (const auto& p : peers) {
    if (p.name == name) return true;
  }
  return false;
}

// Returns 0 on success, or:
//   1 = unknown peer, 2 = self-link,
//   3 = peer already linked, 4 = duplicate link
int LinkAddLocked(WgRelay* r, const std::string& a,
                   const std::string& b) {
  if (a == b) return 2;
  if (!PeerKnown(r->peers, a) ||
      !PeerKnown(r->peers, b)) {
    return 1;
  }
  for (const auto& l : r->links) {
    if ((l.a == a && l.b == b) ||
        (l.a == b && l.b == a)) {
      return 4;
    }
  }
  if (PeerHasLink(r->links, a) ||
      PeerHasLink(r->links, b)) {
    return 3;
  }
  r->links.push_back({a, b});
  return 0;
}

bool LinkRemoveLocked(WgRelay* r, const std::string& a,
                       const std::string& b) {
  for (auto it = r->links.begin();
       it != r->links.end(); ++it) {
    if ((it->a == a && it->b == b) ||
        (it->a == b && it->b == a)) {
      r->links.erase(it);
      return true;
    }
  }
  return false;
}

void LoadRosterFile(WgRelay* r,
                     const std::string& path) {
  std::ifstream f(path);
  if (!f.is_open()) return;
  int peers = 0, links = 0, bad = 0;
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream iss(line);
    std::string kind;
    iss >> kind;
    if (kind == "peer") {
      std::string name, endpoint, pubkey, label;
      iss >> name >> endpoint;
      if (iss) iss >> pubkey;
      if (iss) {
        std::string rest;
        std::getline(iss, rest);
        while (!rest.empty() && rest.front() == ' ') {
          rest.erase(rest.begin());
        }
        label = rest;
      }
      if (name.empty() || endpoint.empty()) {
        ++bad;
        continue;
      }
      if (PeerAddLocked(r, name, endpoint, label)) {
        if (!pubkey.empty()) {
          PeerKeyLocked(r, name, pubkey);
        }
        ++peers;
      } else {
        ++bad;
      }
    } else if (kind == "link") {
      std::string a, b;
      iss >> a >> b;
      if (a.empty() || b.empty()) {
        ++bad;
        continue;
      }
      int rc = LinkAddLocked(r, a, b);
      if (rc == 0) {
        ++links;
      } else {
        ++bad;
      }
    } else if (kind == "nic") {
      std::string name, iface;
      iss >> name >> iface;
      if (name.empty() || iface.empty()) {
        ++bad;
        continue;
      }
      // Set the in-memory binding only; the BPF maps are
      // populated later when XdpInsertLinkByNameLocked
      // runs for any links that involve this peer.
      if (!PeerNicLocked(r, name, iface)) ++bad;
    } else {
      ++bad;
    }
  }
  spdlog::info(
      "wg-relay roster loaded from {}: {} peers, {} "
      "links, {} bad",
      path, peers, links, bad);
}

void PersistRosterLocked(WgRelay* r) {
  if (r->roster_path.empty()) return;
  std::string body = SerializeRoster(r->peers, r->links);
  if (!WriteRosterAtomic(r->roster_path, body)) {
    spdlog::warn("wg-relay roster write {} failed",
                 r->roster_path);
  }
}

// -- Forward path --------------------------------------

void HandlePacket(WgRelay* r, const uint8_t* pkt,
                   size_t len,
                   const sockaddr_storage& src,
                   socklen_t src_len) {
  r->stats.rx_packets.fetch_add(1,
                                 std::memory_order_relaxed);
  // Lookup is O(N) over the peer table; N is operator-
  // small (dozens), and the lock window covers the
  // sendto so the table can't change underfoot.
  std::lock_guard lk(r->peers_mu);
  WgRelayPeer* src_peer = nullptr;
  for (auto& p : r->peers) {
    if (EndpointMatches(p, src, src_len)) {
      src_peer = &p;
      break;
    }
  }
  if (!src_peer) {
    r->stats.drop_unknown_src.fetch_add(
        1, std::memory_order_relaxed);
    return;
  }
  src_peer->rx_bytes += len;
  src_peer->last_seen_ns = NowNs();

  const WgRelayPeer* dst = nullptr;
  for (const auto& l : r->links) {
    if (l.a == src_peer->name) {
      for (const auto& p : r->peers) {
        if (p.name == l.b) {
          dst = &p;
          break;
        }
      }
      break;
    }
    if (l.b == src_peer->name) {
      for (const auto& p : r->peers) {
        if (p.name == l.a) {
          dst = &p;
          break;
        }
      }
      break;
    }
  }
  if (!dst) {
    r->stats.drop_no_link.fetch_add(
        1, std::memory_order_relaxed);
    return;
  }

  ssize_t sent = sendto(
      r->sock_fd, pkt, len, 0,
      reinterpret_cast<const sockaddr*>(&dst->endpoint),
      dst->endpoint_len);
  if (sent < 0) {
    spdlog::warn("wg-relay sendto: {}",
                 std::strerror(errno));
    return;
  }
  r->stats.fwd_packets.fetch_add(
      1, std::memory_order_relaxed);
  src_peer->fwd_bytes += static_cast<size_t>(sent);
}

void RecvLoop(WgRelay* r) {
  std::vector<uint8_t> buf(2048);
  while (r->running.load(std::memory_order_acquire)) {
    pollfd pfd{r->sock_fd, POLLIN, 0};
    int rc = poll(&pfd, 1, 200);
    if (rc <= 0) continue;
    sockaddr_storage src{};
    socklen_t src_len = sizeof(src);
    ssize_t n = recvfrom(
        r->sock_fd, buf.data(), buf.size(), 0,
        reinterpret_cast<sockaddr*>(&src), &src_len);
    if (n <= 0) continue;
    HandlePacket(r, buf.data(),
                 static_cast<size_t>(n), src, src_len);
  }
}

// -- XDP fast-path helpers -----------------------------

// These structs MUST match bpf/wg_relay.bpf.c byte for
// byte. The BPF program reads/writes them from kernel
// context and the verifier hates layout drift.
#pragma pack(push, 1)
struct XdpEpKey {
  uint32_t ip;
  uint16_t port;
  uint16_t pad;
};
struct XdpEpVal {
  uint32_t ip;
  uint16_t port;
  uint16_t pad;
  uint32_t ifindex;  // egress NIC for traffic to this peer
};
#pragma pack(pop)
static_assert(sizeof(XdpEpKey) == 8, "ep_key layout");
static_assert(sizeof(XdpEpVal) == 12, "ep_val layout");

// Convert a sockaddr_storage (v4 or v4-mapped v6) to the
// (ip, port) pair the BPF program keys on. Returns false
// if the endpoint can't be expressed as plain IPv4.
bool EndpointToV4(const sockaddr_storage& ss, socklen_t len,
                   uint32_t* out_ip, uint16_t* out_port) {
  if (ss.ss_family == AF_INET) {
    const auto* a =
        reinterpret_cast<const sockaddr_in*>(&ss);
    *out_ip = a->sin_addr.s_addr;
    *out_port = a->sin_port;
    return true;
  }
  if (ss.ss_family == AF_INET6) {
    const auto* a =
        reinterpret_cast<const sockaddr_in6*>(&ss);
    if (!IN6_IS_ADDR_V4MAPPED(&a->sin6_addr)) return false;
    std::memcpy(out_ip, &a->sin6_addr.s6_addr[12], 4);
    *out_port = a->sin6_port;
    return true;
  }
  (void)len;
  return false;
}

// Read a NIC's primary IPv4 (first AF_INET on the
// interface) via getifaddrs. Returns network byte order
// in *out, or false if the NIC has no IPv4.
bool ReadNicIpv4(const std::string& iface,
                  uint32_t* out_be) {
  struct ifaddrs* addrs = nullptr;
  if (getifaddrs(&addrs) != 0) return false;
  bool found = false;
  for (auto* a = addrs; a; a = a->ifa_next) {
    if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) {
      continue;
    }
    if (iface != a->ifa_name) continue;
    *out_be = reinterpret_cast<sockaddr_in*>(a->ifa_addr)
                  ->sin_addr.s_addr;
    found = true;
    break;
  }
  freeifaddrs(addrs);
  return found;
}

// Read a NIC's hardware MAC from sysfs. Cleaner than
// SIOCGIFHWADDR — no socket needed, no ioctl quirks.
bool ReadNicMac(const std::string& iface, uint8_t mac[6]) {
  std::string path = "/sys/class/net/" + iface + "/address";
  std::ifstream f(path);
  if (!f) return false;
  std::string s;
  std::getline(f, s);
  unsigned int b[6] = {0};
  if (std::sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &b[0],
                   &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
    return false;
  }
  for (int i = 0; i < 6; ++i) mac[i] = static_cast<uint8_t>(b[i]);
  return true;
}

// Split "ens4f0,ens4f1" into individual interface names.
std::vector<std::string> SplitIfaceList(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ',' || c == ' ') {
      if (!cur.empty()) { out.push_back(cur); cur.clear(); }
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

bool XdpAttach(WgRelay* r, const std::string& iface_list,
                const std::string& bpf_obj_path,
                uint16_t port) {
  auto ifaces = SplitIfaceList(iface_list);
  if (ifaces.empty()) return false;

  // Resolve every NIC up front; bail out before any kernel
  // state mutation if any of them is unknown.
  std::vector<WgXdpAttachment> attachments;
  attachments.reserve(ifaces.size());
  for (const auto& name : ifaces) {
    WgXdpAttachment a;
    a.iface = name;
    a.ifindex = static_cast<int>(if_nametoindex(name.c_str()));
    if (a.ifindex == 0) {
      spdlog::error(
          "wg-relay xdp: interface '{}' not found: {}",
          name, std::strerror(errno));
      return false;
    }
    if (!ReadNicMac(name, a.mac)) {
      spdlog::error(
          "wg-relay xdp: cannot read MAC for '{}'", name);
      return false;
    }
    if (!ReadNicIpv4(name, &a.ipv4_be)) {
      spdlog::warn(
          "wg-relay xdp: '{}' has no IPv4 — cross-NIC "
          "redirect to it will fall back to userspace",
          name);
      // Not fatal; with single-NIC configs ipv4_be is
      // unused. Cross-NIC redirect to this NIC will hit
      // the ENOENT fallback and userspace will handle it.
    }
    attachments.push_back(std::move(a));
  }

  struct bpf_object* obj =
      bpf_object__open_file(bpf_obj_path.c_str(), nullptr);
  if (!obj) {
    spdlog::error("wg-relay xdp: open '{}' failed: {}",
                  bpf_obj_path, std::strerror(errno));
    return false;
  }
  if (bpf_object__load(obj) < 0) {
    spdlog::error("wg-relay xdp: BPF load failed: {}",
                  std::strerror(errno));
    bpf_object__close(obj);
    return false;
  }

  struct bpf_program* prog =
      bpf_object__find_program_by_name(obj, "wg_relay_xdp");
  auto get_map = [&](const char* name) -> struct bpf_map* {
    return bpf_object__find_map_by_name(obj, name);
  };
  auto* peers_map = get_map("wg_peers");
  auto* macs_map = get_map("wg_macs");
  auto* stats_map = get_map("wg_xdp_stats");
  auto* port_map = get_map("wg_port");
  auto* peer_bytes_map = get_map("wg_peer_bytes");
  auto* devmap = get_map("wg_devmap");
  auto* nic_macs_map = get_map("wg_nic_macs");
  auto* nic_ips_map = get_map("wg_nic_ips");
  if (!prog || !peers_map || !macs_map || !stats_map ||
      !port_map || !peer_bytes_map || !devmap ||
      !nic_macs_map || !nic_ips_map) {
    spdlog::error("wg-relay xdp: program/maps not found "
                  "in {}", bpf_obj_path);
    bpf_object__close(obj);
    return false;
  }
  int prog_fd = bpf_program__fd(prog);
  int peers_fd = bpf_map__fd(peers_map);
  int macs_fd = bpf_map__fd(macs_map);
  int stats_fd = bpf_map__fd(stats_map);
  int port_fd = bpf_map__fd(port_map);
  int peer_bytes_fd = bpf_map__fd(peer_bytes_map);
  int devmap_fd = bpf_map__fd(devmap);
  int nic_macs_fd = bpf_map__fd(nic_macs_map);
  int nic_ips_fd = bpf_map__fd(nic_ips_map);

  uint32_t key0 = 0;
  if (bpf_map_update_elem(port_fd, &key0, &port, BPF_ANY) <
      0) {
    spdlog::error("wg-relay xdp: set port: {}",
                  std::strerror(errno));
    bpf_object__close(obj);
    return false;
  }

  // Populate devmap (key=ifindex, value=ifindex) and
  // wg_nic_macs (key=ifindex, value=MAC) for every NIC the
  // BPF program will be attached to. The BPF program needs
  // both for cross-NIC redirect.
  for (const auto& a : attachments) {
    uint32_t k = static_cast<uint32_t>(a.ifindex);
    if (bpf_map_update_elem(devmap_fd, &k, &k, BPF_ANY) <
        0) {
      spdlog::error("wg-relay xdp: devmap[{}] set failed: {}",
                    a.ifindex, std::strerror(errno));
      bpf_object__close(obj);
      return false;
    }
    struct {
      uint8_t mac[6];
      uint16_t pad;
    } macval{};
    std::memcpy(macval.mac, a.mac, 6);
    if (bpf_map_update_elem(nic_macs_fd, &k, &macval,
                             BPF_ANY) < 0) {
      spdlog::error(
          "wg-relay xdp: nic_macs[{}] set failed: {}",
          a.ifindex, std::strerror(errno));
      bpf_object__close(obj);
      return false;
    }
    if (a.ipv4_be != 0) {
      if (bpf_map_update_elem(nic_ips_fd, &k, &a.ipv4_be,
                               BPF_ANY) < 0) {
        spdlog::error(
            "wg-relay xdp: nic_ips[{}] set failed: {}",
            a.ifindex, std::strerror(errno));
        bpf_object__close(obj);
        return false;
      }
    }
  }

  // Attach to each NIC. Native first; generic fallback per
  // NIC, since some drivers in a mixed setup support
  // native and others don't.
  std::vector<bool> natives;
  natives.reserve(attachments.size());
  for (const auto& a : attachments) {
    int rc = bpf_xdp_attach(a.ifindex, prog_fd,
                             XDP_FLAGS_DRV_MODE, nullptr);
    bool native = rc == 0;
    if (rc < 0) {
      rc = bpf_xdp_attach(a.ifindex, prog_fd,
                           XDP_FLAGS_SKB_MODE, nullptr);
      if (rc < 0) {
        spdlog::error(
            "wg-relay xdp: attach {} failed: {}", a.iface,
            std::strerror(-rc));
        // Roll back any NICs we already attached.
        for (const auto& done : attachments) {
          if (done.ifindex == a.ifindex) break;
          bpf_xdp_detach(done.ifindex, 0, nullptr);
        }
        bpf_object__close(obj);
        return false;
      }
    }
    natives.push_back(native);
  }

  r->xdp.bpf_obj = obj;
  r->xdp.prog_fd = prog_fd;
  r->xdp.attachments = std::move(attachments);
  r->xdp.peers_map_fd = peers_fd;
  r->xdp.macs_map_fd = macs_fd;
  r->xdp.stats_map_fd = stats_fd;
  r->xdp.port_map_fd = port_fd;
  r->xdp.peer_bytes_map_fd = peer_bytes_fd;
  r->xdp.devmap_fd = devmap_fd;
  r->xdp.nic_macs_map_fd = nic_macs_fd;
  r->xdp.nic_ips_map_fd = nic_ips_fd;
  r->xdp.attached = true;
  for (size_t i = 0; i < r->xdp.attachments.size(); ++i) {
    spdlog::info(
        "wg-relay xdp: attached on {} "
        "(ifindex={}, mode={})",
        r->xdp.attachments[i].iface,
        r->xdp.attachments[i].ifindex,
        natives[i] ? "native" : "generic");
  }
  return true;
}

void XdpDetach(WgRelay* r) {
  if (!r->xdp.attached) return;
  for (const auto& a : r->xdp.attachments) {
    if (a.ifindex > 0) {
      int rc = bpf_xdp_detach(a.ifindex, 0, nullptr);
      if (rc < 0) {
        spdlog::warn(
            "wg-relay xdp: detach {} failed: {}", a.iface,
            std::strerror(-rc));
      }
    }
  }
  if (r->xdp.bpf_obj) {
    bpf_object__close(
        static_cast<struct bpf_object*>(r->xdp.bpf_obj));
  }
  r->xdp = {};
}

// Resolve a peer's NIC name to the ifindex of an
// attachment. Empty string falls back to the first
// attached NIC, which is the iteration-1 single-NIC
// behaviour. Unknown name returns 0 (BPF treats 0 as
// "no preference, use XDP_TX").
uint32_t ResolveNicIfindex(const WgRelay* r,
                            const std::string& nic) {
  if (!r->xdp.attached || r->xdp.attachments.empty()) {
    return 0;
  }
  if (nic.empty()) {
    return static_cast<uint32_t>(
        r->xdp.attachments.front().ifindex);
  }
  for (const auto& a : r->xdp.attachments) {
    if (a.iface == nic) {
      return static_cast<uint32_t>(a.ifindex);
    }
  }
  spdlog::warn(
      "wg-relay xdp: peer NIC '{}' is not in xdp_interface "
      "list — falling back to ifindex=0", nic);
  return 0;
}

// Insert (or update) a single direction in the BPF
// peer map: src_endpoint -> partner endpoint, with the
// egress ifindex baked in so the BPF hot path can pick
// XDP_TX vs XDP_REDIRECT on a single map lookup.
void XdpInsertLinkDir(WgRelay* r, const sockaddr_storage& src,
                       socklen_t src_len,
                       const sockaddr_storage& dst,
                       socklen_t dst_len,
                       uint32_t dst_ifindex) {
  if (!r->xdp.attached) return;
  XdpEpKey key{};
  XdpEpVal val{};
  if (!EndpointToV4(src, src_len, &key.ip, &key.port)) return;
  if (!EndpointToV4(dst, dst_len, &val.ip, &val.port)) return;
  val.ifindex = dst_ifindex;
  int rc = bpf_map_update_elem(r->xdp.peers_map_fd, &key,
                                &val, BPF_ANY);
  if (rc < 0) {
    spdlog::warn("wg-relay xdp: peer insert failed: {}",
                 std::strerror(-rc));
  }
}

void XdpRemoveLinkDir(WgRelay* r, const sockaddr_storage& src,
                       socklen_t src_len) {
  if (!r->xdp.attached) return;
  XdpEpKey key{};
  if (!EndpointToV4(src, src_len, &key.ip, &key.port)) return;
  // ENOENT is fine — operator may delete a peer/link the
  // BPF map never had (e.g. v6-only endpoint).
  int rc =
      bpf_map_delete_elem(r->xdp.peers_map_fd, &key);
  if (rc < 0 && rc != -ENOENT) {
    spdlog::warn("wg-relay xdp: peer delete failed: {}",
                 std::strerror(-rc));
  }
  // Also clear any learned MAC for this endpoint so a
  // re-added peer starts from a clean cold-start cycle.
  bpf_map_delete_elem(r->xdp.macs_map_fd, &key);
  // Clear the per-peer byte counters too — operators
  // expect a removed-then-re-added peer to start at 0.
  bpf_map_delete_elem(r->xdp.peer_bytes_map_fd, &key);
}

// Sum the per-CPU XDP byte counters for a single source
// endpoint and add them to the userspace totals. Called
// from WgRelayListPeers, lock-free against the BPF hot
// path because PERCPU_HASH writes are CPU-local.
void XdpAddPeerBytes(const WgRelay* r,
                      const sockaddr_storage& src,
                      socklen_t src_len, uint64_t* rx_bytes,
                      uint64_t* fwd_bytes) {
  if (!r->xdp.attached) return;
  XdpEpKey key{};
  if (!EndpointToV4(src, src_len, &key.ip, &key.port)) return;
  int ncpus = libbpf_num_possible_cpus();
  if (ncpus < 0) return;
  struct PercpuVal {
    uint64_t rx;
    uint64_t fwd;
  };
  auto vals = std::make_unique<PercpuVal[]>(
      static_cast<size_t>(ncpus));
  if (bpf_map_lookup_elem(r->xdp.peer_bytes_map_fd, &key,
                           vals.get()) < 0) {
    // ENOENT just means no XDP traffic from this source
    // yet — leave the userspace numbers untouched.
    return;
  }
  for (int c = 0; c < ncpus; ++c) {
    *rx_bytes += vals[c].rx;
    *fwd_bytes += vals[c].fwd;
  }
}

void XdpReadStats(const WgRelay* r, WgXdpStats* out) {
  *out = {};
  if (!r->xdp.attached) return;
  int ncpus = libbpf_num_possible_cpus();
  if (ncpus < 0) return;
  auto vals = std::make_unique<uint64_t[]>(
      static_cast<size_t>(ncpus));
  uint64_t* sums[] = {&out->rx_xdp, &out->fwd_xdp,
                       &out->pass_no_peer, &out->pass_no_mac};
  for (uint32_t k = 0; k < 4; ++k) {
    if (bpf_map_lookup_elem(r->xdp.stats_map_fd, &k,
                             vals.get()) < 0) {
      continue;
    }
    for (int c = 0; c < ncpus; ++c) {
      *sums[k] += vals[static_cast<size_t>(c)];
    }
  }
}

// Convenience: insert both directions for a link by name.
// Caller holds peers_mu.
void XdpInsertLinkByNameLocked(WgRelay* r,
                                const std::string& a,
                                const std::string& b) {
  const WgRelayPeer* pa = nullptr;
  const WgRelayPeer* pb = nullptr;
  for (const auto& p : r->peers) {
    if (p.name == a) pa = &p;
    if (p.name == b) pb = &p;
  }
  if (!pa || !pb) return;
  // Direction a→b: the BPF entry keyed on a's endpoint
  // needs b's ifindex (so packets from a get redirected
  // to b's NIC). And vice versa.
  uint32_t b_if = ResolveNicIfindex(r, pb->nic);
  uint32_t a_if = ResolveNicIfindex(r, pa->nic);
  XdpInsertLinkDir(r, pa->endpoint, pa->endpoint_len,
                    pb->endpoint, pb->endpoint_len, b_if);
  XdpInsertLinkDir(r, pb->endpoint, pb->endpoint_len,
                    pa->endpoint, pa->endpoint_len, a_if);
}

void XdpRemoveLinkByNameLocked(WgRelay* r,
                                const std::string& a,
                                const std::string& b) {
  const WgRelayPeer* pa = nullptr;
  const WgRelayPeer* pb = nullptr;
  for (const auto& p : r->peers) {
    if (p.name == a) pa = &p;
    if (p.name == b) pb = &p;
  }
  if (pa) XdpRemoveLinkDir(r, pa->endpoint, pa->endpoint_len);
  if (pb) XdpRemoveLinkDir(r, pb->endpoint, pb->endpoint_len);
}

}  // namespace

WgRelay* WgRelayStart(const WgRelayConfig& cfg) {
  int sock = socket(AF_INET6, SOCK_DGRAM, 0);
  if (sock < 0) {
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
      spdlog::error("wg-relay socket: {}",
                    std::strerror(errno));
      return nullptr;
    }
  }
  int off = 0;
  setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &off,
              sizeof(off));
  int reuse = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse,
              sizeof(reuse));
  sockaddr_in6 addr{};
  addr.sin6_family = AF_INET6;
  addr.sin6_addr = in6addr_any;
  addr.sin6_port = htons(cfg.port);
  if (bind(sock, reinterpret_cast<sockaddr*>(&addr),
            sizeof(addr)) < 0) {
    spdlog::error("wg-relay bind :{}: {}", cfg.port,
                  std::strerror(errno));
    close(sock);
    return nullptr;
  }

  auto* r = new WgRelay();
  r->sock_fd = sock;
  r->port = cfg.port;
  r->roster_path = cfg.roster_path;
  if (!r->roster_path.empty()) {
    std::lock_guard lk(r->peers_mu);
    LoadRosterFile(r, r->roster_path);
  }
  // yaml-side static config wins on collision so an
  // operator can rebuild from an authoritative file.
  {
    std::lock_guard lk(r->peers_mu);
    for (const auto& p : cfg.peers) {
      // Replace if name already exists from on-disk
      // load.
      (void)PeerRemoveLocked(r, p.name);
      if (PeerAddLocked(r, p.name, p.endpoint, p.label)) {
        if (!p.pubkey_b64.empty()) {
          PeerKeyLocked(r, p.name, p.pubkey_b64);
        }
      }
    }
    for (const auto& l : cfg.links) {
      (void)LinkAddLocked(r, l.a, l.b);
    }
    if (!cfg.peers.empty() || !cfg.links.empty()) {
      PersistRosterLocked(r);
    }
  }

  r->running.store(true, std::memory_order_release);
  r->loop_thread = std::thread(RecvLoop, r);

  // Bring up the XDP fast path if the operator asked for
  // it. Failure here is non-fatal — we log and stay on
  // the userspace path; correctness is unchanged.
  if (!cfg.xdp_interface.empty()) {
    std::string obj_path = cfg.xdp_bpf_obj_path.empty()
        ? std::string("/usr/lib/hyper-derp/wg_relay.bpf.o")
        : cfg.xdp_bpf_obj_path;
    if (XdpAttach(r, cfg.xdp_interface, obj_path,
                   cfg.port)) {
      // Replay current links into the BPF map so the fast
      // path is live for already-loaded roster entries.
      std::lock_guard lk(r->peers_mu);
      for (const auto& l : r->links) {
        XdpInsertLinkByNameLocked(r, l.a, l.b);
      }
    }
  }

  size_t pc, lc;
  {
    std::lock_guard lk(r->peers_mu);
    pc = r->peers.size();
    lc = r->links.size();
  }
  spdlog::info(
      "wg-relay listening on UDP :{} ({} peers, {} links, "
      "xdp={})",
      cfg.port, pc, lc, r->xdp.attached ? "on" : "off");
  return r;
}

void WgRelayStop(WgRelay* r) {
  if (!r) return;
  r->running.store(false, std::memory_order_release);
  if (r->loop_thread.joinable()) r->loop_thread.join();
  XdpDetach(r);
  if (r->sock_fd >= 0) close(r->sock_fd);
  delete r;
}

bool WgRelayPeerAdd(WgRelay* r, const std::string& name,
                     const std::string& endpoint,
                     const std::string& label) {
  std::lock_guard lk(r->peers_mu);
  if (!PeerAddLocked(r, name, endpoint, label)) {
    return false;
  }
  PersistRosterLocked(r);
  return true;
}

bool WgRelayPeerKey(WgRelay* r, const std::string& name,
                     const std::string& pubkey_b64) {
  std::lock_guard lk(r->peers_mu);
  if (!PeerKeyLocked(r, name, pubkey_b64)) return false;
  PersistRosterLocked(r);
  return true;
}

bool WgRelayPeerNic(WgRelay* r, const std::string& name,
                     const std::string& nic) {
  std::lock_guard lk(r->peers_mu);
  // Reject names that aren't actually attached.
  if (!nic.empty() && r->xdp.attached) {
    bool found = false;
    for (const auto& a : r->xdp.attachments) {
      if (a.iface == nic) { found = true; break; }
    }
    if (!found) return false;
  }
  if (!PeerNicLocked(r, name, nic)) return false;
  // Update any existing BPF link entries that involve this
  // peer, so the new ifindex takes effect immediately.
  for (const auto& l : r->links) {
    if (l.a == name || l.b == name) {
      XdpInsertLinkByNameLocked(r, l.a, l.b);
    }
  }
  PersistRosterLocked(r);
  return true;
}

bool WgRelayPeerRemove(WgRelay* r,
                        const std::string& name) {
  std::lock_guard lk(r->peers_mu);
  // Snapshot the links the peer participates in BEFORE we
  // mutate the table — PeerRemoveLocked also tears those
  // links down and we need both sides of each one to
  // remove the BPF entries.
  std::vector<std::pair<std::string, std::string>>
      removed_links;
  for (const auto& l : r->links) {
    if (l.a == name || l.b == name) {
      removed_links.push_back({l.a, l.b});
    }
  }
  if (!PeerRemoveLocked(r, name)) return false;
  for (const auto& l : removed_links) {
    XdpRemoveLinkByNameLocked(r, l.first, l.second);
  }
  PersistRosterLocked(r);
  return true;
}

bool WgRelayLinkAdd(WgRelay* r, const std::string& a,
                     const std::string& b) {
  std::lock_guard lk(r->peers_mu);
  if (LinkAddLocked(r, a, b) != 0) return false;
  XdpInsertLinkByNameLocked(r, a, b);
  PersistRosterLocked(r);
  return true;
}

bool WgRelayLinkRemove(WgRelay* r, const std::string& a,
                        const std::string& b) {
  std::lock_guard lk(r->peers_mu);
  if (!LinkRemoveLocked(r, a, b)) return false;
  XdpRemoveLinkByNameLocked(r, a, b);
  PersistRosterLocked(r);
  return true;
}

namespace {

std::string LinkPartner(
    const std::vector<WgRelayLink>& links,
    const std::string& name) {
  for (const auto& l : links) {
    if (l.a == name) return l.b;
    if (l.b == name) return l.a;
  }
  return {};
}

}  // namespace

std::vector<WgRelayPeerInfo> WgRelayListPeers(
    const WgRelay* r) {
  std::vector<WgRelayPeerInfo> out;
  std::lock_guard lk(r->peers_mu);
  out.reserve(r->peers.size());
  for (const auto& p : r->peers) {
    WgRelayPeerInfo i;
    i.name = p.name;
    i.endpoint = p.endpoint_str;
    i.pubkey_b64 = p.pubkey_b64;
    i.label = p.label;
    i.last_seen_ns = p.last_seen_ns;
    i.rx_bytes = p.rx_bytes;
    i.fwd_bytes = p.fwd_bytes;
    // Fold in the XDP per-CPU byte counters when the fast
    // path is attached. Without this the bytes freeze at
    // whatever the cold-start packet was, since every
    // subsequent packet bypasses the userspace path that
    // bumps p.rx_bytes / p.fwd_bytes.
    XdpAddPeerBytes(r, p.endpoint, p.endpoint_len,
                     &i.rx_bytes, &i.fwd_bytes);
    i.linked_to = LinkPartner(r->links, p.name);
    out.push_back(std::move(i));
  }
  return out;
}

std::vector<WgRelayLinkInfo> WgRelayListLinks(
    const WgRelay* r) {
  std::vector<WgRelayLinkInfo> out;
  std::lock_guard lk(r->peers_mu);
  out.reserve(r->links.size());
  for (const auto& l : r->links) {
    out.push_back({l.a, l.b});
  }
  return out;
}

std::string WgRelayShowConfig(
    const WgRelay* r, const std::string& name,
    const std::string& relay_advertised_endpoint) {
  std::lock_guard lk(r->peers_mu);
  const WgRelayPeer* self = nullptr;
  for (const auto& p : r->peers) {
    if (p.name == name) {
      self = &p;
      break;
    }
  }
  if (!self) return {};
  std::string partner_name =
      LinkPartner(r->links, name);
  const WgRelayPeer* partner = nullptr;
  for (const auto& p : r->peers) {
    if (p.name == partner_name) {
      partner = &p;
      break;
    }
  }

  std::string out;
  out += "[Interface]\n";
  out += "# PrivateKey = (paste your `wg genkey` output)\n";
  out += "# Address    = (your tunnel IP, e.g. "
         "10.99.0.1/24)\n";
  out += "ListenPort   = 51820\n";
  out += "\n";
  if (partner) {
    out += "# ";
    out += partner->name;
    if (!partner->label.empty()) {
      out += " (";
      out += partner->label;
      out += ")";
    }
    out += "\n";
    out += "[Peer]\n";
    if (!partner->pubkey_b64.empty()) {
      out += "PublicKey           = ";
      out += partner->pubkey_b64;
      out += "\n";
    } else {
      out +=
          "# PublicKey           = (set with `wg peer "
          "key " +
          partner->name + " <pubkey>` on the relay)\n";
    }
    out += "AllowedIPs          = (your call)\n";
    out += "Endpoint            = ";
    out += relay_advertised_endpoint;
    out += "\n";
    out += "PersistentKeepalive = 25\n";
  } else {
    out += "# no link configured for this peer — add "
           "one with `wg link add " +
           name + " <other-name>`\n";
  }
  return out;
}

WgRelayStatsSnapshot WgRelayGetStats(const WgRelay* r) {
  WgRelayStatsSnapshot s;
  s.rx_packets =
      r->stats.rx_packets.load(std::memory_order_relaxed);
  s.fwd_packets =
      r->stats.fwd_packets.load(std::memory_order_relaxed);
  s.drop_unknown_src = r->stats.drop_unknown_src.load(
      std::memory_order_relaxed);
  s.drop_no_link = r->stats.drop_no_link.load(
      std::memory_order_relaxed);
  s.xdp_attached = r->xdp.attached;
  XdpReadStats(r, &s.xdp);
  std::lock_guard lk(r->peers_mu);
  s.peer_count = r->peers.size();
  s.link_count = r->links.size();
  return s;
}

}  // namespace hyper_derp
