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
#include "hyper_derp/key_format.h"
#include "hyper_derp/metrics.h"
#include "hyper_derp/protocol.h"

namespace hyper_derp {

struct MetricsServer {
  crow::SimpleApp app;
  std::thread thread;
  Ctx* ctx;
  HdPeerRegistry* hd_peers;
  HdServerCounters hd_counters;
  HdAuditRing* audit_ring = nullptr;
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
      pj["key_str"] = KeyToCkString(s.key);
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
        crow::json::wvalue rule;
        rule["key"] = std::string(dhex);
        rule["key_str"] = KeyToCkString(s.rules[r]);
        rules.push_back(std::move(rule));
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
    if (ParseKeyString(key_hex, &key) ==
        KeyPrefix::kInvalid) {
      return crow::response(400, "invalid key");
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
    if (ParseKeyString(key_hex, &key) ==
        KeyPrefix::kInvalid) {
      return crow::response(400, "invalid key");
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
    if (ParseKeyString(key_hex, &key) ==
        KeyPrefix::kInvalid) {
      return crow::response(400, "invalid key");
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
      HdPeersRevoke(ms->hd_peers, key.data());
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
    if (ParseKeyString(key_hex, &peer_key) ==
        KeyPrefix::kInvalid) {
      return crow::response(400, "invalid peer key");
    }
    if (ParseKeyString(dst_hex, &dst_key) ==
        KeyPrefix::kInvalid) {
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

  // Redirect a peer to a different relay.
  CROW_ROUTE(ms->app, "/api/v1/peers/<string>/redirect")
  .methods("POST"_method)
  ([ms](const crow::request& req,
        const std::string& key_hex) {
    auto body = crow::json::load(req.body);
    if (!body || !body.has("target_url")) {
      return crow::response(400, "need target_url");
    }
    std::string target_url = body["target_url"].s();
    // Default reason: Rebalancing.
    uint8_t reason_val = static_cast<uint8_t>(
        HdRedirectReason::kRebalancing);
    if (body.has("reason")) {
      reason_val =
          static_cast<uint8_t>(body["reason"].i());
    }
    Key key{};
    if (ParseKeyString(key_hex, &key) ==
        KeyPrefix::kInvalid) {
      return crow::response(400, "invalid key");
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
    }
    if (fd < 0) {
      return crow::response(409, "peer has no socket");
    }
    auto r = HdSendRedirect(
        fd, static_cast<HdRedirectReason>(reason_val),
        target_url);
    if (!r) {
      return crow::response(500, r.error().message);
    }
    return crow::response(200, "redirect sent");
  });

  // Peer routing-policy read.
  CROW_ROUTE(ms->app,
             "/api/v1/peers/<string>/policy")
  .methods("GET"_method)
  ([ms](const std::string& key_hex) {
    Key key{};
    if (ParseKeyString(key_hex, &key) ==
        KeyPrefix::kInvalid) {
      return crow::response(400, "invalid key");
    }
    HdPeerPolicy pol;
    {
      std::lock_guard lock(ms->hd_peers->mutex);
      auto* got =
          HdPeersLookupPolicy(ms->hd_peers, key.data());
      if (!got) {
        return crow::response(404, "peer not found");
      }
      pol = *got;
    }
    static const char* kIntentNames[] = {
        "prefer_direct", "require_direct",
        "prefer_relay", "require_relay"};
    crow::json::wvalue j;
    j["has_pin"] = pol.has_pin;
    j["override_client"] = pol.override_client;
    j["pinned_intent"] = kIntentNames[static_cast<int>(
        pol.pinned_intent) & 3];
    j["audit_tag"] = pol.audit_tag;
    j["reason"] = pol.reason;
    return crow::response(200, j);
  });

  // Peer routing-policy write.
  CROW_ROUTE(ms->app,
             "/api/v1/peers/<string>/policy")
  .methods("PUT"_method)
  ([ms](const crow::request& req,
        const std::string& key_hex) {
    Key key{};
    if (ParseKeyString(key_hex, &key) ==
        KeyPrefix::kInvalid) {
      return crow::response(400, "invalid key");
    }
    auto body = crow::json::load(req.body);
    if (!body) {
      return crow::response(400, "invalid json");
    }
    HdPeerPolicy pol;
    if (body.has("has_pin")) {
      pol.has_pin = body["has_pin"].b();
    }
    if (body.has("override_client")) {
      pol.override_client =
          body["override_client"].b();
    }
    if (body.has("pinned_intent")) {
      std::string s = body["pinned_intent"].s();
      if (s == "prefer_direct") {
        pol.pinned_intent = HdIntent::kPreferDirect;
      } else if (s == "require_direct") {
        pol.pinned_intent = HdIntent::kRequireDirect;
      } else if (s == "prefer_relay") {
        pol.pinned_intent = HdIntent::kPreferRelay;
      } else if (s == "require_relay") {
        pol.pinned_intent = HdIntent::kRequireRelay;
      } else {
        return crow::response(
            400, "invalid pinned_intent");
      }
    }
    if (body.has("audit_tag")) {
      pol.audit_tag = body["audit_tag"].s();
    }
    if (body.has("reason")) {
      pol.reason = body["reason"].s();
    }
    if (!HdPeersSetPolicy(ms->hd_peers, key.data(),
                          pol)) {
      return crow::response(404, "peer not found");
    }
    return crow::response(200, "policy updated");
  });

  // Peer routing-policy clear.
  CROW_ROUTE(ms->app,
             "/api/v1/peers/<string>/policy")
  .methods("DELETE"_method)
  ([ms](const std::string& key_hex) {
    Key key{};
    if (ParseKeyString(key_hex, &key) ==
        KeyPrefix::kInvalid) {
      return crow::response(400, "invalid key");
    }
    if (!HdPeersClearPolicy(ms->hd_peers, key.data())) {
      return crow::response(404, "peer not found");
    }
    return crow::response(200, "policy cleared");
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
      j["relay_id"] = ms->hd_peers->relay_id;
      j["denylist_size"] = static_cast<int>(
          ms->hd_peers->denylist.size());
    }
    j["workers"] = ms->ctx->num_workers;
    return crow::response(200, j);
  });

  // Generate a new random relay key. This is a helper for
  // admins bootstrapping a relay; the returned key must
  // then be set in the server config and distributed to
  // clients out of band. Does not mutate server state.
  CROW_ROUTE(ms->app, "/api/v1/relay/init")
  .methods("POST"_method)
  ([]() {
    Key k{};
    randombytes_buf(k.data(), kKeySize);
    crow::json::wvalue j;
    char hex[kKeySize * 2 + 1];
    sodium_bin2hex(hex, sizeof(hex),
                   k.data(), kKeySize);
    j["relay_key"] = std::string(hex);
    j["relay_key_str"] = KeyToRkString(k);
    j["note"] =
        "this key is shown once — set hd.relay_key in "
        "the server config and distribute to clients";
    return crow::response(200, j);
  });

  // Per-worker + per-peer traffic counters.
  CROW_ROUTE(ms->app, "/api/v1/counters")
  ([ms]() {
    crow::json::wvalue j;
    uint64_t total_rx = 0, total_tx = 0;
    uint64_t total_send_drops = 0;
    uint64_t total_mesh_fwd = 0, total_fleet_fwd = 0;
    crow::json::wvalue::list workers;
    for (int i = 0; i < ms->ctx->num_workers; i++) {
      Worker* w = ms->ctx->workers[i];
      if (!w) continue;
      crow::json::wvalue wj;
      wj["id"] = w->id;
      wj["recv_bytes"] =
          static_cast<int64_t>(w->stats.recv_bytes);
      wj["send_bytes"] =
          static_cast<int64_t>(w->stats.send_bytes);
      wj["send_drops"] =
          static_cast<int64_t>(w->stats.send_drops);
      wj["hd_mesh_forwards"] = static_cast<int64_t>(
          w->stats.hd_mesh_forwards);
      wj["hd_fleet_forwards"] = static_cast<int64_t>(
          w->stats.hd_fleet_forwards);
      total_rx += w->stats.recv_bytes;
      total_tx += w->stats.send_bytes;
      total_send_drops += w->stats.send_drops;
      total_mesh_fwd += w->stats.hd_mesh_forwards;
      total_fleet_fwd += w->stats.hd_fleet_forwards;
      workers.push_back(std::move(wj));
    }
    j["workers"] = std::move(workers);
    j["total_recv_bytes"] =
        static_cast<int64_t>(total_rx);
    j["total_send_bytes"] =
        static_cast<int64_t>(total_tx);
    j["total_send_drops"] =
        static_cast<int64_t>(total_send_drops);
    j["total_hd_mesh_forwards"] =
        static_cast<int64_t>(total_mesh_fwd);
    j["total_hd_fleet_forwards"] =
        static_cast<int64_t>(total_fleet_fwd);
    if (ms->hd_counters.enrollments) {
      j["hd_enrollments"] = static_cast<int64_t>(
          ms->hd_counters.enrollments->load(
              std::memory_order_relaxed));
    }
    if (ms->hd_counters.auth_failures) {
      j["hd_auth_failures"] = static_cast<int64_t>(
          ms->hd_counters.auth_failures->load(
              std::memory_order_relaxed));
    }
    return crow::response(200, j);
  });

  // Read-only runtime config view. Sensitive fields (keys,
  // denylist file contents) are redacted.
  CROW_ROUTE(ms->app, "/api/v1/config")
  ([ms]() {
    crow::json::wvalue j;
    j["workers"] = ms->ctx->num_workers;
    j["hd_enabled"] = (ms->hd_peers != nullptr);
    if (ms->hd_peers) {
      std::lock_guard lock(ms->hd_peers->mutex);
      j["hd_relay_id"] = ms->hd_peers->relay_id;
      j["hd_enroll_mode"] =
          ms->hd_peers->enroll_mode ==
              HdEnrollMode::kAutoApprove
          ? "auto" : "manual";
      const auto& pol = ms->hd_peers->policy;
      j["hd_policy_max_peers"] = pol.max_peers;
      j["hd_policy_require_ip_range"] =
          pol.require_ip_range;
      crow::json::wvalue::list ak;
      for (const auto& p : pol.allowed_keys) {
        ak.push_back(p);
      }
      j["hd_policy_allowed_keys"] = std::move(ak);
      j["hd_denylist_path"] =
          ms->hd_peers->denylist_path;
      j["hd_denylist_size"] =
          static_cast<int>(ms->hd_peers->denylist.size());
    }
    return crow::response(200, j);
  });

  // Routing-policy audit records (newest first).
  CROW_ROUTE(ms->app, "/api/v1/audit/recent")
  ([ms](const crow::request& req) {
    if (!ms->audit_ring) {
      return crow::response(503,
                            "audit ring not wired");
    }
    int limit = 100;
    auto qs_limit = req.url_params.get("limit");
    if (qs_limit) {
      try {
        limit = std::stoi(qs_limit);
      } catch (...) {}
    }
    if (limit <= 0) limit = 1;
    if (limit > kHdAuditRingSize) {
      limit = kHdAuditRingSize;
    }
    auto qs_peer = req.url_params.get("peer");
    Key filter_key{};
    bool have_filter = false;
    if (qs_peer) {
      if (ParseKeyString(qs_peer, &filter_key) !=
          KeyPrefix::kInvalid) {
        have_filter = true;
      } else {
        return crow::response(400, "invalid peer key");
      }
    }
    std::vector<HdAuditRecord> snap(limit);
    int n = HdAuditSnapshot(ms->audit_ring, snap.data(),
                            limit);
    crow::json::wvalue::list arr;
    for (int i = 0; i < n; i++) {
      const auto& r = snap[i];
      if (have_filter &&
          std::memcmp(r.client_key.data(),
                      filter_key.data(), kKeySize) != 0 &&
          std::memcmp(r.target_key.data(),
                      filter_key.data(), kKeySize) != 0) {
        continue;
      }
      // Parse the LD-JSON line back to crow JSON so the
      // top-level response is a proper JSON array rather
      // than a string blob.
      std::string line = HdAuditToJson(r);
      arr.push_back(
          crow::json::load(line.data(), line.size()));
    }
    crow::json::wvalue j;
    j["count"] = static_cast<int>(arr.size());
    j["records"] = std::move(arr);
    return crow::response(200, j);
  });
}

// -- HTMX admin UI -----------------------------------------------------------

namespace {

// Minimal HTML shell. The peer table is loaded and
// refreshed by htmx every 2s; action buttons POST via
// hx-post and swap the updated fragment in place.
constexpr const char* kAdminIndexHtml = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>hyper-derp admin</title>
<script src="https://unpkg.com/htmx.org@1.9.12"></script>
<style>
body{font:14px/1.4 ui-monospace,monospace;margin:2em;
  max-width:1100px}
h1{font-size:1.1em;margin:0 0 1em}
table{border-collapse:collapse;width:100%;margin:.5em 0}
th,td{padding:.3em .6em;border-bottom:1px solid #ddd;
  text-align:left;vertical-align:top}
th{background:#f6f6f6;font-weight:600}
.key{font-size:.8em;word-break:break-all;max-width:420px}
.state{text-transform:uppercase;font-size:.78em;
  letter-spacing:.05em;padding:.1em .4em;border-radius:3px}
.state.pending{background:#fef3c7;color:#854d0e}
.state.approved{background:#dcfce7;color:#166534}
.state.denied{background:#fee2e2;color:#991b1b}
button{font:inherit;padding:.2em .6em;margin-right:.3em;
  cursor:pointer}
.muted{color:#888}
.topbar{display:flex;justify-content:space-between;
  align-items:center;margin-bottom:1em}
</style>
</head>
<body>
<div class="topbar">
<h1>hyper-derp admin</h1>
<div id="relay-status" hx-get="/admin/status"
  hx-trigger="load, every 5s" hx-swap="innerHTML"></div>
</div>
<nav style="margin-bottom:1em">
<a href="#" hx-get="/admin/peers" hx-target="#peers"
  hx-swap="innerHTML">peers</a> |
<a href="#" hx-get="/admin/audit" hx-target="#peers"
  hx-swap="innerHTML">audit</a>
</nav>
<div id="peers" hx-get="/admin/peers"
  hx-trigger="load, every 2s" hx-swap="innerHTML"></div>
</body>
</html>
)HTML";

const char* IntentName(HdIntent i) {
  switch (i) {
    case HdIntent::kPreferDirect:
      return "prefer_direct";
    case HdIntent::kRequireDirect:
      return "require_direct";
    case HdIntent::kPreferRelay:
      return "prefer_relay";
    case HdIntent::kRequireRelay:
      return "require_relay";
  }
  return "?";
}

std::string RenderPolicyCell(const HdPeerPolicy& pol) {
  std::string out;
  if (!pol.has_pin && !pol.override_client &&
      pol.audit_tag.empty()) {
    out = "<span class=\"muted\">—</span>";
  } else {
    if (pol.has_pin) {
      out += IntentName(pol.pinned_intent);
      if (pol.override_client) {
        out += " (override)";
      }
    } else {
      out += "<span class=\"muted\">no pin</span>";
    }
    if (!pol.audit_tag.empty()) {
      out += " <em>[";
      out += pol.audit_tag;
      out += "]</em>";
    }
  }
  return out;
}

std::string RenderPeerRow(const std::string& hex,
                          const std::string& state,
                          int fd,
                          int rule_count,
                          const HdPeerPolicy& pol) {
  std::string row;
  row.reserve(768);
  row += "<tr><td class=\"key\">";
  row += hex;
  row += "</td><td><span class=\"state ";
  row += state;
  row += "\">";
  row += state;
  row += "</span></td><td>";
  row += std::to_string(fd);
  row += "</td><td>";
  row += std::to_string(rule_count);
  row += "</td><td>";
  row += RenderPolicyCell(pol);
  row += "</td><td>";
  if (state == "pending") {
    row += "<button hx-post=\"/admin/peers/";
    row += hex;
    row += "/approve\" hx-target=\"#peers\" "
           "hx-swap=\"innerHTML\">approve</button>";
    row += "<button hx-post=\"/admin/peers/";
    row += hex;
    row += "/deny\" hx-target=\"#peers\" "
           "hx-swap=\"innerHTML\">deny</button>";
  } else if (state == "approved") {
    row += "<button hx-get=\"/admin/peers/";
    row += hex;
    row += "/policy\" hx-target=\"#peers\" "
           "hx-swap=\"innerHTML\">edit policy</button>";
    row += "<button hx-delete=\"/admin/peers/";
    row += hex;
    row += "\" hx-target=\"#peers\" "
           "hx-swap=\"innerHTML\" "
           "hx-confirm=\"Revoke this peer? "
           "Key will be added to the denylist.\">"
           "revoke</button>";
  }
  row += "</td></tr>";
  return row;
}

std::string RenderPolicyForm(const std::string& hex,
                             const HdPeerPolicy& pol) {
  auto opt = [&](const char* v,
                 HdIntent want) -> std::string {
    std::string o = "<option value=\"";
    o += v;
    if (pol.has_pin && pol.pinned_intent == want) {
      o += "\" selected>";
    } else {
      o += "\">";
    }
    o += v;
    o += "</option>";
    return o;
  };
  std::string h;
  h.reserve(1024);
  h += "<div style=\"padding:1em;border:1px solid #ccc;"
       "border-radius:4px\">";
  h += "<h3 style=\"margin-top:0\">policy for ";
  h += hex;
  h += "</h3>";
  h += "<form hx-put=\"/admin/peers/";
  h += hex;
  h += "/policy\" hx-ext=\"json-enc\" "
       "hx-target=\"#peers\" hx-swap=\"innerHTML\">";
  h += "<label><input type=\"checkbox\" name=\"has_pin\"";
  if (pol.has_pin) h += " checked";
  h += "> has_pin</label><br>";
  h += "<label><input type=\"checkbox\" "
       "name=\"override_client\"";
  if (pol.override_client) h += " checked";
  h += "> override_client</label><br>";
  h += "<label>pinned_intent: "
       "<select name=\"pinned_intent\">";
  h += opt("prefer_direct", HdIntent::kPreferDirect);
  h += opt("require_direct", HdIntent::kRequireDirect);
  h += opt("prefer_relay", HdIntent::kPreferRelay);
  h += opt("require_relay", HdIntent::kRequireRelay);
  h += "</select></label><br>";
  h += "<label>audit_tag: <input name=\"audit_tag\" "
       "value=\"";
  h += pol.audit_tag;
  h += "\"></label><br>";
  h += "<label>reason: <input name=\"reason\" value=\"";
  h += pol.reason;
  h += "\"></label><br>";
  h += "<button type=\"submit\">save</button> ";
  h += "<button type=\"button\" "
       "hx-get=\"/admin/peers\" hx-target=\"#peers\" "
       "hx-swap=\"innerHTML\">cancel</button> ";
  h += "<button type=\"button\" hx-delete=\"/admin/peers/";
  h += hex;
  h += "/policy\" hx-target=\"#peers\" "
       "hx-swap=\"innerHTML\">clear</button>";
  h += "</form></div>";
  return h;
}

std::string RenderPeerList(HdPeerRegistry* reg) {
  struct Snap {
    std::string hex;
    std::string state;
    int fd;
    int rule_count;
    HdPeerPolicy policy;
  };
  std::vector<Snap> snaps;
  {
    std::lock_guard lock(reg->mutex);
    for (int i = 0; i < kHdMaxPeers; i++) {
      auto& p = reg->peers[i];
      if (p.occupied != 1) continue;
      char hex[kKeySize * 2 + 1];
      KeyToHex(p.key, hex);
      Snap s;
      s.hex = hex;
      s.state = p.state == HdPeerState::kApproved
                    ? "approved"
                    : p.state == HdPeerState::kPending
                    ? "pending" : "denied";
      s.fd = p.fd;
      s.rule_count = p.rule_count;
      s.policy = reg->policies[i];
      snaps.push_back(std::move(s));
    }
  }
  std::string html;
  html.reserve(4096);
  html += "<table><thead><tr><th>peer key</th>"
          "<th>state</th><th>fd</th><th>rules</th>"
          "<th>policy</th>"
          "<th>actions</th></tr></thead><tbody>";
  if (snaps.empty()) {
    html += "<tr><td colspan=\"6\" class=\"muted\">"
            "no peers</td></tr>";
  }
  for (auto& s : snaps) {
    html += RenderPeerRow(s.hex, s.state, s.fd,
                          s.rule_count, s.policy);
  }
  html += "</tbody></table>";
  return html;
}

}  // namespace

static void RegisterAdminRoutes(MetricsServer* ms) {
  if (!ms->hd_peers) return;

  CROW_ROUTE(ms->app, "/admin")
  ([]() {
    crow::response r(200, kAdminIndexHtml);
    r.add_header("Content-Type", "text/html");
    return r;
  });

  CROW_ROUTE(ms->app, "/admin/peers")
  ([ms]() {
    crow::response r(200, RenderPeerList(ms->hd_peers));
    r.add_header("Content-Type", "text/html");
    return r;
  });

  CROW_ROUTE(ms->app, "/admin/audit")
  ([ms]() {
    std::string html;
    html.reserve(8192);
    html += "<table><thead><tr><th>ts</th>"
            "<th>client</th><th>target</th>"
            "<th>intent</th><th>decision</th>"
            "<th>reason</th></tr></thead><tbody>";
    if (!ms->audit_ring) {
      html += "<tr><td colspan=\"6\" class=\"muted\">"
              "audit ring not wired</td></tr>";
    } else {
      std::vector<HdAuditRecord> snap(50);
      int n = HdAuditSnapshot(ms->audit_ring,
                              snap.data(), 50);
      if (n == 0) {
        html += "<tr><td colspan=\"6\" "
                "class=\"muted\">no decisions "
                "yet</td></tr>";
      }
      for (int i = 0; i < n; i++) {
        const auto& r = snap[i];
        time_t secs = static_cast<time_t>(
            r.ts_ns / 1000000000ULL);
        char ts[32];
        struct tm tmv{};
        gmtime_r(&secs, &tmv);
        strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);
        static const char* kIntent[] = {
            "prefer_direct", "require_direct",
            "prefer_relay", "require_relay"};
        static const char* kMode[] = {"denied",
                                       "direct",
                                       "relayed"};
        char ck[kKeySize * 2 + 1];
        char tk[kKeySize * 2 + 1];
        KeyToHex(r.client_key, ck);
        KeyToHex(r.target_key, tk);
        html += "<tr><td>";
        html += ts;
        html += "</td><td class=\"key\">";
        html += std::string(ck, 16);
        html += "...</td><td class=\"key\">";
        html += std::string(tk, 16);
        html += "...</td><td>";
        html += kIntent[
            static_cast<int>(r.client_intent) & 3];
        html += "</td><td><span class=\"state ";
        html += kMode[
            static_cast<int>(r.mode) & 3];
        html += "\">";
        html += kMode[
            static_cast<int>(r.mode) & 3];
        html += "</span></td><td>";
        if (r.deny_reason != HdDenyReason::kNone) {
          html += "0x";
          char hex[8];
          snprintf(hex, sizeof(hex), "%04x",
                   static_cast<int>(r.deny_reason));
          html += hex;
        } else {
          html += "—";
        }
        html += "</td></tr>";
      }
    }
    html += "</tbody></table>";
    crow::response r(200, html);
    r.add_header("Content-Type", "text/html");
    return r;
  });

  CROW_ROUTE(ms->app, "/admin/status")
  ([ms]() {
    std::string html;
    {
      std::lock_guard lock(ms->hd_peers->mutex);
      html += "<span class=\"muted\">relay_id=";
      html += std::to_string(ms->hd_peers->relay_id);
      html += " · peers=";
      html += std::to_string(ms->hd_peers->peer_count);
      html += " · denylist=";
      html += std::to_string(
          ms->hd_peers->denylist.size());
      html += " · mode=";
      html += ms->hd_peers->enroll_mode ==
                      HdEnrollMode::kAutoApprove
                  ? "auto"
                  : "manual";
      html += "</span>";
    }
    crow::response r(200, html);
    r.add_header("Content-Type", "text/html");
    return r;
  });

  CROW_ROUTE(ms->app,
             "/admin/peers/<string>/approve")
  .methods("POST"_method)
  ([ms](const std::string& key_hex) {
    Key key{};
    if (ParseKeyString(key_hex, &key) ==
        KeyPrefix::kInvalid) {
      return crow::response(400, "invalid key");
    }
    int fd = -1;
    uint16_t pid = 0;
    {
      std::lock_guard lock(ms->hd_peers->mutex);
      if (!HdPeersApprove(ms->hd_peers, key.data())) {
        return crow::response(404, "peer not found");
      }
      auto* p = HdPeersLookup(ms->hd_peers, key.data());
      if (p) {
        fd = p->fd;
        pid = p->peer_id;
      }
    }
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
    crow::response r(200, RenderPeerList(ms->hd_peers));
    r.add_header("Content-Type", "text/html");
    return r;
  });

  CROW_ROUTE(ms->app, "/admin/peers/<string>/deny")
  .methods("POST"_method)
  ([ms](const std::string& key_hex) {
    Key key{};
    if (ParseKeyString(key_hex, &key) ==
        KeyPrefix::kInvalid) {
      return crow::response(400, "invalid key");
    }
    int fd = -1;
    {
      std::lock_guard lock(ms->hd_peers->mutex);
      auto* p = HdPeersLookup(ms->hd_peers, key.data());
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
    crow::response r(200, RenderPeerList(ms->hd_peers));
    r.add_header("Content-Type", "text/html");
    return r;
  });

  // Admin: GET policy — returns an edit-form fragment.
  CROW_ROUTE(ms->app,
             "/admin/peers/<string>/policy")
  .methods("GET"_method)
  ([ms](const std::string& key_hex) {
    Key key{};
    if (ParseKeyString(key_hex, &key) ==
        KeyPrefix::kInvalid) {
      return crow::response(400, "invalid key");
    }
    HdPeerPolicy pol;
    {
      std::lock_guard lock(ms->hd_peers->mutex);
      auto* got =
          HdPeersLookupPolicy(ms->hd_peers, key.data());
      if (!got) {
        return crow::response(404, "peer not found");
      }
      pol = *got;
    }
    crow::response r(200, RenderPolicyForm(key_hex, pol));
    r.add_header("Content-Type", "text/html");
    return r;
  });

  // Admin: PUT policy — form-url-encoded from HTMX.
  CROW_ROUTE(ms->app,
             "/admin/peers/<string>/policy")
  .methods("PUT"_method)
  ([ms](const crow::request& req,
        const std::string& key_hex) {
    Key key{};
    if (ParseKeyString(key_hex, &key) ==
        KeyPrefix::kInvalid) {
      return crow::response(400, "invalid key");
    }
    // Parse application/x-www-form-urlencoded body.
    HdPeerPolicy pol;
    std::string body = req.body;
    auto field = [&](const std::string& name)
        -> std::string {
      size_t pos = 0;
      while (pos < body.size()) {
        size_t eq = body.find('=', pos);
        size_t amp = body.find('&', pos);
        if (amp == std::string::npos) amp = body.size();
        if (eq != std::string::npos && eq < amp) {
          if (body.compare(pos, eq - pos, name) == 0) {
            std::string v =
                body.substr(eq + 1, amp - eq - 1);
            // Minimal URL-decode: + -> space, %XX -> byte.
            std::string out;
            for (size_t i = 0; i < v.size(); i++) {
              char c = v[i];
              if (c == '+') {
                out += ' ';
              } else if (c == '%' && i + 2 < v.size()) {
                int hi = std::stoi(
                    v.substr(i + 1, 2), nullptr, 16);
                out += static_cast<char>(hi);
                i += 2;
              } else {
                out += c;
              }
            }
            return out;
          }
        }
        pos = amp + 1;
      }
      return {};
    };
    pol.has_pin = !field("has_pin").empty();
    pol.override_client =
        !field("override_client").empty();
    std::string pin = field("pinned_intent");
    if (pin == "prefer_direct") {
      pol.pinned_intent = HdIntent::kPreferDirect;
    } else if (pin == "require_direct") {
      pol.pinned_intent = HdIntent::kRequireDirect;
    } else if (pin == "prefer_relay") {
      pol.pinned_intent = HdIntent::kPreferRelay;
    } else if (pin == "require_relay") {
      pol.pinned_intent = HdIntent::kRequireRelay;
    }
    pol.audit_tag = field("audit_tag");
    pol.reason = field("reason");
    if (!HdPeersSetPolicy(ms->hd_peers, key.data(),
                          pol)) {
      return crow::response(404, "peer not found");
    }
    crow::response r(200, RenderPeerList(ms->hd_peers));
    r.add_header("Content-Type", "text/html");
    return r;
  });

  // Admin: DELETE policy — clears to defaults.
  CROW_ROUTE(ms->app,
             "/admin/peers/<string>/policy")
  .methods("DELETE"_method)
  ([ms](const std::string& key_hex) {
    Key key{};
    if (ParseKeyString(key_hex, &key) ==
        KeyPrefix::kInvalid) {
      return crow::response(400, "invalid key");
    }
    if (!HdPeersClearPolicy(ms->hd_peers, key.data())) {
      return crow::response(404, "peer not found");
    }
    crow::response r(200, RenderPeerList(ms->hd_peers));
    r.add_header("Content-Type", "text/html");
    return r;
  });

  CROW_ROUTE(ms->app, "/admin/peers/<string>")
  .methods("DELETE"_method)
  ([ms](const std::string& key_hex) {
    Key key{};
    if (ParseKeyString(key_hex, &key) ==
        KeyPrefix::kInvalid) {
      return crow::response(400, "invalid key");
    }
    int fd = -1;
    {
      std::lock_guard lock(ms->hd_peers->mutex);
      auto* p = HdPeersLookup(ms->hd_peers, key.data());
      if (!p) {
        return crow::response(404, "peer not found");
      }
      fd = p->fd;
      HdPeersRevoke(ms->hd_peers, key.data());
    }
    if (fd >= 0) {
      DpRemovePeer(ms->ctx, key);
      close(fd);
    }
    crow::response r(200, RenderPeerList(ms->hd_peers));
    r.add_header("Content-Type", "text/html");
    return r;
  });
}

// -- Public API --------------------------------------------------------------

MetricsServer* MetricsStart(const MetricsConfig& config,
                            Ctx* ctx,
                            HdPeerRegistry* hd_peers,
                            HdServerCounters hd_counters,
                            HdAuditRing* audit_ring) {
  if (config.port == 0) {
    return nullptr;
  }

  auto* ms = new MetricsServer;
  ms->ctx = ctx;
  ms->hd_peers = hd_peers;
  ms->hd_counters = hd_counters;
  ms->audit_ring = audit_ring;
  ms->start_time = std::chrono::steady_clock::now();

  RegisterRoutes(ms, config.enable_debug);
  RegisterHdRoutes(ms);
  RegisterAdminRoutes(ms);

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
