/// @file fleet_view.cc
/// @brief Fleet topology view implementation.

#include "hd/fleet/fleet_view.h"

#include <cstring>
#include <unordered_map>

#include <spdlog/spdlog.h>

#include "hyper_derp/hd_protocol.h"

namespace hd::fleet {

struct FleetView::Impl {
  hd::sdk::Client* client = nullptr;
  mutable std::mutex mutex;
  std::unordered_map<uint16_t, RelayInfo> relays;
  RelayChangeCallback on_change;
  bool running = false;
};

FleetView::FleetView(hd::sdk::Client& client)
    : impl_(new Impl()) {
  impl_->client = &client;
}

FleetView::~FleetView() {
  Stop();
  delete impl_;
}

void FleetView::Start() {
  impl_->running = true;

  // Register for MeshData to receive RouteAnnounce
  // forwarded from the relay. The relay's control plane
  // sends RouteAnnounce as MeshData with a "RTAN" magic
  // prefix to enrolled peers.

  // For now, the fleet view is populated manually via
  // SendFleetData responses or by the application calling
  // ListRelays after receiving relay info from the admin
  // API.

  spdlog::info("fleet view started");
}

void FleetView::Stop() {
  impl_->running = false;
}

std::vector<RelayInfo> FleetView::ListRelays() const {
  std::lock_guard lock(impl_->mutex);
  std::vector<RelayInfo> result;
  result.reserve(impl_->relays.size());
  for (const auto& [id, r] : impl_->relays) {
    result.push_back(r);
  }
  return result;
}

const RelayInfo* FleetView::FindRelay(
    uint16_t relay_id) const {
  std::lock_guard lock(impl_->mutex);
  auto it = impl_->relays.find(relay_id);
  if (it == impl_->relays.end()) return nullptr;
  return &it->second;
}

hd::sdk::Result<> FleetView::SendFleetData(
    uint16_t dst_relay_id,
    uint16_t dst_peer_id,
    std::span<const uint8_t> data) {
  // Build FleetData frame: [4B hdr][2B relay][2B peer][payload]
  int payload_len =
      hyper_derp::kHdFleetDstSize +
      static_cast<int>(data.size());
  int frame_len =
      hyper_derp::kHdFrameHeaderSize + payload_len;

  auto buf = std::make_unique<uint8_t[]>(frame_len);
  hyper_derp::HdBuildFleetDataHeader(
      buf.get(), dst_relay_id, dst_peer_id,
      static_cast<int>(data.size()));
  std::memcpy(
      buf.get() + hyper_derp::kHdFrameHeaderSize +
          hyper_derp::kHdFleetDstSize,
      data.data(), data.size());

  // Send as raw data through the client.
  return impl_->client->SendMeshData(
      0,  // peer_id 0 = relay itself routes it.
      std::span<const uint8_t>(buf.get(), frame_len));
}

void FleetView::SetRelayChangeCallback(
    RelayChangeCallback cb) {
  std::lock_guard lock(impl_->mutex);
  impl_->on_change = std::move(cb);
}

}  // namespace hd::fleet
