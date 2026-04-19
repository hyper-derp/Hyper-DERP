/// @file fleet_view.h
/// @brief Fleet topology view for cross-relay addressing.
///
/// Subscribes to RouteAnnounce frames and PeerInfo events
/// to build a live view of the relay mesh. Applications can
/// query reachable relays, their peer counts, and hop
/// distances. Sending to cross-relay peers uses FleetData
/// frames transparently.

#ifndef HD_FLEET_VIEW_H_
#define HD_FLEET_VIEW_H_

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "hd/sdk/client.h"
#include "hd/sdk/error.h"

namespace hd::fleet {

/// A relay in the mesh.
struct RelayInfo {
  uint16_t id = 0;
  std::string name;
  int peer_count = 0;
  uint8_t hop_count = 0;
  bool reachable = false;
};

/// A peer on a remote relay.
struct RemotePeer {
  uint16_t peer_id = 0;
  uint16_t relay_id = 0;
  std::string name;
};

using RelayChangeCallback =
    std::function<void(const RelayInfo& relay,
                       bool added)>;

/// Fleet topology view. Attaches to a Client and tracks
/// relay announcements.
class FleetView {
 public:
  explicit FleetView(hd::sdk::Client& client);
  ~FleetView();

  FleetView(const FleetView&) = delete;
  FleetView& operator=(const FleetView&) = delete;

  /// Start tracking fleet topology.
  void Start();

  /// Stop tracking.
  void Stop();

  /// List known relays.
  std::vector<RelayInfo> ListRelays() const;

  /// Find a relay by ID.
  const RelayInfo* FindRelay(uint16_t relay_id) const;

  /// Send data to a peer on a remote relay.
  hd::sdk::Result<> SendFleetData(
      uint16_t dst_relay_id,
      uint16_t dst_peer_id,
      std::span<const uint8_t> data);

  /// Callback for relay topology changes.
  void SetRelayChangeCallback(RelayChangeCallback cb);

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace hd::fleet

#endif  // HD_FLEET_VIEW_H_
