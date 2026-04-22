/// @file config.cc
/// @brief YAML configuration file loader using rapidyaml.

#include "hyper_derp/config.h"

#include <ryml.hpp>
#include <ryml_std.hpp>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "hyper_derp/hd_peers.h"

namespace hyper_derp {

static auto ReadFile(const char* path, std::string* out)
    -> std::expected<void, Error<ConfigError>> {
  FILE* f = fopen(path, "r");
  if (!f) {
    return MakeError(ConfigError::FileNotFound,
                     std::string("cannot open ") + path +
                         ": " + strerror(errno));
  }
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (len < 0) {
    fclose(f);
    return MakeError(ConfigError::ReadFailed,
                     "ftell failed");
  }
  out->resize(static_cast<size_t>(len));
  size_t n = fread(out->data(), 1, out->size(), f);
  fclose(f);
  if (n != out->size()) {
    return MakeError(ConfigError::ReadFailed,
                     "short read");
  }
  return {};
}

/// Read an integer from a YAML node into *out.
/// Returns false and sets err on invalid values.
static bool ReadInt(ryml::ConstNodeRef node,
                    const char* name, int* out,
                    int lo, int hi,
                    Error<ConfigError>* err) {
  if (!node.has_val()) return true;
  auto val = node.val();
  char* end;
  errno = 0;
  long v = strtol(val.data(), &end, 10);
  if (end == val.data() || errno == ERANGE ||
      v < lo || v > hi) {
    *err = {ConfigError::InvalidValue,
            std::string(name) + ": expected integer [" +
                std::to_string(lo) + ".." +
                std::to_string(hi) + "]"};
    return false;
  }
  *out = static_cast<int>(v);
  return true;
}

/// Read a boolean from a YAML node.
static bool ReadBool(ryml::ConstNodeRef node,
                     const char* name, bool* out,
                     Error<ConfigError>* err) {
  if (!node.has_val()) return true;
  auto val = node.val();
  if (val == "true" || val == "yes" || val == "on" ||
      val == "1") {
    *out = true;
  } else if (val == "false" || val == "no" ||
             val == "off" || val == "0") {
    *out = false;
  } else {
    *err = {ConfigError::InvalidValue,
            std::string(name) + ": expected boolean"};
    return false;
  }
  return true;
}

/// Read a uint64_t from a YAML node.
static bool ReadUint64(ryml::ConstNodeRef node,
                       const char* name, uint64_t* out,
                       Error<ConfigError>* err) {
  if (!node.has_val()) return true;
  auto val = node.val();
  char* end;
  errno = 0;
  unsigned long long v =
      strtoull(val.data(), &end, 10);
  if (end == val.data() || errno == ERANGE) {
    *err = {ConfigError::InvalidValue,
            std::string(name) +
                ": expected non-negative integer"};
    return false;
  }
  *out = static_cast<uint64_t>(v);
  return true;
}

/// Read a string from a YAML node.
static void ReadStr(ryml::ConstNodeRef node,
                    std::string* out) {
  if (node.has_val()) {
    auto val = node.val();
    out->assign(val.data(), val.len);
  }
}

/// Parse a pin_cores list: [0, 2, 4, 6] or "0,2,4,6".
static bool ReadPinCores(ryml::ConstNodeRef node,
                         std::array<int, kMaxWorkers>* out,
                         Error<ConfigError>* err) {
  if (node.is_seq()) {
    int i = 0;
    for (auto child : node.children()) {
      if (i >= kMaxWorkers) break;
      int core = -1;
      if (!ReadInt(child, "pin_cores[]", &core,
                   0, 1023, err)) {
        return false;
      }
      (*out)[i++] = core;
    }
  } else if (node.has_val()) {
    auto val = node.val();
    std::string s(val.data(), val.len);
    const char* p = s.c_str();
    int i = 0;
    while (*p && i < kMaxWorkers) {
      char* end;
      errno = 0;
      long v = strtol(p, &end, 10);
      if (end == p || errno == ERANGE ||
          v < 0 || v > 1023) {
        *err = {ConfigError::InvalidValue,
                "pin_cores: invalid core number"};
        return false;
      }
      (*out)[i++] = static_cast<int>(v);
      if (*end == ',') {
        p = end + 1;
      } else {
        break;
      }
    }
  }
  return true;
}

auto LoadConfig(const char* path, ServerConfig* config)
    -> std::expected<void, Error<ConfigError>> {
  std::string buf;
  auto rd = ReadFile(path, &buf);
  if (!rd) return std::unexpected(rd.error());

  ryml::Tree tree;
  try {
    tree = ryml::parse_in_arena(
        ryml::to_csubstr(path),
        ryml::to_csubstr(buf));
  } catch (const std::exception& e) {
    return MakeError(ConfigError::ParseFailed,
                     std::string("YAML parse: ") +
                         e.what());
  }

  ryml::ConstNodeRef root = tree.rootref();
  if (!root.is_map()) {
    return MakeError(ConfigError::ParseFailed,
                     "root must be a mapping");
  }

  Error<ConfigError> err{};

#define TRY_INT(key, field, lo, hi)             \
  if (root.has_child(#key)) {                   \
    int v = 0;                                  \
    if (!ReadInt(root[#key], #key, &v, lo, hi,  \
                 &err))                         \
      return std::unexpected(err);              \
    config->field = v;                          \
  }

#define TRY_BOOL(key, field)                    \
  if (root.has_child(#key)) {                   \
    bool v = false;                             \
    if (!ReadBool(root[#key], #key, &v, &err))  \
      return std::unexpected(err);              \
    config->field = v;                          \
  }

#define TRY_STR(key, field)                     \
  if (root.has_child(#key))                     \
    ReadStr(root[#key], &config->field);

  TRY_INT(port, port, 1, 65535)
  TRY_INT(workers, num_workers, 0, kMaxWorkers)
  TRY_INT(sockbuf, sockbuf_size, 0, 256 * 1024 * 1024)
  TRY_INT(max_accept_rate, max_accept_per_sec,
          0, 1000000)
  TRY_BOOL(sqpoll, sqpoll)
  TRY_STR(tls_cert, tls_cert)
  TRY_STR(tls_key, tls_key)

  if (root.has_child("peer_rate_limit")) {
    if (!ReadUint64(root["peer_rate_limit"],
                    "peer_rate_limit",
                    &config->peer_rate_limit, &err))
      return std::unexpected(err);
  }

  if (root.has_child("pin_cores")) {
    if (!ReadPinCores(root["pin_cores"],
                      &config->pin_cores, &err)) {
      return std::unexpected(err);
    }
  }

  // Metrics sub-section.
  if (root.has_child("metrics")) {
    auto m = root["metrics"];
    if (m.is_map()) {
      if (m.has_child("port")) {
        int v = 0;
        if (!ReadInt(m["port"], "metrics.port", &v,
                     0, 65535, &err))
          return std::unexpected(err);
        config->metrics.port = static_cast<uint16_t>(v);
      }
      if (m.has_child("tls_cert"))
        ReadStr(m["tls_cert"], &config->metrics.tls_cert);
      if (m.has_child("tls_key"))
        ReadStr(m["tls_key"], &config->metrics.tls_key);
      if (m.has_child("debug_endpoints")) {
        bool v = false;
        if (!ReadBool(m["debug_endpoints"],
                      "metrics.debug_endpoints", &v,
                      &err))
          return std::unexpected(err);
        config->metrics.enable_debug = v;
      }
    }
  }

  // HD Protocol sub-section.
  if (root.has_child("hd")) {
    auto h = root["hd"];
    if (h.is_map()) {
      if (h.has_child("relay_key"))
        ReadStr(h["relay_key"], &config->hd_relay_key);
      if (h.has_child("relay_id")) {
        int v = 0;
        if (!ReadInt(h["relay_id"], "hd.relay_id",
                     &v, 0, 65535, &err))
          return std::unexpected(err);
        config->hd_relay_id = static_cast<uint16_t>(v);
      }
      if (h.has_child("seed_relays")) {
        auto sr = h["seed_relays"];
        if (sr.is_seq()) {
          for (auto child : sr.children()) {
            if (child.has_val()) {
              auto val = child.val();
              config->seed_relays.emplace_back(
                  val.data(), val.len);
            }
          }
        }
      }
      if (h.has_child("denylist_path")) {
        ReadStr(h["denylist_path"],
                &config->hd_denylist_path);
      }
      if (h.has_child("peer_policy_path")) {
        ReadStr(h["peer_policy_path"],
                &config->hd_peer_policy_path);
      }
      if (h.has_child("enroll_mode")) {
        auto val = h["enroll_mode"].val();
        std::string_view mode(val.data(), val.len);
        if (mode == "auto") {
          config->hd_enroll_mode =
              HdEnrollMode::kAutoApprove;
        } else if (mode == "manual") {
          config->hd_enroll_mode =
              HdEnrollMode::kManual;
        } else {
          return MakeError(
              ConfigError::InvalidValue,
              "hd.enroll_mode: expected manual|auto");
        }
      }
      // Optional auto-approve policy block:
      //   hd.enrollment:
      //     max_peers: 100
      //     allowed_keys: [ck_abc*, ...]
      //     require_ip_range: "10.0.0.0/8"
      if (h.has_child("enrollment")) {
        auto e = h["enrollment"];
        if (e.is_map()) {
          if (e.has_child("max_peers")) {
            int v = 0;
            if (!ReadInt(e["max_peers"],
                         "hd.enrollment.max_peers",
                         &v, 0, 100000, &err))
              return std::unexpected(err);
            config->hd_enroll_policy.max_peers = v;
          }
          if (e.has_child("require_ip_range")) {
            ReadStr(e["require_ip_range"],
                    &config->hd_enroll_policy
                         .require_ip_range);
          }
          if (e.has_child("allowed_keys")) {
            auto ak = e["allowed_keys"];
            if (ak.is_seq()) {
              for (auto child : ak.children()) {
                if (child.has_val()) {
                  auto v = child.val();
                  config->hd_enroll_policy.allowed_keys
                      .emplace_back(v.data(), v.len);
                }
              }
            }
          }
        }
      }
    }
  }

  // Level 2 (direct path) sub-section.
  if (root.has_child("level2")) {
    auto l2 = root["level2"];
    if (l2.is_map()) {
      if (l2.has_child("enabled")) {
        bool v = false;
        if (!ReadBool(l2["enabled"],
                      "level2.enabled", &v, &err))
          return std::unexpected(err);
        config->level2.enabled = v;
      }
      if (l2.has_child("stun_port")) {
        int v = 0;
        if (!ReadInt(l2["stun_port"],
                     "level2.stun_port", &v,
                     1, 65535, &err))
          return std::unexpected(err);
        config->level2.stun_port =
            static_cast<uint16_t>(v);
      }
      if (l2.has_child("xdp_interface"))
        ReadStr(l2["xdp_interface"],
                &config->level2.xdp_interface);
      if (l2.has_child("turn")) {
        auto t = l2["turn"];
        if (t.is_map()) {
          if (t.has_child("realm"))
            ReadStr(t["realm"],
                    &config->level2.turn_realm);
          if (t.has_child("max_allocations")) {
            int v = 0;
            if (!ReadInt(t["max_allocations"],
                         "level2.turn.max_allocations",
                         &v, 1, 1000000, &err))
              return std::unexpected(err);
            config->level2.turn_max_allocations = v;
          }
          if (t.has_child("default_lifetime")) {
            int v = 0;
            if (!ReadInt(t["default_lifetime"],
                         "level2.turn.default_lifetime",
                         &v, 30, 3600, &err))
              return std::unexpected(err);
            config->level2.turn_default_lifetime = v;
          }
        }
      }
    }
  }

  // Log level (returned in config for main.cc to apply).
  if (root.has_child("log_level")) {
    auto val = root["log_level"].val();
    std::string_view lv(val.data(), val.len);
    if (lv != "debug" && lv != "info" &&
        lv != "warn" && lv != "error") {
      return MakeError(
          ConfigError::InvalidValue,
          "log_level: expected debug|info|warn|error");
    }
  }

#undef TRY_INT
#undef TRY_BOOL
#undef TRY_STR

  return {};
}

}  // namespace hyper_derp
