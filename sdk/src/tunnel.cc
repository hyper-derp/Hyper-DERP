/// @file tunnel.cc
/// @brief Tunnel implementation.

#include "hd/sdk/tunnel.h"
#include "internal.h"

#include <cstring>

#include "hyper_derp/hd_protocol.h"

namespace hd::sdk {

Tunnel::Tunnel() : impl_(new Impl()) {}

Tunnel::~Tunnel() {
  delete impl_;
}

Tunnel::Tunnel(Tunnel&& o) noexcept : impl_(o.impl_) {
  o.impl_ = nullptr;
}

Tunnel& Tunnel::operator=(Tunnel&& o) noexcept {
  if (this != &o) {
    delete impl_;
    impl_ = o.impl_;
    o.impl_ = nullptr;
  }
  return *this;
}

Result<> Tunnel::Send(std::span<const uint8_t> data) {
  if (!impl_ || impl_->mode.load() == Mode::Closed) {
    return std::unexpected(
        MakeError(ErrorCode::kNotConnected, "closed"));
  }
  if (!impl_->hd || !impl_->send_mutex) {
    return std::unexpected(
        MakeError(ErrorCode::kNotConnected, "no client"));
  }
  std::lock_guard lock(*impl_->send_mutex);
  auto r = hyper_derp::HdClientSendMeshData(
      impl_->hd, impl_->peer_id,
      data.data(), static_cast<int>(data.size()));
  if (!r) {
    return std::unexpected(
        MakeError(ErrorCode::kSendFailed,
                  r.error().message));
  }
  return {};
}

void Tunnel::SetDataCallback(DataCallback cb) {
  if (!impl_) return;
  std::lock_guard lock(impl_->cb_mutex);
  impl_->on_data = std::move(cb);
}

void Tunnel::SetModeChangeCallback(ModeChangeCallback cb) {
  if (!impl_) return;
  std::lock_guard lock(impl_->cb_mutex);
  impl_->on_mode = std::move(cb);
}

Mode Tunnel::CurrentMode() const {
  return impl_ ? impl_->mode.load() : Mode::Closed;
}

const std::string& Tunnel::PeerName() const {
  static const std::string empty;
  return impl_ ? impl_->peer_name : empty;
}

uint16_t Tunnel::PeerId() const {
  return impl_ ? impl_->peer_id : 0;
}

void Tunnel::Close() {
  if (!impl_) return;
  impl_->mode.store(Mode::Closed);
  {
    std::lock_guard lock(impl_->cb_mutex);
    impl_->on_data = nullptr;
    impl_->on_mode = nullptr;
  }
}

}  // namespace hd::sdk
