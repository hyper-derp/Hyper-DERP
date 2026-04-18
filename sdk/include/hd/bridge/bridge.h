/// @file bridge.h
/// @brief Bridge a tunnel to a local TCP or unix socket.
///
/// Usage:
///   hd::bridge::Bridge bridge(client);
///   bridge.Listen("tcp://127.0.0.1:8080", "camera-01");
///   bridge.Listen("unix:///var/run/hd.sock", "peer-name");
///   // Each accepted connection runs a bidirectional pump
///   // between the local socket and the HD tunnel.
///   bridge.Stop();

#ifndef HD_BRIDGE_H_
#define HD_BRIDGE_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "hd/sdk/client.h"
#include "hd/sdk/error.h"

namespace hd::bridge {

/// A single bridge listener.
struct Listener {
  std::string address;  // "tcp://host:port" or "unix://path"
  std::string peer_name;
  int fd = -1;
  std::atomic<bool> running{false};
  std::thread accept_thread;
  std::vector<std::thread> conn_threads;
};

/// Bridge manager. Owns listeners and connection threads.
class Bridge {
 public:
  explicit Bridge(hd::sdk::Client& client);
  ~Bridge();

  Bridge(const Bridge&) = delete;
  Bridge& operator=(const Bridge&) = delete;

  /// Start listening. Spawns accept thread.
  hd::sdk::Result<> Listen(const std::string& address,
                           const std::string& peer_name);

  /// Stop all listeners and drain connections.
  void Stop();

 private:
  hd::sdk::Client& client_;
  std::vector<std::unique_ptr<Listener>> listeners_;
};

}  // namespace hd::bridge

#endif  // HD_BRIDGE_H_
