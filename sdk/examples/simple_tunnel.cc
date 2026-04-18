/// @file simple_tunnel.cc
/// @brief Minimal HD SDK example: connect, discover peer,
///   open tunnel, send/receive data.

#include <hd/sdk.hpp>
#include <print>
#include <csignal>
#include <cstring>

static volatile sig_atomic_t g_stop = 0;
static void OnSignal(int) { g_stop = 1; }

int main(int argc, char** argv) {
  if (argc < 3) {
    std::println(stderr,
        "Usage: {} <relay-url> <relay-key>", argv[0]);
    return 1;
  }

  // Create client.
  auto result = hd::sdk::Client::Create({
      .relay_url = argv[1],
      .relay_key = argv[2],
      .tls = true,
  });
  if (!result) {
    std::println(stderr, "Error: {}",
                 result.error().message);
    return 1;
  }
  auto& client = *result;

  // Print peer events.
  client.SetPeerCallback(
      [](const hd::sdk::PeerInfo& peer, bool connected) {
    std::println("Peer {} (id={}) {}",
                 peer.name, peer.peer_id,
                 connected ? "connected" : "disconnected");
  });

  client.Start();
  std::println("Connected. Waiting for peers...");

  signal(SIGINT, OnSignal);
  signal(SIGTERM, OnSignal);

  while (!g_stop) {
    usleep(500000);

    // Try to open a tunnel to the first peer.
    auto peers = client.ListPeers();
    if (!peers.empty() && !g_stop) {
      auto& p = peers[0];
      auto tr = client.Open(p.name);
      if (tr) {
        auto& tunnel = *tr;
        std::println("Tunnel open to {} ({})",
                     tunnel.PeerName(), tunnel.PeerId());

        // Send a message.
        const char* msg = "hello from simple_tunnel";
        tunnel.Send(std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(msg),
            strlen(msg)));

        // Receive.
        tunnel.SetDataCallback(
            [](std::span<const uint8_t> data) {
          std::println("Received: {}",
              std::string_view(
                  reinterpret_cast<const char*>(
                      data.data()),
                  data.size()));
        });

        while (!g_stop) usleep(200000);
        tunnel.Close();
      }
    }
  }

  client.Stop();
  std::println("Done.");
  return 0;
}
