/// @file wg_config.cc
/// @brief YAML config loader for hd-wg daemon.

#include "hd/wg/wg_config.h"

#include <cstdio>
#include <cstring>

#include <ryml.hpp>
#include <ryml_std.hpp>
#include <c4/format.hpp>

namespace hyper_derp {

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

static int ReadInt(ryml::ConstNodeRef node,
                   const char* key, int def = 0) {
  if (!node.has_child(ryml::to_csubstr(key))) return def;
  auto child = node[ryml::to_csubstr(key)];
  if (!child.has_val()) return def;
  int val = def;
  child >> val;
  return val;
}

bool WgLoadConfig(const char* path,
                  WgDaemonConfig* config) {
  FILE* f = fopen(path, "r");
  if (!f) {
    fprintf(stderr, "error: cannot open %s\n", path);
    return false;
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size <= 0 || size > 65536) {
    fclose(f);
    return false;
  }

  std::string buf(size, '\0');
  if (fread(buf.data(), 1, size, f) !=
      static_cast<size_t>(size)) {
    fclose(f);
    return false;
  }
  fclose(f);

  ryml::Tree tree = ryml::parse_in_arena(
      ryml::to_csubstr(buf));
  auto root = tree.rootref();

  if (root.has_child("relay")) {
    auto relay = root["relay"];
    config->relay_host = ReadStr(relay, "host");
    config->relay_port = static_cast<uint16_t>(
        ReadInt(relay, "port", 3341));
    config->relay_key_hex = ReadStr(relay, "key");
  }

  if (root.has_child("wireguard")) {
    auto wg = root["wireguard"];
    config->wg_private_key_hex =
        ReadStr(wg, "private_key");
    config->wg_interface =
        ReadStr(wg, "interface", "wg0");
    config->wg_listen_port = static_cast<uint16_t>(
        ReadInt(wg, "listen_port", 51820));
  }

  if (root.has_child("tunnel")) {
    auto tun = root["tunnel"];
    config->tunnel_cidr = ReadStr(tun, "cidr");
  }

  if (root.has_child("proxy")) {
    auto px = root["proxy"];
    config->proxy_port = static_cast<uint16_t>(
        ReadInt(px, "port", 51821));
  }

  if (root.has_child("stun")) {
    auto stun = root["stun"];
    config->stun_server = ReadStr(stun, "server");
    config->stun_timeout_ms =
        ReadInt(stun, "timeout_ms", 5000);
  }

  config->keepalive_secs =
      ReadInt(root, "keepalive", 25);
  config->log_level =
      ReadStr(root, "log_level", "info");
  config->force_relay =
      ReadInt(root, "force_relay", 0) != 0;

  return true;
}

}  // namespace hyper_derp
