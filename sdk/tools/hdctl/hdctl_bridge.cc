/// @file hdctl_bridge.cc
/// @brief hdctl bridge mode: YAML-driven tunnel bridging.
///
/// Usage:
///   hdctl bridge --config tunnel.yaml
///
/// Config:
///   relay:
///     url: "hd://relay:443"
///     key: "rk_..."
///   bridges:
///     - name: "camera"
///       listen: "tcp://127.0.0.1:8080"
///       peer: "camera-01"
///     - name: "sensors"
///       listen: "unix:///var/run/hd.sock"
///       peer: "sensor-gw"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>

#include <ryml.hpp>
#include <ryml_std.hpp>

#include <spdlog/spdlog.h>

#include <hd/sdk.hpp>
#include <hd/bridge/bridge.h>

using namespace hd::sdk;

static volatile sig_atomic_t g_stop = 0;
static void SigHandler(int) { g_stop = 1; }

struct BridgeEntry {
  std::string name;
  std::string listen;
  std::string peer;
};

static std::string ReadStr(ryml::ConstNodeRef node,
                           const char* key,
                           const char* def = "") {
  if (!node.has_child(ryml::to_csubstr(key))) return def;
  auto child = node[ryml::to_csubstr(key)];
  if (!child.has_val()) return def;
  std::string val;
  child >> val;
  return val;
}

int hdctl_bridge_main(const char* config_path) {
  // Read config file.
  FILE* f = fopen(config_path, "r");
  if (!f) {
    fprintf(stderr, "error: cannot open %s\n",
            config_path);
    return 1;
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::string buf(size, '\0');
  if (fread(buf.data(), 1, size, f) !=
      static_cast<size_t>(size)) {
    fclose(f);
    return 1;
  }
  fclose(f);

  ryml::Tree tree = ryml::parse_in_arena(
      ryml::to_csubstr(buf));
  auto root = tree.rootref();

  // Parse relay config.
  ClientConfig cfg;
  if (root.has_child("relay")) {
    auto relay = root["relay"];
    cfg.relay_url = ReadStr(relay, "url");
    cfg.relay_key = ReadStr(relay, "key");
    cfg.key_path = ReadStr(relay, "key_path");
  }

  // Parse bridges.
  std::vector<BridgeEntry> entries;
  if (root.has_child("bridges") &&
      root["bridges"].is_seq()) {
    for (auto node : root["bridges"]) {
      BridgeEntry e;
      e.name = ReadStr(node, "name");
      e.listen = ReadStr(node, "listen");
      e.peer = ReadStr(node, "peer");
      if (!e.listen.empty() && !e.peer.empty()) {
        entries.push_back(std::move(e));
      }
    }
  }

  if (entries.empty()) {
    fprintf(stderr, "error: no bridges configured\n");
    return 1;
  }

  // Connect to relay.
  spdlog::info("connecting to {}", cfg.relay_url);
  auto result = Client::Create(cfg);
  if (!result) {
    spdlog::error("connect failed: {}",
                  result.error().message);
    return 1;
  }
  auto& client = *result;

  client.SetPeerCallback(
      [](const PeerInfo& p, bool conn) {
    spdlog::info("peer {} (id={}) {}",
                 p.name, p.peer_id,
                 conn ? "connected" : "disconnected");
  });

  auto sr = client.Start();
  if (!sr) {
    spdlog::error("start failed: {}",
                  sr.error().message);
    return 1;
  }

  // Wait for peers to appear before opening bridges.
  spdlog::info("waiting for peers...");
  usleep(2000000);

  // Start bridges.
  hd::bridge::Bridge bridge(client);
  for (const auto& e : entries) {
    auto r = bridge.Listen(e.listen, e.peer);
    if (!r) {
      spdlog::error("bridge {}: {}", e.name,
                    r.error().message);
    } else {
      spdlog::info("bridge {} -> {} on {}",
                   e.name, e.peer, e.listen);
    }
  }

  // Run until signal.
  signal(SIGINT, SigHandler);
  signal(SIGTERM, SigHandler);
  signal(SIGPIPE, SIG_IGN);

  spdlog::info("hdctl bridge running ({} bridges)",
               entries.size());
  while (!g_stop) {
    usleep(500000);
  }

  spdlog::info("shutting down");
  bridge.Stop();
  client.Stop();
  return 0;
}
