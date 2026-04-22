/// @file client.cc
/// @brief Client implementation.

#include "hd/sdk/client.h"
#include "internal.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <sodium.h>
#include <spdlog/spdlog.h>

#include "hyper_derp/hd_client.h"
#include "hyper_derp/hd_protocol.h"
#include "hyper_derp/protocol.h"

namespace hd::sdk {

// -- URL parsing -------------------------------------------------------------

static bool ParseRelayUrl(const std::string& url,
                          std::string* host,
                          uint16_t* port) {
  std::string s = url;
  // Strip hd:// prefix.
  if (s.starts_with("hd://")) s = s.substr(5);
  auto colon = s.rfind(':');
  if (colon != std::string::npos) {
    *host = s.substr(0, colon);
    *port = static_cast<uint16_t>(
        std::stoi(s.substr(colon + 1)));
  } else {
    *host = s;
    *port = 443;
  }
  return !host->empty();
}

// -- Key persistence ---------------------------------------------------------

static bool SaveKey(const std::string& path,
                    const uint8_t* pub, const uint8_t* priv) {
  if (path.empty()) return true;
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return false;
  fwrite(pub, 1, 32, f);
  fwrite(priv, 1, 32, f);
  fclose(f);
  return true;
}

static bool LoadKey(const std::string& path,
                    uint8_t* pub, uint8_t* priv) {
  if (path.empty()) return false;
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  bool ok = fread(pub, 1, 32, f) == 32 &&
            fread(priv, 1, 32, f) == 32;
  fclose(f);
  return ok;
}

// -- Impl --------------------------------------------------------------------

struct Client::Impl {
  ClientConfig config;
  std::string host;
  uint16_t port = 0;
  hyper_derp::Key relay_key{};

  hyper_derp::HdClient hd{};
  std::atomic<Status> status{Status::Disconnected};
  std::atomic<bool> running{false};
  std::thread io_thread;

  // Peers: peer_id → PeerInfo.
  std::mutex peers_mutex;
  std::unordered_map<uint16_t, PeerInfo> peers;

  // Tunnels: peer_id → Tunnel::Impl*.
  std::mutex tunnels_mutex;
  std::unordered_map<uint16_t, Tunnel::Impl*> tunnels;

  // Callbacks.
  std::mutex cb_mutex;
  PeerCallback on_peer;
  ErrorCallback on_error;
  RedirectCallback on_redirect;

  // Send serialization.
  std::mutex send_mutex;

  // Frame pool.
  std::unique_ptr<FramePool> pool;

  // OpenConnection waiters: correlation_id → slot.
  struct OpenWait {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    hyper_derp::HdOpenConnectionResult result{};
  };
  std::mutex open_waits_mutex;
  std::unordered_map<uint64_t, OpenWait*> open_waits;
  std::atomic<uint64_t> next_corr_id{1};
};

// -- Connect helper ----------------------------------------------------------

bool Client::DoConnect(Impl* impl) {
  hyper_derp::HdClientClose(&impl->hd);

  // Try to load persisted keys.
  uint8_t pub[32], priv[32];
  if (LoadKey(impl->config.key_path, pub, priv)) {
    hyper_derp::HdClientInitWithKeys(
        &impl->hd, pub, priv, impl->relay_key);
  } else {
    auto init = hyper_derp::HdClientInit(&impl->hd);
    if (!init) {
      spdlog::error("sdk init: {}",
                    init.error().message);
      return false;
    }
    // Persist new keys.
    SaveKey(impl->config.key_path,
            impl->hd.public_key.data(),
            impl->hd.private_key.data());
  }
  impl->hd.relay_key = impl->relay_key;

  auto conn = hyper_derp::HdClientConnect(
      &impl->hd, impl->host.c_str(), impl->port);
  if (!conn) {
    spdlog::error("sdk connect: {}",
                  conn.error().message);
    return false;
  }

  if (impl->config.tls) {
    auto tls = hyper_derp::HdClientTlsConnect(&impl->hd);
    if (!tls) {
      spdlog::error("sdk tls: {}", tls.error().message);
      hyper_derp::HdClientClose(&impl->hd);
      return false;
    }
  }

  auto up = hyper_derp::HdClientUpgrade(&impl->hd);
  if (!up) {
    spdlog::error("sdk upgrade: {}", up.error().message);
    hyper_derp::HdClientClose(&impl->hd);
    return false;
  }

  auto enr = hyper_derp::HdClientEnroll(&impl->hd);
  if (!enr) {
    spdlog::error("sdk enroll: {}", enr.error().message);
    hyper_derp::HdClientClose(&impl->hd);
    return false;
  }

  hyper_derp::HdClientSetTimeout(&impl->hd, 100);
  impl->status.store(Status::Connected);
  return true;
}

// -- Recv loop ---------------------------------------------------------------

void Client::RecvLoop(Impl* impl) {
  int backoff = impl->config.reconnect_initial_ms;

  auto now_ms = []() -> uint64_t {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000 +
           static_cast<uint64_t>(ts.tv_nsec) / 1000000;
  };

  uint64_t last_ping = now_ms();

  while (impl->running.load()) {
    // Reconnect.
    if (impl->status.load() != Status::Connected) {
      if (!impl->config.auto_reconnect) {
        impl->running.store(false);
        break;
      }
      impl->status.store(Status::Connecting);
      for (int i = 0;
           i < backoff / 100 && impl->running.load();
           i++) {
        usleep(100000);
      }
      if (!impl->running.load()) break;
      if (DoConnect(impl)) {
        spdlog::info("sdk reconnected");
        backoff = impl->config.reconnect_initial_ms;
        last_ping = now_ms();
        // Re-announce to existing tunnels: reset mode to
        // Pending so data resumes on reconnect.
        {
          std::lock_guard lock(impl->tunnels_mutex);
          for (auto& [id, t] : impl->tunnels) {
            t->mode.store(Mode::Relayed);
          }
        }
      } else {
        backoff = std::min(backoff * 2,
                           impl->config.reconnect_max_ms);
      }
      continue;
    }

    // Keepalive.
    auto ka_ms = impl->config.keepalive.count() * 1000;
    if (ka_ms > 0 && now_ms() - last_ping >
        static_cast<uint64_t>(ka_ms)) {
      std::lock_guard lock(impl->send_mutex);
      if (!hyper_derp::HdClientSendPing(&impl->hd)) {
        spdlog::warn("sdk ping failed");
        impl->status.store(Status::Disconnected);
        continue;
      }
      last_ping = now_ms();
    }

    // Receive.
    uint8_t buf[hyper_derp::kHdMaxFramePayload];
    int buf_len;
    hyper_derp::HdFrameType ftype;

    auto rv = hyper_derp::HdClientRecvFrame(
        &impl->hd, &ftype, buf, &buf_len, sizeof(buf));
    if (!rv) {
      if (!impl->hd.connected) {
        spdlog::warn("sdk disconnected");
        impl->status.store(Status::Disconnected);
      }
      continue;
    }

    DispatchFrame(impl, ftype, buf, buf_len);
  }
}

// -- Public API --------------------------------------------------------------

Client::Client() : impl_(new Impl()) {}

Client::~Client() {
  if (impl_) {
    Stop();
    hyper_derp::HdClientClose(&impl_->hd);
    delete impl_;
  }
}

Client::Client(Client&& o) noexcept : impl_(o.impl_) {
  o.impl_ = nullptr;
}

Client& Client::operator=(Client&& o) noexcept {
  if (this != &o) {
    if (impl_) {
      Stop();
      hyper_derp::HdClientClose(&impl_->hd);
      delete impl_;
    }
    impl_ = o.impl_;
    o.impl_ = nullptr;
  }
  return *this;
}

Result<Client> Client::Create(const ClientConfig& config) {
  if (sodium_init() < 0 && sodium_init() < 0) {
    return std::unexpected(
        MakeError(ErrorCode::kInitFailed, "sodium_init"));
  }

  Client c;
  c.impl_->config = config;

  // Parse URL.
  if (!ParseRelayUrl(config.relay_url,
                     &c.impl_->host, &c.impl_->port)) {
    return std::unexpected(
        MakeError(ErrorCode::kConnectFailed,
                  "invalid relay_url"));
  }

  // Parse relay key.
  if (config.relay_key.size() == 64) {
    sodium_hex2bin(c.impl_->relay_key.data(), 32,
                   config.relay_key.c_str(), 64,
                   nullptr, nullptr, nullptr);
  }

  if (!DoConnect(c.impl_)) {
    return std::unexpected(
        MakeError(ErrorCode::kConnectFailed,
                  "failed to connect to relay"));
  }

  // Create frame pool.
  c.impl_->pool = std::make_unique<FramePool>(
      config.frame_pool_size);

  spdlog::info("sdk connected to {}:{}", c.impl_->host,
               c.impl_->port);
  return c;
}

Result<> Client::Start() {
  if (impl_->running.load()) {
    return std::unexpected(
        MakeError(ErrorCode::kAlreadyRunning,
                  "already running"));
  }
  impl_->running.store(true);
  if (impl_->config.event_thread == EventThread::Own) {
    impl_->io_thread = std::thread(RecvLoop, impl_);
  }
  return {};
}

void Client::Stop() {
  if (!impl_) return;
  impl_->running.store(false);
  if (impl_->io_thread.joinable()) {
    impl_->io_thread.join();
  }
  // Close all tunnels.
  {
    std::lock_guard lock(impl_->tunnels_mutex);
    for (auto& [id, t] : impl_->tunnels) {
      t->mode.store(Mode::Closed);
    }
    impl_->tunnels.clear();
  }
}

void Client::Run() {
  if (!impl_->running.load()) return;
  RecvLoop(impl_);
}

void Client::DispatchFrame(Impl* impl,
                           hyper_derp::HdFrameType ftype,
                           const uint8_t* buf,
                           int buf_len) {
  using hyper_derp::HdFrameType;

  if (ftype == HdFrameType::kPeerInfo && buf_len >= 34) {
    PeerInfo pi{};
    std::memcpy(pi.public_key.data(), buf, 32);
    pi.peer_id = static_cast<uint16_t>(buf[32]) << 8 |
                 buf[33];
    // Extended format: [2B peer_id][2B relay_id]
    if (buf_len >= 36) {
      pi.relay_id = static_cast<uint16_t>(buf[34]) << 8 |
                    buf[35];
    }
    pi.connected = true;
    char hex[17];
    sodium_bin2hex(hex, sizeof(hex), buf, 8);
    pi.name = hex;
    {
      std::lock_guard lock(impl->peers_mutex);
      impl->peers[pi.peer_id] = pi;
    }
    {
      std::lock_guard lock(impl->cb_mutex);
      if (impl->on_peer) impl->on_peer(pi, true);
    }
  } else if (ftype == HdFrameType::kPeerGone &&
             buf_len >= 32) {
    PeerInfo pi{};
    std::memcpy(pi.public_key.data(), buf, 32);
    pi.connected = false;
    uint16_t gone_id = 0;
    {
      std::lock_guard lock(impl->peers_mutex);
      for (auto& [id, p] : impl->peers) {
        if (p.public_key == pi.public_key) {
          gone_id = id;
          pi = p;
          pi.connected = false;
          break;
        }
      }
      if (gone_id > 0) impl->peers.erase(gone_id);
    }
    {
      std::lock_guard lock(impl->cb_mutex);
      if (impl->on_peer) impl->on_peer(pi, false);
    }
    {
      std::lock_guard lock(impl->tunnels_mutex);
      auto it = impl->tunnels.find(gone_id);
      if (it != impl->tunnels.end()) {
        it->second->mode.store(Mode::Closed);
      }
    }
  } else if (ftype == HdFrameType::kOpenConnectionResult) {
    hyper_derp::HdOpenConnectionResult r;
    if (!hyper_derp::HdParseOpenConnectionResult(
            buf, buf_len, &r)) {
      return;
    }
    Impl::OpenWait* w = nullptr;
    {
      std::lock_guard lock(impl->open_waits_mutex);
      auto it = impl->open_waits.find(r.correlation_id);
      if (it != impl->open_waits.end()) w = it->second;
    }
    if (w) {
      std::lock_guard wlk(w->mu);
      w->result = r;
      w->done = true;
      w->cv.notify_all();
    }
  } else if (ftype == HdFrameType::kIncomingConnection) {
    hyper_derp::HdIncomingConnection inc;
    if (!hyper_derp::HdParseIncomingConnection(
            buf, buf_len, &inc)) {
      return;
    }
    // Phase 2 auto-consent: accept with our defaults.
    hyper_derp::HdIntent b_intent =
        impl->config.default_routing ==
                Intent::PreferDirect
            ? hyper_derp::HdIntent::kPreferDirect
        : impl->config.default_routing ==
                Intent::RequireDirect
            ? hyper_derp::HdIntent::kRequireDirect
        : impl->config.default_routing ==
                Intent::PreferRelay
            ? hyper_derp::HdIntent::kPreferRelay
            : hyper_derp::HdIntent::kRequireRelay;
    uint8_t b_flags = 0;
    if (impl->config.allow_upgrade) {
      b_flags |= hyper_derp::kHdFlagAllowUpgrade;
    }
    if (impl->config.allow_downgrade) {
      b_flags |= hyper_derp::kHdFlagAllowDowngrade;
    }
    uint8_t rbuf[hyper_derp::kHdFrameHeaderSize +
                 hyper_derp::kHdIncomingRespSize];
    int rn = hyper_derp::HdBuildIncomingConnResponse(
        rbuf, inc.correlation_id, b_intent, b_flags,
        hyper_derp::kHdFlagAccept);
    std::lock_guard lock(impl->send_mutex);
    // Raw send on the HD client: we bypass the helpers
    // since no HdClientSendIncomingConnResponse exists.
    int total = 0;
    while (total < rn) {
      int w = 0;
      if (impl->hd.ssl) {
        w = SSL_write(impl->hd.ssl, rbuf + total,
                      rn - total);
      } else {
        w = ::write(impl->hd.fd, rbuf + total,
                    rn - total);
      }
      if (w <= 0) break;
      total += w;
    }
  } else if (ftype == HdFrameType::kIncomingConnResult) {
    // Target-side result; we don't track targets in the
    // SDK Phase 2. Logged at debug for observability.
    hyper_derp::HdIncomingConnResult r;
    if (hyper_derp::HdParseIncomingConnResult(
            buf, buf_len, &r)) {
      spdlog::debug("sdk: IncomingConnResult mode={} "
                    "corr={:x}",
                    static_cast<int>(r.mode),
                    r.correlation_id);
    }
  } else if (ftype == HdFrameType::kRedirect &&
             buf_len >= 1) {
    hyper_derp::HdRedirectReason reason;
    char url[hyper_derp::kHdRedirectMaxUrl + 1];
    int url_len = hyper_derp::HdParseRedirect(
        buf, buf_len, &reason, url, sizeof(url));
    if (url_len < 0) return;
    std::string target(url, url_len);
    spdlog::info("sdk redirect: reason={} target={}",
                 static_cast<int>(reason), target);
    bool follow = true;
    {
      std::lock_guard lock(impl->cb_mutex);
      if (impl->on_redirect) {
        follow = impl->on_redirect(reason, target);
      }
    }
    if (!follow || target.empty()) return;
    std::string new_host;
    uint16_t new_port = 0;
    if (!ParseRelayUrl(target, &new_host, &new_port)) {
      spdlog::warn("sdk redirect: bad target url");
      return;
    }
    impl->host = std::move(new_host);
    impl->port = new_port;
    impl->config.relay_url = target;
    // Trigger the RecvLoop's reconnect path with the
    // updated host/port.
    hyper_derp::HdClientClose(&impl->hd);
    impl->status.store(Status::Disconnected);
  } else if (ftype == HdFrameType::kMeshData &&
             buf_len >= 2) {
    uint16_t src =
        static_cast<uint16_t>(buf[0]) << 8 | buf[1];
    const uint8_t* payload = buf + 2;
    int payload_len = buf_len - 2;
    std::lock_guard lock(impl->tunnels_mutex);
    auto it = impl->tunnels.find(src);
    if (it != impl->tunnels.end()) {
      auto* t = it->second;
      std::lock_guard tl(t->cb_mutex);
      if (t->on_data && t->mode.load() != Mode::Closed) {
        t->on_data(std::span<const uint8_t>(
            payload, payload_len));
      }
    }
  }
  // kPong handled silently.
}

void Client::Poll() {
  if (!impl_ || impl_->status.load() != Status::Connected)
    return;

  uint8_t buf[hyper_derp::kHdMaxFramePayload];
  int buf_len;
  hyper_derp::HdFrameType ftype;
  for (int i = 0; i < 64; i++) {
    auto rv = hyper_derp::HdClientRecvFrame(
        &impl_->hd, &ftype, buf, &buf_len, sizeof(buf));
    if (!rv) break;
    DispatchFrame(impl_, ftype, buf, buf_len);
  }
}

int Client::EventFd() const {
  return impl_ ? impl_->hd.fd : -1;
}

Status Client::GetStatus() const {
  return impl_ ? impl_->status.load()
               : Status::Disconnected;
}

// -- Tunnels -----------------------------------------------------------------

Result<Tunnel> Client::Open(const std::string& peer_name) {
  return Open(peer_name, {});
}

Result<Tunnel> Client::Open(const std::string& peer_name,
                            const TunnelOptions& opts) {
  if (impl_->status.load() != Status::Connected) {
    return std::unexpected(
        MakeError(ErrorCode::kNotConnected,
                  "not connected"));
  }

  // Find peer by name.
  uint16_t peer_id = 0;
  {
    std::lock_guard lock(impl_->peers_mutex);
    for (auto& [id, p] : impl_->peers) {
      if (p.name == peer_name) {
        peer_id = id;
        break;
      }
    }
  }

  if (peer_id == 0) {
    // Also try matching by peer_id if name is numeric.
    try {
      uint16_t num = static_cast<uint16_t>(
          std::stoi(peer_name));
      std::lock_guard lock(impl_->peers_mutex);
      if (impl_->peers.count(num)) peer_id = num;
    } catch (...) {}
  }

  if (peer_id == 0) {
    return std::unexpected(
        MakeError(ErrorCode::kPeerNotFound,
                  "peer not found: " + peer_name));
  }

  // Look up relay_id for cross-relay routing.
  uint16_t relay_id = 0;
  {
    std::lock_guard lock(impl_->peers_mutex);
    auto it = impl_->peers.find(peer_id);
    if (it != impl_->peers.end()) {
      relay_id = it->second.relay_id;
    }
  }

  Tunnel t;
  t.impl_->peer_name = peer_name;
  t.impl_->peer_id = peer_id;
  t.impl_->relay_id = relay_id;
  t.impl_->hd = &impl_->hd;
  t.impl_->send_mutex = &impl_->send_mutex;
  t.impl_->mode.store(Mode::Pending);

  // Register in tunnel map so data gets dispatched.
  {
    std::lock_guard lock(impl_->tunnels_mutex);
    impl_->tunnels[peer_id] = t.impl_;
  }

  // Send OpenConnection and wait for the result.
  Intent intent = opts.routing.value_or(
      impl_->config.default_routing);
  bool allow_upgrade = opts.allow_upgrade.value_or(
      impl_->config.allow_upgrade);
  bool allow_downgrade = opts.allow_downgrade.value_or(
      impl_->config.allow_downgrade);
  hyper_derp::HdIntent wire_intent =
      intent == Intent::PreferDirect
          ? hyper_derp::HdIntent::kPreferDirect
      : intent == Intent::RequireDirect
          ? hyper_derp::HdIntent::kRequireDirect
      : intent == Intent::PreferRelay
          ? hyper_derp::HdIntent::kPreferRelay
          : hyper_derp::HdIntent::kRequireRelay;
  uint8_t flags = 0;
  if (allow_upgrade) {
    flags |= hyper_derp::kHdFlagAllowUpgrade;
  }
  if (allow_downgrade) {
    flags |= hyper_derp::kHdFlagAllowDowngrade;
  }

  uint64_t corr = impl_->next_corr_id.fetch_add(1);
  Impl::OpenWait wait_slot;
  {
    std::lock_guard lock(impl_->open_waits_mutex);
    impl_->open_waits[corr] = &wait_slot;
  }

  uint8_t frame[hyper_derp::kHdFrameHeaderSize +
                hyper_derp::kHdOpenConnSize];
  hyper_derp::HdBuildOpenConnection(
      frame, peer_id, relay_id, wire_intent, flags, corr);
  {
    std::lock_guard lock(impl_->send_mutex);
    int total = 0;
    int rn = sizeof(frame);
    while (total < rn) {
      int w = 0;
      if (impl_->hd.ssl) {
        w = SSL_write(impl_->hd.ssl, frame + total,
                      rn - total);
      } else {
        w = ::write(impl_->hd.fd, frame + total,
                    rn - total);
      }
      if (w <= 0) break;
      total += w;
    }
  }

  // Wait up to 8s for the result.
  {
    std::unique_lock lk(wait_slot.mu);
    wait_slot.cv.wait_for(lk, std::chrono::seconds(8),
                          [&] { return wait_slot.done; });
  }
  {
    std::lock_guard lock(impl_->open_waits_mutex);
    impl_->open_waits.erase(corr);
  }
  if (!wait_slot.done) {
    // Timed out locally.
    t.impl_->mode.store(Mode::Closed);
    t.impl_->deny_reason.store(static_cast<uint16_t>(
        hyper_derp::HdDenyReason::kTargetUnresponsive));
    return std::unexpected(
        MakeError(ErrorCode::kOpenFailed,
                  "OpenConnection timed out"));
  }
  const auto& r = wait_slot.result;
  if (r.mode == hyper_derp::HdConnMode::kDenied) {
    t.impl_->mode.store(Mode::Closed);
    t.impl_->deny_reason.store(
        static_cast<uint16_t>(r.deny_reason));
    return std::unexpected(
        MakeError(ErrorCode::kOpenFailed,
                  "OpenConnection denied"));
  }
  t.impl_->mode.store(
      r.mode == hyper_derp::HdConnMode::kDirect
          ? Mode::Direct
          : Mode::Relayed);
  return t;
}

// -- Peers -------------------------------------------------------------------

std::vector<PeerInfo> Client::ListPeers() const {
  std::vector<PeerInfo> result;
  if (!impl_) return result;
  std::lock_guard lock(impl_->peers_mutex);
  result.reserve(impl_->peers.size());
  for (auto& [id, p] : impl_->peers) {
    result.push_back(p);
  }
  return result;
}

// -- Callbacks ---------------------------------------------------------------

void Client::SetPeerCallback(PeerCallback cb) {
  std::lock_guard lock(impl_->cb_mutex);
  impl_->on_peer = std::move(cb);
}

void Client::SetErrorCallback(ErrorCallback cb) {
  std::lock_guard lock(impl_->cb_mutex);
  impl_->on_error = std::move(cb);
}

void Client::SetRedirectCallback(RedirectCallback cb) {
  std::lock_guard lock(impl_->cb_mutex);
  impl_->on_redirect = std::move(cb);
}

// -- Raw send ----------------------------------------------------------------

Result<> Client::SendMeshData(
    uint16_t dst_peer_id,
    std::span<const uint8_t> data) {
  if (impl_->status.load() != Status::Connected) {
    return std::unexpected(
        MakeError(ErrorCode::kNotConnected,
                  "not connected"));
  }
  std::lock_guard lock(impl_->send_mutex);
  auto r = hyper_derp::HdClientSendMeshData(
      &impl_->hd, dst_peer_id,
      data.data(), static_cast<int>(data.size()));
  if (!r) {
    return std::unexpected(
        MakeError(ErrorCode::kSendFailed,
                  r.error().message));
  }
  return {};
}

// -- Frame pool --------------------------------------------------------------

std::unique_ptr<FrameBuffer> Client::AllocFrame() {
  if (!impl_ || !impl_->pool) return nullptr;
  return impl_->pool->Alloc();
}

}  // namespace hd::sdk
