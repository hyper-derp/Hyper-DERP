/// @file wg_relay.cc
/// @brief Transparent UDP relay for stock WireGuard.

#include "hyper_derp/wg_relay.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <fstream>
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
  size_t pc, lc;
  {
    std::lock_guard lk(r->peers_mu);
    pc = r->peers.size();
    lc = r->links.size();
  }
  spdlog::info(
      "wg-relay listening on UDP :{} ({} peers, {} links)",
      cfg.port, pc, lc);
  return r;
}

void WgRelayStop(WgRelay* r) {
  if (!r) return;
  r->running.store(false, std::memory_order_release);
  if (r->loop_thread.joinable()) r->loop_thread.join();
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

bool WgRelayPeerRemove(WgRelay* r,
                        const std::string& name) {
  std::lock_guard lk(r->peers_mu);
  if (!PeerRemoveLocked(r, name)) return false;
  PersistRosterLocked(r);
  return true;
}

bool WgRelayLinkAdd(WgRelay* r, const std::string& a,
                     const std::string& b) {
  std::lock_guard lk(r->peers_mu);
  if (LinkAddLocked(r, a, b) != 0) return false;
  PersistRosterLocked(r);
  return true;
}

bool WgRelayLinkRemove(WgRelay* r, const std::string& a,
                        const std::string& b) {
  std::lock_guard lk(r->peers_mu);
  if (!LinkRemoveLocked(r, a, b)) return false;
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
  std::lock_guard lk(r->peers_mu);
  s.peer_count = r->peers.size();
  s.link_count = r->links.size();
  return s;
}

}  // namespace hyper_derp
