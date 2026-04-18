/// @file hd_sdk.cc
/// @brief High-level HD Protocol client SDK implementation.

#include "hd_sdk.h"

#include <cstring>

#include <spdlog/spdlog.h>

namespace hyper_derp {

// -- Impl --------------------------------------------------------------------

struct HdSdk::Impl {
  HdSdkConfig config;
  HdClient client{};
  std::atomic<bool> running{false};
  std::atomic<bool> connected{false};
  std::thread recv_thread;

  // Callbacks (mutex-protected).
  std::mutex cb_mutex;
  DataCallback on_data;
  MeshDataCallback on_mesh_data;
  PeerInfoCallback on_peer_info;
  PeerGoneCallback on_peer_gone;
  ConnectionCallback on_connection;

  // Send lock (HdClient is not thread-safe).
  std::mutex send_mutex;
};

// -- Connect helper ----------------------------------------------------------

bool HdSdk::DoConnect(Impl* impl) {
  auto& cfg = impl->config;
  auto& c = impl->client;

  HdClientClose(&c);
  auto init = HdClientInit(&c);
  if (!init) {
    spdlog::error("sdk init: {}",
                  init.error().message);
    return false;
  }
  c.relay_key = cfg.relay_key;

  auto conn = HdClientConnect(
      &c, cfg.relay_host.c_str(), cfg.relay_port);
  if (!conn) {
    spdlog::error("sdk connect: {}",
                  conn.error().message);
    return false;
  }

  if (cfg.tls) {
    auto tls = HdClientTlsConnect(&c);
    if (!tls) {
      spdlog::error("sdk tls: {}",
                    tls.error().message);
      HdClientClose(&c);
      return false;
    }
  }

  auto up = HdClientUpgrade(&c);
  if (!up) {
    spdlog::error("sdk upgrade: {}",
                  up.error().message);
    HdClientClose(&c);
    return false;
  }

  auto enr = HdClientEnroll(&c);
  if (!enr) {
    spdlog::error("sdk enroll: {}",
                  enr.error().message);
    HdClientClose(&c);
    return false;
  }

  HdClientSetTimeout(&c, 100);
  impl->connected.store(true);
  return true;
}

// -- Recv thread -------------------------------------------------------------

void HdSdk::RecvLoop(Impl* impl) {
  int reconnect_delay = impl->config.reconnect_initial_delay_ms;
  uint64_t last_ping = 0;

  auto now_ms = []() -> uint64_t {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000 +
           static_cast<uint64_t>(ts.tv_nsec) / 1000000;
  };

  last_ping = now_ms();

  while (impl->running.load()) {
    // Reconnect if disconnected.
    if (!impl->connected.load()) {
      if (!impl->config.auto_reconnect) {
        impl->running.store(false);
        break;
      }

      {
        std::lock_guard lock(impl->cb_mutex);
        if (impl->on_connection)
          impl->on_connection(
              ConnectionState::kReconnecting);
      }

      // Backoff sleep.
      for (int i = 0;
           i < reconnect_delay / 100 && impl->running.load();
           i++) {
        usleep(100000);
      }
      if (!impl->running.load()) break;

      if (DoConnect(impl)) {
        spdlog::info("sdk reconnected (peer_id={})",
                     impl->client.peer_id);
        reconnect_delay =
            impl->config.reconnect_initial_delay_ms;
        last_ping = now_ms();
        {
          std::lock_guard lock(impl->cb_mutex);
          if (impl->on_connection)
            impl->on_connection(ConnectionState::kConnected);
        }
      } else {
        reconnect_delay = std::min(
            reconnect_delay * 2,
            impl->config.reconnect_max_delay_ms);
      }
      continue;
    }

    // Keepalive ping.
    if (impl->config.keepalive_ms > 0 &&
        now_ms() - last_ping >
            static_cast<uint64_t>(impl->config.keepalive_ms)) {
      std::lock_guard lock(impl->send_mutex);
      auto r = HdClientSendPing(&impl->client);
      if (!r) {
        spdlog::warn("sdk ping failed, disconnecting");
        impl->connected.store(false);
        {
          std::lock_guard cbl(impl->cb_mutex);
          if (impl->on_connection)
            impl->on_connection(
                ConnectionState::kDisconnected);
        }
        continue;
      }
      last_ping = now_ms();
    }

    // Receive frames.
    uint8_t buf[kHdMaxFramePayload];
    int buf_len;
    HdFrameType ftype;

    auto rv = HdClientRecvFrame(
        &impl->client, &ftype, buf, &buf_len, sizeof(buf));
    if (!rv) {
      if (!impl->client.connected) {
        spdlog::warn("sdk relay disconnected");
        impl->connected.store(false);
        {
          std::lock_guard lock(impl->cb_mutex);
          if (impl->on_connection)
            impl->on_connection(
                ConnectionState::kDisconnected);
        }
      }
      continue;
    }

    // Dispatch.
    std::lock_guard lock(impl->cb_mutex);

    switch (ftype) {
      case HdFrameType::kData:
        if (impl->on_data)
          impl->on_data(buf, buf_len);
        break;

      case HdFrameType::kMeshData:
        if (buf_len >= 2 && impl->on_mesh_data) {
          uint16_t src =
              static_cast<uint16_t>(buf[0]) << 8 | buf[1];
          impl->on_mesh_data(src, buf + 2, buf_len - 2);
        }
        break;

      case HdFrameType::kPeerInfo:
        if (buf_len >= 34 && impl->on_peer_info) {
          PeerEvent e{};
          memcpy(e.key.data(), buf, 32);
          e.peer_id =
              static_cast<uint16_t>(buf[32]) << 8 |
              buf[33];
          impl->on_peer_info(e);
        }
        break;

      case HdFrameType::kPeerGone:
        if (buf_len >= 32 && impl->on_peer_gone) {
          PeerEvent e{};
          memcpy(e.key.data(), buf, 32);
          impl->on_peer_gone(e);
        }
        break;

      case HdFrameType::kPong:
        // Keepalive response — handled internally.
        break;

      default:
        break;
    }
  }
}

// -- Public API --------------------------------------------------------------

HdSdk::HdSdk() : impl_(new Impl()) {}

HdSdk::~HdSdk() {
  if (impl_) {
    Stop();
    HdClientClose(&impl_->client);
    delete impl_;
  }
}

HdSdk::HdSdk(HdSdk&& other) noexcept
    : impl_(other.impl_) {
  other.impl_ = nullptr;
}

HdSdk& HdSdk::operator=(HdSdk&& other) noexcept {
  if (this != &other) {
    if (impl_) {
      Stop();
      HdClientClose(&impl_->client);
      delete impl_;
    }
    impl_ = other.impl_;
    other.impl_ = nullptr;
  }
  return *this;
}

auto HdSdk::Create(const HdSdkConfig& config)
    -> std::expected<HdSdk, Error<HdSdkError>> {
  HdSdk sdk;
  sdk.impl_->config = config;

  if (!DoConnect(sdk.impl_)) {
    return std::unexpected(MakeError(
        HdSdkError::ConnectFailed,
        "failed to connect to relay"));
  }

  spdlog::info("sdk connected (peer_id={})",
               sdk.impl_->client.peer_id);
  return sdk;
}

auto HdSdk::Start()
    -> std::expected<void, Error<HdSdkError>> {
  if (impl_->running.load()) {
    return std::unexpected(MakeError(
        HdSdkError::AlreadyRunning, "already running"));
  }

  impl_->running.store(true);
  impl_->recv_thread = std::thread(RecvLoop, impl_);
  return {};
}

void HdSdk::Stop() {
  if (!impl_) return;
  impl_->running.store(false);
  if (impl_->recv_thread.joinable()) {
    impl_->recv_thread.join();
  }
}

bool HdSdk::IsRunning() const {
  return impl_ && impl_->running.load();
}

bool HdSdk::IsConnected() const {
  return impl_ && impl_->connected.load();
}

const Key& HdSdk::PublicKey() const {
  return impl_->client.public_key;
}

uint16_t HdSdk::PeerId() const {
  return impl_->client.peer_id;
}

// -- Callback registration ---------------------------------------------------

void HdSdk::OnData(DataCallback cb) {
  std::lock_guard lock(impl_->cb_mutex);
  impl_->on_data = std::move(cb);
}

void HdSdk::OnMeshData(MeshDataCallback cb) {
  std::lock_guard lock(impl_->cb_mutex);
  impl_->on_mesh_data = std::move(cb);
}

void HdSdk::OnPeerInfo(PeerInfoCallback cb) {
  std::lock_guard lock(impl_->cb_mutex);
  impl_->on_peer_info = std::move(cb);
}

void HdSdk::OnPeerGone(PeerGoneCallback cb) {
  std::lock_guard lock(impl_->cb_mutex);
  impl_->on_peer_gone = std::move(cb);
}

void HdSdk::OnConnection(ConnectionCallback cb) {
  std::lock_guard lock(impl_->cb_mutex);
  impl_->on_connection = std::move(cb);
}

// -- Send (thread-safe via send_mutex) ---------------------------------------

auto HdSdk::SendData(const uint8_t* data, int len)
    -> std::expected<void, Error<HdSdkError>> {
  if (!impl_->connected.load()) {
    return std::unexpected(MakeError(
        HdSdkError::NotConnected, "not connected"));
  }
  std::lock_guard lock(impl_->send_mutex);
  auto r = HdClientSendData(&impl_->client, data, len);
  if (!r) {
    return std::unexpected(MakeError(
        HdSdkError::SendFailed, r.error().message));
  }
  return {};
}

auto HdSdk::SendMeshData(uint16_t dst_peer_id,
                         const uint8_t* data, int len)
    -> std::expected<void, Error<HdSdkError>> {
  if (!impl_->connected.load()) {
    return std::unexpected(MakeError(
        HdSdkError::NotConnected, "not connected"));
  }
  std::lock_guard lock(impl_->send_mutex);
  auto r = HdClientSendMeshData(
      &impl_->client, dst_peer_id, data, len);
  if (!r) {
    return std::unexpected(MakeError(
        HdSdkError::SendFailed, r.error().message));
  }
  return {};
}

auto HdSdk::SendPing()
    -> std::expected<void, Error<HdSdkError>> {
  if (!impl_->connected.load()) {
    return std::unexpected(MakeError(
        HdSdkError::NotConnected, "not connected"));
  }
  std::lock_guard lock(impl_->send_mutex);
  auto r = HdClientSendPing(&impl_->client);
  if (!r) {
    return std::unexpected(MakeError(
        HdSdkError::SendFailed, r.error().message));
  }
  return {};
}

}  // namespace hyper_derp
