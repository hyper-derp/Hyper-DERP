/// @file client.cc
/// @brief Client implementation.

#include "hd/sdk/client.h"
#include "internal.h"

#include <atomic>
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

  // Send serialization.
  std::mutex send_mutex;
};

// -- Connect helper ----------------------------------------------------------

bool Client::DoConnect(Impl* impl) {
  hyper_derp::HdClientClose(&impl->hd);
  auto init = hyper_derp::HdClientInit(&impl->hd);
  if (!init) {
    spdlog::error("sdk init: {}", init.error().message);
    return false;
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

    // Dispatch.
    using hyper_derp::HdFrameType;

    if (ftype == HdFrameType::kPeerInfo && buf_len >= 34) {
      PeerInfo pi{};
      std::memcpy(pi.public_key.data(), buf, 32);
      pi.peer_id = static_cast<uint16_t>(buf[32]) << 8 |
                   buf[33];
      pi.connected = true;
      // Name = hex of first 8 bytes of key.
      char hex[17];
      sodium_bin2hex(hex, sizeof(hex), buf, 8);
      pi.name = hex;

      {
        std::lock_guard lock(impl->peers_mutex);
        impl->peers[pi.peer_id] = pi;
      }
      {
        std::lock_guard lock(impl->cb_mutex);
        if (impl->on_peer)
          impl->on_peer(pi, true);
      }

    } else if (ftype == HdFrameType::kPeerGone &&
               buf_len >= 32) {
      hyper_derp::Key key;
      std::memcpy(key.data(), buf, 32);
      PeerInfo pi{};
      std::memcpy(pi.public_key.data(), buf, 32);
      pi.connected = false;

      // Find peer_id by key.
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
        if (impl->on_peer)
          impl->on_peer(pi, false);
      }
      // Close tunnel if open.
      {
        std::lock_guard lock(impl->tunnels_mutex);
        auto it = impl->tunnels.find(gone_id);
        if (it != impl->tunnels.end()) {
          it->second->mode.store(Mode::Closed);
        }
      }

    } else if (ftype == HdFrameType::kMeshData &&
               buf_len >= 2) {
      uint16_t src =
          static_cast<uint16_t>(buf[0]) << 8 | buf[1];
      const uint8_t* payload = buf + 2;
      int payload_len = buf_len - 2;

      // Dispatch to tunnel.
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

    } else if (ftype == HdFrameType::kPong) {
      // Keepalive handled.
    }
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

void Client::Poll() {
  // Single iteration of recv — process available frames.
  if (!impl_ || impl_->status.load() != Status::Connected)
    return;

  uint8_t buf[hyper_derp::kHdMaxFramePayload];
  int buf_len;
  hyper_derp::HdFrameType ftype;
  // Process up to 64 frames per Poll.
  for (int i = 0; i < 64; i++) {
    auto rv = hyper_derp::HdClientRecvFrame(
        &impl_->hd, &ftype, buf, &buf_len, sizeof(buf));
    if (!rv) break;
    // TODO: dispatch like RecvLoop.
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

  Tunnel t;
  t.impl_->peer_name = peer_name;
  t.impl_->peer_id = peer_id;
  t.impl_->hd = &impl_->hd;
  t.impl_->send_mutex = &impl_->send_mutex;
  t.impl_->mode.store(Mode::Relayed);

  // Register in tunnel map so data gets dispatched.
  {
    std::lock_guard lock(impl_->tunnels_mutex);
    impl_->tunnels[peer_id] = t.impl_;
  }

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

}  // namespace hd::sdk
