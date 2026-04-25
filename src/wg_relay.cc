/// @file wg_relay.cc
/// @brief Transparent WireGuard packet relay implementation.

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

#include <openssl/evp.h>
#include <spdlog/spdlog.h>

namespace hyper_derp {

namespace {

// -- WireGuard wire format -----------------------------

// Message types.
constexpr uint8_t kWgTypeInit = 1;
constexpr uint8_t kWgTypeResponse = 2;
constexpr uint8_t kWgTypeCookie = 3;
constexpr uint8_t kWgTypeData = 4;

// Fixed sizes per RFC; type 4 (data) is variable.
constexpr size_t kWgInitSize = 148;
constexpr size_t kWgResponseSize = 92;
constexpr size_t kWgCookieSize = 64;
constexpr size_t kWgDataMin = 32;
constexpr size_t kWgMacBytes = 16;

// MAC1 lives at the same offset relative to packet start
// for types 1 and 2: end of message minus mac2 (16) minus
// mac1 (16). Compute by subtracting from the fixed sizes
// when we know the type.
constexpr size_t kInitMac1Off = kWgInitSize - 32;       // 116
constexpr size_t kRespMac1Off = kWgResponseSize - 32;   // 60

// LABEL_MAC1 is the WG-specific label fed into the MAC1
// key derivation. 8 bytes per the spec.
constexpr uint8_t kLabelMac1[8] = {
    'm', 'a', 'c', '1', '-', '-', '-', '-'};

// -- BLAKE2s helpers via OpenSSL EVP --------------------

// Unkeyed BLAKE2s-256. `out` must be 32 bytes.
void Blake2s256(const uint8_t* data, size_t len,
                uint8_t out[32]) {
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  unsigned int n = 32;
  EVP_DigestInit_ex(ctx, EVP_blake2s256(), nullptr);
  EVP_DigestUpdate(ctx, data, len);
  EVP_DigestFinal_ex(ctx, out, &n);
  EVP_MD_CTX_free(ctx);
}

// Keyed BLAKE2s with 16-byte output (BLAKE2SMAC, used for
// WireGuard MAC1 / MAC2). Returns true on success.
bool Blake2sMac16(const uint8_t* key, size_t key_len,
                  const uint8_t* data, size_t data_len,
                  uint8_t out[16]) {
  EVP_MAC* mac = EVP_MAC_fetch(nullptr, "BLAKE2SMAC",
                                nullptr);
  if (!mac) return false;
  EVP_MAC_CTX* ctx = EVP_MAC_CTX_new(mac);
  if (!ctx) {
    EVP_MAC_free(mac);
    return false;
  }
  size_t out_size = 16;
  OSSL_PARAM params[] = {
      OSSL_PARAM_construct_size_t("size", &out_size),
      OSSL_PARAM_construct_end()};
  bool ok =
      EVP_MAC_init(ctx, key, key_len, params) == 1 &&
      EVP_MAC_update(ctx, data, data_len) == 1;
  size_t got = 0;
  if (ok) {
    ok = EVP_MAC_final(ctx, out, &got, 16) == 1 &&
         got == 16;
  }
  EVP_MAC_CTX_free(ctx);
  EVP_MAC_free(mac);
  return ok;
}

// Compute the MAC1 key for a given responder pubkey:
//   MAC1_KEY = BLAKE2s(LABEL_MAC1 || responder_pubkey)
void Mac1KeyForPubkey(const uint8_t pubkey[32],
                      uint8_t key_out[32]) {
  uint8_t buf[8 + 32];
  std::memcpy(buf, kLabelMac1, 8);
  std::memcpy(buf + 8, pubkey, 32);
  Blake2s256(buf, sizeof(buf), key_out);
}

// -- endpoint helpers ----------------------------------

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
                    const sockaddr_storage& b, socklen_t lb) {
  if (la != lb) return false;
  return std::memcmp(&a, &b, la) == 0;
}

std::string SockaddrToString(const sockaddr_storage& sa) {
  char host[NI_MAXHOST];
  char serv[NI_MAXSERV];
  socklen_t len = (sa.ss_family == AF_INET6)
                      ? sizeof(sockaddr_in6)
                      : sizeof(sockaddr_in);
  if (getnameinfo(reinterpret_cast<const sockaddr*>(&sa),
                   len, host, sizeof(host), serv,
                   sizeof(serv),
                   NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
    return "?";
  }
  return std::string(host) + ":" + serv;
}

uint64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now()
                 .time_since_epoch())
      .count();
}

bool HexToBytes(const std::string& hex, uint8_t* out,
                 size_t want) {
  if (hex.size() != want * 2) return false;
  for (size_t i = 0; i < want; ++i) {
    auto hex_nibble = [](char c, int* v) {
      if (c >= '0' && c <= '9') {
        *v = c - '0';
        return true;
      }
      if (c >= 'a' && c <= 'f') {
        *v = c - 'a' + 10;
        return true;
      }
      if (c >= 'A' && c <= 'F') {
        *v = c - 'A' + 10;
        return true;
      }
      return false;
    };
    int hi, lo;
    if (!hex_nibble(hex[i * 2], &hi) ||
        !hex_nibble(hex[i * 2 + 1], &lo)) {
      return false;
    }
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

// -- Roster persistence --------------------------------

// Each line: `<pubkey-hex> <endpoint> [label]`. Lines
// beginning with `#` are comments. The format is
// deliberately tab/space-tolerant so an operator can
// hand-edit it during recovery without breaking the
// loader.
std::string SerializeRoster(
    const std::vector<WgRelayPeer>& peers) {
  std::string out;
  out += "# wg-relay roster — managed by `wg peer add` / "
         "`wg peer remove`\n";
  out += "# format: <pubkey-hex> <endpoint> [label]\n";
  for (const auto& p : peers) {
    static const char* h = "0123456789abcdef";
    std::string hex(64, '0');
    for (size_t i = 0; i < 32; ++i) {
      hex[i * 2] = h[(p.pubkey[i] >> 4) & 0xF];
      hex[i * 2 + 1] = h[p.pubkey[i] & 0xF];
    }
    out += hex;
    out += ' ';
    out += p.endpoint_str;
    if (!p.label.empty()) {
      out += ' ';
      out += p.label;
    }
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

// Add or replace a peer assuming the caller already
// holds peers_mu. Used by both the roster loader and
// the public WgRelayAddPeer.
bool WgRelayAddPeerLocked(WgRelay* r,
                           const uint8_t pubkey[32],
                           const std::string& endpoint,
                           const std::string& label) {
  sockaddr_storage ep{};
  socklen_t ep_len = 0;
  if (!ParseHostPort(endpoint, &ep, &ep_len)) {
    return false;
  }
  for (auto& p : r->peers) {
    if (std::memcmp(p.pubkey, pubkey, 32) == 0) {
      p.endpoint = ep;
      p.endpoint_len = ep_len;
      p.endpoint_str = endpoint;
      p.label = label;
      return true;
    }
  }
  WgRelayPeer p;
  std::memcpy(p.pubkey, pubkey, 32);
  p.endpoint = ep;
  p.endpoint_len = ep_len;
  p.endpoint_str = endpoint;
  p.label = label;
  r->peers.push_back(std::move(p));
  return true;
}

void LoadRosterFile(WgRelay* r, const std::string& path) {
  std::ifstream f(path);
  if (!f.is_open()) return;
  int loaded = 0;
  int bad = 0;
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream iss(line);
    std::string hex, endpoint, label;
    iss >> hex >> endpoint;
    if (iss) std::getline(iss, label);
    while (!label.empty() && label.front() == ' ') {
      label.erase(label.begin());
    }
    uint8_t pk[32];
    if (!HexToBytes(hex, pk, 32) || endpoint.empty()) {
      ++bad;
      continue;
    }
    if (WgRelayAddPeerLocked(r, pk, endpoint, label)) {
      ++loaded;
    } else {
      ++bad;
    }
  }
  spdlog::info(
      "wg-relay roster loaded from {}: {} peers, {} bad",
      path, loaded, bad);
}

// -- Forward path --------------------------------------

// Look up which roster entry is the destination of a
// type-1 or type-2 handshake packet by recomputing MAC1
// against each peer's pubkey.
const WgRelayPeer* IdentifyHandshakeDst(
    const std::vector<WgRelayPeer>& peers,
    const uint8_t* pkt, size_t mac1_off) {
  uint8_t key[32];
  uint8_t mac[16];
  for (const auto& p : peers) {
    Mac1KeyForPubkey(p.pubkey, key);
    if (!Blake2sMac16(key, 32, pkt, mac1_off, mac)) {
      continue;
    }
    if (std::memcmp(mac, pkt + mac1_off, 16) == 0) {
      return &p;
    }
  }
  return nullptr;
}

void HandlePacket(WgRelay* r, const uint8_t* pkt,
                   size_t len,
                   const sockaddr_storage& src,
                   socklen_t src_len) {
  r->stats.rx_packets.fetch_add(1, std::memory_order_relaxed);

  if (len < 4) {
    r->stats.drop_bad_form.fetch_add(
        1, std::memory_order_relaxed);
    return;
  }
  uint8_t type = pkt[0];

  // The destination 4-tuple this packet should be
  // forwarded to. Set by the per-type branches below.
  sockaddr_storage dst{};
  socklen_t dst_len = 0;
  bool dst_known = false;

  switch (type) {
    case kWgTypeInit: {
      if (len != kWgInitSize) {
        r->stats.drop_bad_form.fetch_add(
            1, std::memory_order_relaxed);
        return;
      }
      const WgRelayPeer* peer = nullptr;
      {
        std::lock_guard lk(r->peers_mu);
        peer = IdentifyHandshakeDst(r->peers, pkt,
                                     kInitMac1Off);
        if (peer) {
          dst = peer->endpoint;
          dst_len = peer->endpoint_len;
          dst_known = true;
        }
      }
      if (!dst_known) break;

      // Record the initiator's session id → src endpoint
      // so the eventual response and any data packets that
      // address this index get routed back to the right
      // place.
      uint32_t sender_index = 0;
      std::memcpy(&sender_index, pkt + 4, 4);
      {
        std::lock_guard lk(r->sessions_mu);
        WgRelaySession s;
        s.session_id = sender_index;
        s.endpoint = src;
        s.endpoint_len = src_len;
        s.last_seen_ns = NowNs();
        r->sessions[sender_index] = s;
      }
      break;
    }
    case kWgTypeResponse: {
      if (len != kWgResponseSize) {
        r->stats.drop_bad_form.fetch_add(
            1, std::memory_order_relaxed);
        return;
      }
      // Response carries `receiver_index` at bytes 8..11.
      // That's the index allocated by the original
      // initiator; look it up to find where to send.
      uint32_t receiver_index = 0;
      std::memcpy(&receiver_index, pkt + 8, 4);
      {
        std::lock_guard lk(r->sessions_mu);
        auto it = r->sessions.find(receiver_index);
        if (it != r->sessions.end()) {
          dst = it->second.endpoint;
          dst_len = it->second.endpoint_len;
          dst_known = true;
          it->second.last_seen_ns = NowNs();
        }
      }
      // Also record the responder's own session id so
      // future data packets addressed to it find a
      // route.
      uint32_t sender_index = 0;
      std::memcpy(&sender_index, pkt + 4, 4);
      {
        std::lock_guard lk(r->sessions_mu);
        WgRelaySession s;
        s.session_id = sender_index;
        s.endpoint = src;
        s.endpoint_len = src_len;
        s.last_seen_ns = NowNs();
        r->sessions[sender_index] = s;
      }
      break;
    }
    case kWgTypeCookie:
    case kWgTypeData: {
      size_t need = (type == kWgTypeCookie)
                        ? kWgCookieSize
                        : kWgDataMin;
      if (len < need) {
        r->stats.drop_bad_form.fetch_add(
            1, std::memory_order_relaxed);
        return;
      }
      // receiver_index for cookie + data is at bytes 4..7.
      uint32_t receiver_index = 0;
      std::memcpy(&receiver_index, pkt + 4, 4);
      {
        std::lock_guard lk(r->sessions_mu);
        auto it = r->sessions.find(receiver_index);
        if (it != r->sessions.end()) {
          dst = it->second.endpoint;
          dst_len = it->second.endpoint_len;
          dst_known = true;
          it->second.last_seen_ns = NowNs();
        }
      }
      if (!dst_known) {
        r->stats.drop_no_session.fetch_add(
            1, std::memory_order_relaxed);
        return;
      }
      break;
    }
    default:
      r->stats.drop_bad_form.fetch_add(
          1, std::memory_order_relaxed);
      return;
  }

  if (!dst_known) {
    r->stats.drop_no_dst.fetch_add(
        1, std::memory_order_relaxed);
    return;
  }

  ssize_t sent = sendto(
      r->sock_fd, pkt, len, 0,
      reinterpret_cast<const sockaddr*>(&dst), dst_len);
  if (sent < 0) {
    spdlog::warn("wg-relay sendto: {}",
                 std::strerror(errno));
    return;
  }
  r->stats.fwd_packets.fetch_add(
      1, std::memory_order_relaxed);
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
  // Replay persisted roster before applying any yaml-side
  // peers; yaml entries take precedence on duplicate keys.
  if (!r->roster_path.empty()) {
    std::lock_guard lk(r->peers_mu);
    LoadRosterFile(r, r->roster_path);
  }
  for (const auto& p : cfg.peers) {
    uint8_t pk[32];
    if (!HexToBytes(p.pubkey_hex, pk, 32)) {
      spdlog::warn("wg-relay: bad pubkey hex, skipping");
      continue;
    }
    if (p.endpoint.empty()) {
      spdlog::warn(
          "wg-relay: peer {} has no endpoint, skipping",
          p.label);
      continue;
    }
    (void)WgRelayAddPeer(r, pk, p.endpoint, p.label);
  }
  r->running.store(true, std::memory_order_release);
  r->loop_thread = std::thread(RecvLoop, r);
  spdlog::info("wg-relay listening on UDP :{} ({} peers)",
                cfg.port, r->peers.size());
  return r;
}

void WgRelayStop(WgRelay* r) {
  if (!r) return;
  r->running.store(false, std::memory_order_release);
  if (r->loop_thread.joinable()) r->loop_thread.join();
  if (r->sock_fd >= 0) close(r->sock_fd);
  delete r;
}

bool WgRelayAddPeer(WgRelay* r, const uint8_t pubkey[32],
                     const std::string& endpoint,
                     const std::string& label) {
  bool ok;
  std::string body;
  {
    std::lock_guard lk(r->peers_mu);
    ok = WgRelayAddPeerLocked(r, pubkey, endpoint, label);
    if (ok) body = SerializeRoster(r->peers);
  }
  if (ok && !r->roster_path.empty()) {
    if (!WriteRosterAtomic(r->roster_path, body)) {
      spdlog::warn(
          "wg-relay roster write {} failed",
          r->roster_path);
    }
  }
  return ok;
}

bool WgRelayRemovePeer(WgRelay* r,
                        const uint8_t pubkey[32]) {
  bool removed = false;
  std::string body;
  {
    std::lock_guard lk(r->peers_mu);
    for (auto it = r->peers.begin();
         it != r->peers.end(); ++it) {
      if (std::memcmp(it->pubkey, pubkey, 32) == 0) {
        r->peers.erase(it);
        removed = true;
        break;
      }
    }
    if (removed) body = SerializeRoster(r->peers);
  }
  if (removed && !r->roster_path.empty()) {
    if (!WriteRosterAtomic(r->roster_path, body)) {
      spdlog::warn(
          "wg-relay roster write {} failed",
          r->roster_path);
    }
  }
  return removed;
}

std::vector<WgRelayPeerInfo> WgRelayListPeers(
    const WgRelay* r) {
  std::vector<WgRelayPeerInfo> out;
  std::lock_guard lk(r->peers_mu);
  out.reserve(r->peers.size());
  for (const auto& p : r->peers) {
    WgRelayPeerInfo i;
    std::memcpy(i.pubkey, p.pubkey, 32);
    i.label = p.label;
    i.endpoint = p.endpoint_str;
    i.last_seen_ns = p.last_seen_ns;
    out.push_back(std::move(i));
  }
  return out;
}

WgRelayStatsSnapshot WgRelayGetStats(const WgRelay* r) {
  WgRelayStatsSnapshot s;
  s.rx_packets =
      r->stats.rx_packets.load(std::memory_order_relaxed);
  s.fwd_packets =
      r->stats.fwd_packets.load(std::memory_order_relaxed);
  s.drop_no_dst =
      r->stats.drop_no_dst.load(std::memory_order_relaxed);
  s.drop_bad_form = r->stats.drop_bad_form.load(
      std::memory_order_relaxed);
  s.drop_no_session = r->stats.drop_no_session.load(
      std::memory_order_relaxed);
  {
    std::lock_guard lk(r->peers_mu);
    s.peer_count = r->peers.size();
  }
  {
    std::lock_guard lk(r->sessions_mu);
    s.session_count = r->sessions.size();
  }
  return s;
}

}  // namespace hyper_derp
