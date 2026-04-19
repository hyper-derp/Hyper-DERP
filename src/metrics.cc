/// @file metrics.cc
/// @brief HTTP metrics server using Crow with optional TLS.
///
/// Exposes Prometheus text format at /metrics, health check
/// at /health, and debug introspection at /debug/workers and
/// /debug/peers.

#include <crow.h>

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include <sodium.h>

#include "hyper_derp/data_plane.h"
#include "hyper_derp/handshake.h"
#include "hyper_derp/hd_handshake.h"
#include "hyper_derp/hd_peers.h"
#include "hyper_derp/metrics.h"
#include "hyper_derp/protocol.h"

namespace hyper_derp {

struct MetricsServer {
  crow::SimpleApp app;
  std::thread thread;
  Ctx* ctx;
  HdPeerRegistry* hd_peers;
  HdServerCounters hd_counters;
  std::chrono::steady_clock::time_point start_time;
};

// -- Prometheus text format helpers ------------------------------------------

static void WriteCounter(std::ostringstream& out,
                         const char* name,
                         const char* help,
                         uint64_t value) {
  out << "# HELP " << name << ' ' << help << '\n'
      << "# TYPE " << name << " counter\n"
      << name << ' ' << value << '\n';
}

static void WriteCounterLabeled(
    std::ostringstream& out,
    const char* name,
    const char* help,
    const char* label_name,
    std::initializer_list<
        std::pair<const char*, uint64_t>> entries) {
  out << "# HELP " << name << ' ' << help << '\n'
      << "# TYPE " << name << " counter\n";
  for (auto& [label_val, value] : entries) {
    out << name << '{' << label_name << "=\""
        << label_val << "\"} " << value << '\n';
  }
}

static void WriteGauge(std::ostringstream& out,
                       const char* name,
                       const char* help,
                       int64_t value) {
  out << "# HELP " << name << ' ' << help << '\n'
      << "# TYPE " << name << " gauge\n"
      << name << ' ' << value << '\n';
}

// -- Stats collection --------------------------------------------------------

struct AggregatedStats {
  uint64_t recv_bytes = 0;
  uint64_t send_bytes = 0;
  uint64_t send_drops = 0;
  uint64_t xfer_drops = 0;
  uint64_t slab_exhausts = 0;
  uint64_t send_epipe = 0;
  uint64_t send_econnreset = 0;
  uint64_t send_eagain = 0;
  uint64_t send_other_err = 0;
  uint64_t recv_enobufs = 0;
  uint64_t frame_pool_hits = 0;
  uint64_t frame_pool_misses = 0;
  uint64_t hd_mesh_forwards = 0;
  uint64_t hd_fleet_forwards = 0;
  uint64_t rate_limit_drops = 0;
  uint64_t hd_fwd_no_rule = 0;
  uint64_t hd_fwd_no_route = 0;
  uint64_t hd_fwd_same_worker = 0;
  int64_t peers_active = 0;
  int64_t hd_peers_active = 0;
  int num_workers = 0;
};

static AggregatedStats CollectStats(Ctx* ctx) {
  AggregatedStats s;
  s.num_workers = ctx->num_workers;
  for (int i = 0; i < ctx->num_workers; i++) {
    Worker* w = ctx->workers[i];
    if (!w) continue;
    s.recv_bytes += __atomic_load_n(
        &w->stats.recv_bytes, __ATOMIC_RELAXED);
    s.send_bytes += __atomic_load_n(
        &w->stats.send_bytes, __ATOMIC_RELAXED);
    s.send_drops += __atomic_load_n(
        &w->stats.send_drops, __ATOMIC_RELAXED);
    s.xfer_drops += __atomic_load_n(
        &w->stats.xfer_drops, __ATOMIC_RELAXED);
    s.slab_exhausts += __atomic_load_n(
        &w->stats.slab_exhausts, __ATOMIC_RELAXED);
    s.send_epipe += __atomic_load_n(
        &w->stats.send_epipe, __ATOMIC_RELAXED);
    s.send_econnreset += __atomic_load_n(
        &w->stats.send_econnreset, __ATOMIC_RELAXED);
    s.send_eagain += __atomic_load_n(
        &w->stats.send_eagain, __ATOMIC_RELAXED);
    s.send_other_err += __atomic_load_n(
        &w->stats.send_other_err, __ATOMIC_RELAXED);
    s.recv_enobufs += __atomic_load_n(
        &w->stats.recv_enobufs, __ATOMIC_RELAXED);
    s.frame_pool_hits += __atomic_load_n(
        &w->stats.frame_pool_hits, __ATOMIC_RELAXED);
    s.frame_pool_misses += __atomic_load_n(
        &w->stats.frame_pool_misses, __ATOMIC_RELAXED);
    s.hd_mesh_forwards += __atomic_load_n(
        &w->stats.hd_mesh_forwards, __ATOMIC_RELAXED);
    s.hd_fleet_forwards += __atomic_load_n(
        &w->stats.hd_fleet_forwards, __ATOMIC_RELAXED);
    s.rate_limit_drops += __atomic_load_n(
        &w->stats.rate_limit_drops, __ATOMIC_RELAXED);
    s.hd_fwd_no_rule += __atomic_load_n(
        &w->stats.hd_fwd_no_rule, __ATOMIC_RELAXED);
    s.hd_fwd_no_route += __atomic_load_n(
        &w->stats.hd_fwd_no_route, __ATOMIC_RELAXED);
    s.hd_fwd_same_worker += __atomic_load_n(
        &w->stats.hd_fwd_same_worker, __ATOMIC_RELAXED);
    // Count active peers from hash table.
    for (int j = 0; j < kHtCapacity; j++) {
      if (w->ht[j].occupied == 1) {
        s.peers_active++;
        if (w->ht[j].protocol == PeerProtocol::kHd) {
          s.hd_peers_active++;
        }
      }
    }
  }
  return s;
}

// -- Route handlers ----------------------------------------------------------

static void RegisterRoutes(MetricsServer* ms,
                           bool enable_debug) {
  // Prometheus scrape endpoint.
  CROW_ROUTE(ms->app, "/metrics")
  ([ms]() {
    auto s = CollectStats(ms->ctx);
    std::ostringstream out;

    WriteCounter(out, "hyper_derp_recv_bytes_total",
                 "Total bytes received", s.recv_bytes);
    WriteCounter(out, "hyper_derp_send_bytes_total",
                 "Total bytes sent", s.send_bytes);
    WriteCounter(out, "hyper_derp_xfer_drops_total",
                 "Cross-shard transfer ring drops",
                 s.xfer_drops);
    WriteCounter(out, "hyper_derp_slab_exhausts_total",
                 "SendItem slab exhaustions",
                 s.slab_exhausts);
    WriteCounterLabeled(
        out, "hyper_derp_send_errors_total",
        "Send errors by type", "reason",
        {{"epipe", s.send_epipe},
         {"econnreset", s.send_econnreset},
         {"eagain", s.send_eagain},
         {"other", s.send_other_err}});
    WriteCounter(out, "hyper_derp_send_drops_total",
                 "Total send drops", s.send_drops);
    WriteCounter(out, "hyper_derp_recv_enobufs_total",
                 "Provided buffer pool exhaustions",
                 s.recv_enobufs);
    WriteGauge(out, "hyper_derp_peers_active",
               "Currently connected peers",
               s.peers_active);
    WriteGauge(out, "hyper_derp_workers",
               "Number of data plane workers",
               s.num_workers);
    WriteCounter(out, "hyper_derp_frame_pool_hits_total",
                 "Frame pool allocations from pool",
                 s.frame_pool_hits);
    WriteCounter(out, "hyper_derp_frame_pool_misses_total",
                 "Frame pool allocations via malloc",
                 s.frame_pool_misses);

    // HD Protocol metrics.
    WriteGauge(out, "hyper_derp_hd_peers_active",
               "HD Protocol peers",
               s.hd_peers_active);
    WriteCounter(out,
                 "hyper_derp_hd_mesh_forwards_total",
                 "MeshData frames forwarded",
                 s.hd_mesh_forwards);
    WriteCounter(out,
                 "hyper_derp_hd_fleet_forwards_total",
                 "FleetData frames forwarded",
                 s.hd_fleet_forwards);
    WriteCounter(out,
                 "hyper_derp_rate_limit_drops_total",
                 "Recv batches dropped by rate limiter",
                 s.rate_limit_drops);

    // Server-level HD counters.
    if (ms->hd_counters.enrollments) {
      WriteCounter(out,
                   "hyper_derp_hd_enrollments_total",
                   "HD enrollment attempts",
                   ms->hd_counters.enrollments->load(
                       std::memory_order_relaxed));
    }
    if (ms->hd_counters.auth_failures) {
      WriteCounter(out,
                   "hyper_derp_hd_auth_failures_total",
                   "HD HMAC auth failures",
                   ms->hd_counters.auth_failures->load(
                       std::memory_order_relaxed));
    }

    auto resp = crow::response(200, out.str());
    resp.set_header("Content-Type",
                    "text/plain; version=0.0.4; "
                    "charset=utf-8");
    return resp;
  });

  // Health check.
  CROW_ROUTE(ms->app, "/health")
  ([ms]() {
    auto now = std::chrono::steady_clock::now();
    auto uptime = std::chrono::duration_cast<
        std::chrono::seconds>(now - ms->start_time);
    auto s = CollectStats(ms->ctx);

    crow::json::wvalue j;
    j["status"] = "ok";
    j["uptime_seconds"] = uptime.count();
    j["workers"] = s.num_workers;
    j["peers_active"] = s.peers_active;
    j["recv_bytes"] = s.recv_bytes;
    j["send_bytes"] = s.send_bytes;
    return crow::response(200, j);
  });

  if (!enable_debug) return;

  // Per-worker stats breakdown.
  CROW_ROUTE(ms->app, "/debug/workers")
  ([ms]() {
    crow::json::wvalue::list workers;
    for (int i = 0; i < ms->ctx->num_workers; i++) {
      Worker* w = ms->ctx->workers[i];
      if (!w) continue;
      crow::json::wvalue wj;
      wj["id"] = w->id;
      wj["recv_bytes"] = __atomic_load_n(
          &w->stats.recv_bytes, __ATOMIC_RELAXED);
      wj["send_bytes"] = __atomic_load_n(
          &w->stats.send_bytes, __ATOMIC_RELAXED);
      wj["send_drops"] = __atomic_load_n(
          &w->stats.send_drops, __ATOMIC_RELAXED);
      wj["xfer_drops"] = __atomic_load_n(
          &w->stats.xfer_drops, __ATOMIC_RELAXED);
      wj["slab_exhausts"] = __atomic_load_n(
          &w->stats.slab_exhausts, __ATOMIC_RELAXED);
      wj["send_epipe"] = __atomic_load_n(
          &w->stats.send_epipe, __ATOMIC_RELAXED);
      wj["send_econnreset"] = __atomic_load_n(
          &w->stats.send_econnreset, __ATOMIC_RELAXED);
      wj["send_eagain"] = __atomic_load_n(
          &w->stats.send_eagain, __ATOMIC_RELAXED);
      wj["hd_fwd_no_route"] = __atomic_load_n(
          &w->stats.hd_fwd_no_route, __ATOMIC_RELAXED);
      wj["hd_fwd_same_worker"] = __atomic_load_n(
          &w->stats.hd_fwd_same_worker, __ATOMIC_RELAXED);
      wj["send_queue_drops"] = __atomic_load_n(
          &w->stats.send_queue_drops, __ATOMIC_RELAXED);
      // Count peers.
      int peers = 0;
      for (int j = 0; j < kHtCapacity; j++) {
        if (w->ht[j].occupied == 1) peers++;
      }
      wj["peers"] = peers;
      workers.push_back(std::move(wj));
    }
    crow::json::wvalue resp;
    resp["workers"] = std::move(workers);
    return crow::response(200, resp);
  });

  // Active peer list.
  CROW_ROUTE(ms->app, "/debug/peers")
  ([ms]() {
    crow::json::wvalue::list peers;
    for (int i = 0; i < ms->ctx->num_workers; i++) {
      Worker* w = ms->ctx->workers[i];
      if (!w) continue;
      for (int j = 0; j < kHtCapacity; j++) {
        if (w->ht[j].occupied != 1) continue;
        Peer* p = &w->ht[j];
        crow::json::wvalue pj;
        // Hex-encode the key.
        char hex[kKeySize * 2 + 1];
        KeyToHex(p->key, hex);
        pj["key"] = std::string(hex);
        pj["fd"] = p->fd;
        pj["worker"] = w->id;
        pj["send_inflight"] = p->send_inflight;
        pj["no_zc"] = p->no_zc;
        peers.push_back(std::move(pj));
      }
    }
    crow::json::wvalue resp;
    resp["peers"] = std::move(peers);
    resp["count"] = peers.size();
    return crow::response(200, resp);
  });
}

// -- HD peer management routes -----------------------------------------------

static void RegisterHdRoutes(MetricsServer* ms) {
  if (!ms->hd_peers) return;

  // List all HD peers.
  CROW_ROUTE(ms->app, "/api/v1/peers")
  ([ms]() {
    // Snapshot under lock, build JSON outside.
    struct PeerSnap {
      Key key;
      HdPeerState state;
      int fd;
      int rule_count;
      Key rules[kHdMaxForwardRules];
    };
    std::vector<PeerSnap> snaps;
    {
      std::lock_guard lock(ms->hd_peers->mutex);
      for (int i = 0; i < kHdMaxPeers; i++) {
        auto& p = ms->hd_peers->peers[i];
        if (p.occupied != 1) continue;
        PeerSnap s;
        s.key = p.key;
        s.state = p.state;
        s.fd = p.fd;
        s.rule_count = p.rule_count;
        for (int r = 0; r < p.rule_count; r++) {
          s.rules[r] = p.rules[r].dst_key;
        }
        snaps.push_back(s);
      }
    }
    crow::json::wvalue::list peers;
    for (auto& s : snaps) {
      crow::json::wvalue pj;
      char hex[kKeySize * 2 + 1];
      KeyToHex(s.key, hex);
      pj["key"] = std::string(hex);
      pj["state"] = s.state == HdPeerState::kApproved
          ? "approved"
          : s.state == HdPeerState::kPending
          ? "pending" : "denied";
      pj["fd"] = s.fd;
      pj["rule_count"] = s.rule_count;
      crow::json::wvalue::list rules;
      for (int r = 0; r < s.rule_count; r++) {
        char dhex[kKeySize * 2 + 1];
        KeyToHex(s.rules[r], dhex);
        rules.push_back(std::string(dhex));
      }
      pj["rules"] = std::move(rules);
      peers.push_back(std::move(pj));
    }
    crow::json::wvalue resp;
    resp["count"] = static_cast<int>(peers.size());
    resp["peers"] = std::move(peers);
    return crow::response(200, resp);
  });

  // Approve a pending peer.
  CROW_ROUTE(ms->app, "/api/v1/peers/<string>/approve")
  .methods("POST"_method)
  ([ms](const std::string& key_hex) {
    Key key{};
    if (key_hex.size() != kKeySize * 2 ||
        sodium_hex2bin(
            key.data(), kKeySize,
            key_hex.c_str(), key_hex.size(),
            nullptr, nullptr, nullptr) != 0) {
      return crow::response(400, "invalid key hex");
    }
    int fd = -1;
    uint16_t pid = 0;
    {
      std::lock_guard lock(ms->hd_peers->mutex);
      if (!HdPeersApprove(ms->hd_peers, key.data())) {
        return crow::response(404, "peer not found");
      }
      auto* p = HdPeersLookup(ms->hd_peers,
                               key.data());
      if (p) {
        fd = p->fd;
        pid = p->peer_id;
      }
    }
    // I/O outside the lock.
    if (fd >= 0) {
      HdSendApproved(fd, key);
      timeval tv{.tv_sec = 0, .tv_usec = 0};
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                 &tv, sizeof(tv));
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                 &tv, sizeof(tv));
      DpAddPeer(ms->ctx, fd, key,
                PeerProtocol::kHd, pid);
    }
    return crow::response(200, "approved");
  });

  // Deny a pending peer.
  CROW_ROUTE(ms->app, "/api/v1/peers/<string>/deny")
  .methods("POST"_method)
  ([ms](const std::string& key_hex) {
    Key key{};
    if (key_hex.size() != kKeySize * 2 ||
        sodium_hex2bin(
            key.data(), kKeySize,
            key_hex.c_str(), key_hex.size(),
            nullptr, nullptr, nullptr) != 0) {
      return crow::response(400, "invalid key hex");
    }
    int fd = -1;
    {
      std::lock_guard lock(ms->hd_peers->mutex);
      auto* p = HdPeersLookup(ms->hd_peers,
                               key.data());
      if (!p) {
        return crow::response(404, "peer not found");
      }
      fd = p->fd;
      HdPeersDeny(ms->hd_peers, key.data());
    }
    if (fd >= 0) {
      HdSendDenied(fd, 0, "denied by admin");
      close(fd);
    }
    return crow::response(200, "denied");
  });

  // Revoke (delete) a peer.
  CROW_ROUTE(ms->app, "/api/v1/peers/<string>")
  .methods("DELETE"_method)
  ([ms](const std::string& key_hex) {
    Key key{};
    if (key_hex.size() != kKeySize * 2 ||
        sodium_hex2bin(
            key.data(), kKeySize,
            key_hex.c_str(), key_hex.size(),
            nullptr, nullptr, nullptr) != 0) {
      return crow::response(400, "invalid key hex");
    }
    int fd = -1;
    {
      std::lock_guard lock(ms->hd_peers->mutex);
      auto* p = HdPeersLookup(ms->hd_peers,
                               key.data());
      if (!p) {
        return crow::response(404, "peer not found");
      }
      fd = p->fd;
      HdPeersRemove(ms->hd_peers, key.data());
    }
    if (fd >= 0) {
      DpRemovePeer(ms->ctx, key);
      close(fd);
    }
    return crow::response(200, "revoked");
  });

  // Add a forwarding rule for a peer.
  CROW_ROUTE(ms->app, "/api/v1/peers/<string>/rules")
  .methods("POST"_method)
  ([ms](const crow::request& req,
        const std::string& key_hex) {
    auto body = crow::json::load(req.body);
    if (!body || !body.has("dst_key")) {
      return crow::response(400, "need dst_key");
    }
    std::string dst_hex = body["dst_key"].s();
    Key peer_key{}, dst_key{};
    if (key_hex.size() != kKeySize * 2 ||
        sodium_hex2bin(
            peer_key.data(), kKeySize,
            key_hex.c_str(), key_hex.size(),
            nullptr, nullptr, nullptr) != 0) {
      return crow::response(400, "invalid peer key");
    }
    if (dst_hex.size() != kKeySize * 2 ||
        sodium_hex2bin(
            dst_key.data(), kKeySize,
            dst_hex.c_str(), dst_hex.size(),
            nullptr, nullptr, nullptr) != 0) {
      return crow::response(400, "invalid dst_key");
    }
    {
      std::lock_guard lock(ms->hd_peers->mutex);
      if (!HdPeersAddRule(ms->hd_peers,
                          peer_key.data(), dst_key)) {
        return crow::response(
            400, "rule limit or peer not found");
      }
    }
    // Push the rule to the data plane (lock-free cmd).
    DpAddFwdRule(ms->ctx, peer_key, dst_key);
    return crow::response(200, "rule added");
  });

  // Relay status.
  CROW_ROUTE(ms->app, "/api/v1/relay")
  ([ms]() {
    crow::json::wvalue j;
    j["hd_enabled"] = (ms->hd_peers != nullptr);
    if (ms->hd_peers) {
      std::lock_guard lock(ms->hd_peers->mutex);
      j["hd_peer_count"] = ms->hd_peers->peer_count;
      j["hd_enroll_mode"] =
          ms->hd_peers->enroll_mode ==
              HdEnrollMode::kAutoApprove
          ? "auto" : "manual";
    }
    j["workers"] = ms->ctx->num_workers;
    return crow::response(200, j);
  });
}

// -- Public API --------------------------------------------------------------

MetricsServer* MetricsStart(const MetricsConfig& config,
                            Ctx* ctx,
                            HdPeerRegistry* hd_peers,
                            HdServerCounters hd_counters) {
  if (config.port == 0) {
    return nullptr;
  }

  auto* ms = new MetricsServer;
  ms->ctx = ctx;
  ms->hd_peers = hd_peers;
  ms->hd_counters = hd_counters;
  ms->start_time = std::chrono::steady_clock::now();

  RegisterRoutes(ms, config.enable_debug);
  RegisterHdRoutes(ms);

  // Suppress Crow's internal logging noise.
  ms->app.loglevel(crow::LogLevel::Warning);

  // TLS setup.
  if (!config.tls_cert.empty() &&
      !config.tls_key.empty()) {
    ms->app.ssl_file(config.tls_cert, config.tls_key);
    spdlog::info("metrics server TLS enabled");
  }

  ms->thread = std::thread([ms, port = config.port]() {
    spdlog::info("metrics server listening on port {}",
                 port);
    ms->app.port(port).concurrency(1).run();
  });

  return ms;
}

void MetricsStop(MetricsServer* server) {
  if (!server) return;
  server->app.stop();
  if (server->thread.joinable()) {
    server->thread.join();
  }
  spdlog::info("metrics server stopped");
  delete server;
}

}  // namespace hyper_derp
