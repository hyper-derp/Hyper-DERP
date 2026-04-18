/// @file tunnel.h
/// @brief Tunnel: a connection to a specific peer.

#ifndef HD_SDK_TUNNEL_H_
#define HD_SDK_TUNNEL_H_

#include <cstdint>
#include <functional>
#include <span>
#include <string>

#include "hd/sdk/config.h"
#include "hd/sdk/error.h"

namespace hd::sdk {

using DataCallback =
    std::function<void(std::span<const uint8_t> data)>;
using ModeChangeCallback =
    std::function<void(Mode mode)>;

class Client;

/// A tunnel to a specific peer.
/// Created via Client::Open(). Not copyable, movable.
class Tunnel {
 public:
  ~Tunnel();
  Tunnel(Tunnel&& other) noexcept;
  Tunnel& operator=(Tunnel&& other) noexcept;
  Tunnel(const Tunnel&) = delete;
  Tunnel& operator=(const Tunnel&) = delete;

  /// Send data to the peer (copy-and-send).
  Result<> Send(std::span<const uint8_t> data);

  /// Register callback for incoming data.
  void SetDataCallback(DataCallback cb);

  /// Register callback for mode transitions.
  void SetModeChangeCallback(ModeChangeCallback cb);

  /// Current connection mode.
  Mode CurrentMode() const;

  /// Peer name this tunnel connects to.
  const std::string& PeerName() const;

  /// Peer ID.
  uint16_t PeerId() const;

  /// Close the tunnel.
  void Close();

 private:
  friend class Client;
  Tunnel();
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace hd::sdk

#endif  // HD_SDK_TUNNEL_H_
