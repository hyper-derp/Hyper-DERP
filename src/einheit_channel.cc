/// @file einheit_channel.cc
/// @brief einheit ROUTER/PUB channel + handler registry.
///
/// Wire-command dispatch on the ROUTER; handlers read Server
/// state, format a UTF-8 key=value body, and hand it back in
/// Response::data. The HD adapter's renderers parse that shape
/// today; upgrading individual handlers to MessagePack maps is
/// a follow-up when typed shapes pay off.

#include "hyper_derp/einheit_channel.h"

#include <atomic>
#include <cstring>
#include <format>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>

#include <sodium.h>
#include <spdlog/spdlog.h>
#include <zmq.hpp>

#include "hyper_derp/data_plane.h"
#include "hyper_derp/einheit_protocol.h"
#include "hyper_derp/fleet_controller.h"
#include "hyper_derp/handshake.h"
#include "hyper_derp/hd_audit.h"
#include "hyper_derp/hd_handshake.h"
#include "hyper_derp/hd_peers.h"
#include "hyper_derp/hd_protocol.h"
#include "hyper_derp/hd_relay_table.h"
#include "hyper_derp/key_format.h"
#include "hyper_derp/server.h"
#include "hyper_derp/types.h"

namespace hyper_derp {

namespace {

using einheit::Request;
using einheit::Response;
using einheit::ResponseError;
using einheit::ResponseStatus;

// Handlers produce a plain UTF-8 body consumed by the HD
// adapter's existing key=value parser. Each line is either
// `key=value`, or for list/table commands
// `prefix.<n>.field=value`.
using Handler =
    std::function<void(Server*, const Request&, Response*)>;

auto ErrorOf(std::string code, std::string msg,
             std::string hint = {}) -> ResponseError {
  return ResponseError{std::move(code), std::move(msg),
                        std::move(hint)};
}

void SetBody(Response* r, const std::string& s) {
  r->data.assign(s.begin(), s.end());
}

// -- Key helpers ---------------------------------------

bool ResolveKey(const Request& req, int arg_idx,
                Key* out, Response* resp) {
  if (static_cast<int>(req.args.size()) <= arg_idx) {
    resp->status = ResponseStatus::kError;
    resp->error = ErrorOf(
        "missing_arg",
        std::format("missing positional arg #{}",
                    arg_idx));
    return false;
  }
  if (ParseKeyString(req.args[arg_idx], out) ==
      KeyPrefix::kInvalid) {
    resp->status = ResponseStatus::kError;
    resp->error = ErrorOf(
        "invalid_key",
        "key must be rk_<hex>, ck_<hex>, or raw 64 hex");
    return false;
  }
  return true;
}

std::string KeyHex(const Key& k) {
  char hex[kKeySize * 2 + 1];
  KeyToHex(k, hex);
  return std::string(hex);
}

// -- Handlers ------------------------------------------

void ShowStatus(Server* s, const Request& /*req*/,
                Response* r) {
  std::string b;
  b += std::format("status=ok\n");
  b += std::format("workers={}\n",
                   s->data_plane.num_workers);
  uint64_t recv = 0, send = 0;
  int peers = 0;
  for (int i = 0; i < s->data_plane.num_workers; i++) {
    auto* w = s->data_plane.workers[i];
    if (!w) continue;
    recv += __atomic_load_n(&w->stats.recv_bytes,
                            __ATOMIC_RELAXED);
    send += __atomic_load_n(&w->stats.send_bytes,
                            __ATOMIC_RELAXED);
    for (int j = 0; j < kHtCapacity; j++) {
      if (w->ht[j].occupied == 1) peers++;
    }
  }
  b += std::format("recv_bytes={}\n", recv);
  b += std::format("send_bytes={}\n", send);
  b += std::format("peers_active={}\n", peers);
  b += std::format("hd_enabled={}\n",
                   s->hd_enabled ? "true" : "false");
  if (s->hd_enabled) {
    std::lock_guard lock(s->hd_peers.mutex);
    b += std::format("hd_peer_count={}\n",
                     s->hd_peers.peer_count);
    b += std::format("hd_denylist_size={}\n",
                     s->hd_peers.denylist.size());
    b += std::format("hd_relay_id={}\n",
                     s->hd_peers.relay_id);
    b += std::format(
        "hd_enroll_mode={}\n",
        s->hd_peers.enroll_mode ==
                HdEnrollMode::kAutoApprove
            ? "auto"
            : "manual");
  }
  SetBody(r, b);
}

void ShowPeers(Server* s, const Request& /*req*/,
               Response* r) {
  if (!s->hd_enabled) {
    SetBody(r, "peer.count=0\n");
    return;
  }
  std::string b;
  std::lock_guard lock(s->hd_peers.mutex);
  int idx = 0;
  for (int i = 0; i < kHdMaxPeers; i++) {
    const auto& p = s->hd_peers.peers[i];
    if (p.occupied != 1) continue;
    const auto& pol = s->hd_peers.policies[i];
    const char* state =
        p.state == HdPeerState::kApproved   ? "approved"
        : p.state == HdPeerState::kPending  ? "pending"
                                             : "denied";
    b += std::format("peer.{}.key={}\n", idx,
                     KeyToCkString(p.key));
    b += std::format("peer.{}.state={}\n", idx, state);
    b += std::format("peer.{}.fd={}\n", idx, p.fd);
    b += std::format("peer.{}.peer_id={}\n", idx,
                     p.peer_id);
    b += std::format("peer.{}.rules={}\n", idx,
                     p.rule_count);
    std::string policy_s;
    if (pol.has_pin) {
      const char* names[] = {
          "prefer_direct", "require_direct",
          "prefer_relay", "require_relay"};
      policy_s = names[static_cast<int>(
                            pol.pinned_intent) &
                        3];
      if (pol.override_client) policy_s += " (override)";
    }
    if (!pol.audit_tag.empty()) {
      if (!policy_s.empty()) policy_s += " ";
      policy_s += std::format("[{}]", pol.audit_tag);
    }
    b += std::format("peer.{}.policy={}\n", idx, policy_s);
    idx++;
  }
  b += std::format("peer.count={}\n", idx);
  SetBody(r, b);
}

void ShowPeer(Server* s, const Request& req,
              Response* r) {
  Key k{};
  if (!ResolveKey(req, 0, &k, r)) return;
  std::lock_guard lock(s->hd_peers.mutex);
  auto* p = HdPeersLookup(&s->hd_peers, k.data());
  if (!p) {
    r->status = ResponseStatus::kError;
    r->error = ErrorOf("not_found", "peer not found");
    return;
  }
  const auto* pol =
      HdPeersLookupPolicy(&s->hd_peers, k.data());
  std::string b;
  b += std::format("key={}\n", KeyToCkString(p->key));
  b += std::format(
      "state={}\n",
      p->state == HdPeerState::kApproved   ? "approved"
      : p->state == HdPeerState::kPending  ? "pending"
                                            : "denied");
  b += std::format("fd={}\n", p->fd);
  b += std::format("peer_id={}\n", p->peer_id);
  b += std::format("rule_count={}\n", p->rule_count);
  for (int i = 0; i < p->rule_count; i++) {
    b += std::format("rule.{}={}\n", i,
                     KeyToCkString(p->rules[i].dst_key));
  }
  if (pol) {
    b += std::format("policy.has_pin={}\n",
                     pol->has_pin ? "true" : "false");
    if (pol->has_pin) {
      const char* names[] = {
          "prefer_direct", "require_direct",
          "prefer_relay", "require_relay"};
      b += std::format(
          "policy.pinned_intent={}\n",
          names[static_cast<int>(pol->pinned_intent) &
                 3]);
    }
    b += std::format(
        "policy.override_client={}\n",
        pol->override_client ? "true" : "false");
    b += std::format("policy.audit_tag={}\n",
                     pol->audit_tag);
    b += std::format("policy.reason={}\n", pol->reason);
  }
  SetBody(r, b);
}

void ShowAudit(Server* s, const Request& req,
               Response* r) {
  int limit = 50;
  auto it = req.flags.find("limit");
  if (it != req.flags.end()) {
    try {
      limit = std::stoi(it->second);
    } catch (...) {
    }
  }
  if (limit < 1) limit = 1;
  if (limit > kHdAuditRingSize) limit = kHdAuditRingSize;
  std::vector<HdAuditRecord> snap(limit);
  int n = HdAuditSnapshot(&s->control_plane.audit_ring,
                          snap.data(), limit);
  std::string b;
  static const char* kIntent[] = {
      "prefer_direct", "require_direct", "prefer_relay",
      "require_relay"};
  static const char* kMode[] = {"denied", "direct",
                                 "relayed"};
  for (int i = 0; i < n; i++) {
    const auto& rec = snap[i];
    time_t secs =
        static_cast<time_t>(rec.ts_ns / 1000000000ULL);
    char ts[32];
    struct tm tmv{};
    gmtime_r(&secs, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    b += std::format("audit.{}.ts={}\n", i, ts);
    b += std::format("audit.{}.client={}\n", i,
                     KeyToCkString(rec.client_key));
    b += std::format("audit.{}.target={}\n", i,
                     KeyToCkString(rec.target_key));
    b += std::format(
        "audit.{}.intent={}\n", i,
        kIntent[static_cast<int>(rec.client_intent) & 3]);
    b += std::format(
        "audit.{}.decision={}\n", i,
        kMode[static_cast<int>(rec.mode) & 3]);
    if (rec.deny_reason != HdDenyReason::kNone) {
      b += std::format("audit.{}.reason=0x{:04x}\n", i,
                       static_cast<int>(rec.deny_reason));
    } else {
      b += std::format("audit.{}.reason=\n", i);
    }
  }
  b += std::format("audit.count={}\n", n);
  SetBody(r, b);
}

void ShowCounters(Server* s, const Request& /*req*/,
                  Response* r) {
  uint64_t total_rx = 0, total_tx = 0, total_drops = 0;
  std::string b;
  for (int i = 0; i < s->data_plane.num_workers; i++) {
    auto* w = s->data_plane.workers[i];
    if (!w) continue;
    uint64_t rx = __atomic_load_n(&w->stats.recv_bytes,
                                   __ATOMIC_RELAXED);
    uint64_t tx = __atomic_load_n(&w->stats.send_bytes,
                                   __ATOMIC_RELAXED);
    uint64_t dr = __atomic_load_n(&w->stats.send_drops,
                                   __ATOMIC_RELAXED);
    b += std::format("worker.{}.recv_bytes={}\n", i, rx);
    b += std::format("worker.{}.send_bytes={}\n", i, tx);
    b += std::format("worker.{}.send_drops={}\n", i, dr);
    total_rx += rx;
    total_tx += tx;
    total_drops += dr;
  }
  b += std::format("total.recv_bytes={}\n", total_rx);
  b += std::format("total.send_bytes={}\n", total_tx);
  b += std::format("total.send_drops={}\n", total_drops);
  b += std::format(
      "hd.enrollments={}\n",
      s->hd_enrollments.load(std::memory_order_relaxed));
  b += std::format("hd.auth_failures={}\n",
                   s->hd_auth_failures.load(
                       std::memory_order_relaxed));
  SetBody(r, b);
}

void ShowConfig(Server* s, const Request& /*req*/,
                Response* r) {
  std::string b;
  b += std::format("port={}\n", s->config.port);
  b += std::format("workers={}\n", s->config.num_workers);
  b += std::format("sqpoll={}\n",
                   s->config.sqpoll ? "true" : "false");
  b += std::format("tls_cert={}\n", s->config.tls_cert);
  b += std::format("hd_enabled={}\n",
                   s->hd_enabled ? "true" : "false");
  if (s->hd_enabled) {
    std::lock_guard lock(s->hd_peers.mutex);
    b += std::format("hd.relay_id={}\n",
                     s->hd_peers.relay_id);
    b += std::format(
        "hd.enroll_mode={}\n",
        s->hd_peers.enroll_mode ==
                HdEnrollMode::kAutoApprove
            ? "auto"
            : "manual");
    b += std::format(
        "hd.relay_policy.forbid_direct={}\n",
        s->hd_peers.relay_policy.forbid_direct ? "true"
                                                : "false");
    b += std::format(
        "hd.relay_policy.forbid_relayed={}\n",
        s->hd_peers.relay_policy.forbid_relayed ? "true"
                                                 : "false");
    b += std::format(
        "hd.relay_policy.max_direct_peers={}\n",
        s->hd_peers.relay_policy.max_direct_peers);
    b += std::format(
        "hd.federation.fleet_id={}\n",
        s->hd_peers.federation_policy.local_fleet_id);
  }
  SetBody(r, b);
}

void ShowFleet(Server* s, const Request& /*req*/,
               Response* r) {
  std::string b;
  b += std::format("self_relay_id={}\n",
                   s->relay_table.self_id);
  {
    std::lock_guard lock(s->relay_table.mutex);
    int idx = 0;
    for (int i = 0; i < kMaxRelays; i++) {
      const auto& e = s->relay_table.entries[i];
      if (e.occupied != 1) continue;
      b += std::format("relay.{}.id={}\n", idx,
                       e.relay_id);
      b += std::format("relay.{}.hops={}\n", idx, e.hops);
      b += std::format("relay.{}.fd={}\n", idx, e.fd);
      idx++;
    }
    b += std::format("relay.count={}\n", idx);
  }
  if (s->fleet_controller_started) {
    b += std::format(
        "fleet_controller.version={}\n",
        s->fleet_controller.applied_version.load());
    b += std::format(
        "fleet_controller.last_status={}\n",
        static_cast<int>(
            s->fleet_controller.last_status.load()));
  } else {
    b += "fleet_controller=disabled\n";
  }
  SetBody(r, b);
}

// -- Peer lifecycle --------------------------------------

void PeerApprove(Server* s, const Request& req,
                 Response* r) {
  Key k{};
  if (!ResolveKey(req, 0, &k, r)) return;
  int fd = -1;
  uint16_t pid = 0;
  {
    std::lock_guard lock(s->hd_peers.mutex);
    if (!HdPeersApprove(&s->hd_peers, k.data())) {
      r->status = ResponseStatus::kError;
      r->error = ErrorOf("not_found", "peer not found");
      return;
    }
    auto* p = HdPeersLookup(&s->hd_peers, k.data());
    if (p) {
      fd = p->fd;
      pid = p->peer_id;
    }
  }
  if (fd >= 0) {
    HdSendApproved(fd, k);
    DpAddPeer(&s->data_plane, fd, k, PeerProtocol::kHd,
              pid);
  }
  SetBody(r, std::format("status=approved\nkey={}\n",
                          KeyToCkString(k)));
}

void PeerDeny(Server* s, const Request& req,
              Response* r) {
  Key k{};
  if (!ResolveKey(req, 0, &k, r)) return;
  int fd = -1;
  {
    std::lock_guard lock(s->hd_peers.mutex);
    auto* p = HdPeersLookup(&s->hd_peers, k.data());
    if (!p) {
      r->status = ResponseStatus::kError;
      r->error = ErrorOf("not_found", "peer not found");
      return;
    }
    fd = p->fd;
    HdPeersDeny(&s->hd_peers, k.data());
  }
  if (fd >= 0) {
    HdSendDenied(fd, 0, "denied by admin");
    ::close(fd);
  }
  SetBody(r, std::format("status=denied\nkey={}\n",
                          KeyToCkString(k)));
}

void PeerRevoke(Server* s, const Request& req,
                Response* r) {
  Key k{};
  if (!ResolveKey(req, 0, &k, r)) return;
  int fd = -1;
  {
    std::lock_guard lock(s->hd_peers.mutex);
    auto* p = HdPeersLookup(&s->hd_peers, k.data());
    if (!p) {
      r->status = ResponseStatus::kError;
      r->error = ErrorOf("not_found", "peer not found");
      return;
    }
    fd = p->fd;
    HdPeersRevoke(&s->hd_peers, k.data());
  }
  if (fd >= 0) {
    DpRemovePeer(&s->data_plane, k);
    ::close(fd);
  }
  SetBody(r, std::format("status=revoked\nkey={}\n",
                          KeyToCkString(k)));
}

void PeerRedirect(Server* s, const Request& req,
                  Response* r) {
  Key k{};
  if (!ResolveKey(req, 0, &k, r)) return;
  if (req.args.size() < 2) {
    r->status = ResponseStatus::kError;
    r->error =
        ErrorOf("missing_arg", "missing target_url");
    return;
  }
  const std::string& url = req.args[1];
  HdRedirectReason reason = HdRedirectReason::kRebalancing;
  auto fit = req.flags.find("reason");
  if (fit != req.flags.end()) {
    try {
      reason = static_cast<HdRedirectReason>(
          std::stoi(fit->second));
    } catch (...) {
    }
  }
  int fd = -1;
  {
    std::lock_guard lock(s->hd_peers.mutex);
    auto* p = HdPeersLookup(&s->hd_peers, k.data());
    if (!p) {
      r->status = ResponseStatus::kError;
      r->error = ErrorOf("not_found", "peer not found");
      return;
    }
    fd = p->fd;
  }
  if (fd < 0) {
    r->status = ResponseStatus::kError;
    r->error =
        ErrorOf("no_socket", "peer has no active socket");
    return;
  }
  auto rv = HdSendRedirect(fd, reason, url);
  if (!rv) {
    r->status = ResponseStatus::kError;
    r->error = ErrorOf("send_failed", rv.error().message);
    return;
  }
  SetBody(r, std::format("status=redirect_sent\nkey={}\n"
                          "target={}\n",
                          KeyToCkString(k), url));
}

// -- Peer policy ----------------------------------------

HdIntent IntentFromString(const std::string& s,
                          HdIntent fallback) {
  if (s == "prefer_direct") return HdIntent::kPreferDirect;
  if (s == "require_direct")
    return HdIntent::kRequireDirect;
  if (s == "prefer_relay") return HdIntent::kPreferRelay;
  if (s == "require_relay")
    return HdIntent::kRequireRelay;
  return fallback;
}

void PeerPolicySet(Server* s, const Request& req,
                   Response* r) {
  Key k{};
  if (!ResolveKey(req, 0, &k, r)) return;
  HdPeerPolicy pol;
  {
    std::lock_guard lock(s->hd_peers.mutex);
    auto* existing =
        HdPeersLookupPolicy(&s->hd_peers, k.data());
    if (existing) pol = *existing;
  }
  auto it_intent = req.flags.find("intent");
  if (it_intent != req.flags.end()) {
    pol.pinned_intent = IntentFromString(
        it_intent->second, pol.pinned_intent);
    pol.has_pin = true;
  }
  auto it_override = req.flags.find("override");
  if (it_override != req.flags.end()) {
    pol.override_client =
        it_override->second == "true" ||
        it_override->second == "1";
  }
  auto it_tag = req.flags.find("audit-tag");
  if (it_tag != req.flags.end()) {
    pol.audit_tag = it_tag->second;
  }
  auto it_reason = req.flags.find("reason");
  if (it_reason != req.flags.end()) {
    pol.reason = it_reason->second;
  }
  if (!HdPeersSetPolicy(&s->hd_peers, k.data(), pol)) {
    r->status = ResponseStatus::kError;
    r->error = ErrorOf("not_found", "peer not found");
    return;
  }
  SetBody(r, std::format("status=policy_set\nkey={}\n",
                          KeyToCkString(k)));
}

void PeerPolicyClear(Server* s, const Request& req,
                     Response* r) {
  Key k{};
  if (!ResolveKey(req, 0, &k, r)) return;
  if (!HdPeersClearPolicy(&s->hd_peers, k.data())) {
    r->status = ResponseStatus::kError;
    r->error = ErrorOf("not_found", "peer not found");
    return;
  }
  SetBody(r, std::format("status=policy_cleared\nkey={}\n",
                          KeyToCkString(k)));
}

void PeerRuleAdd(Server* s, const Request& req,
                 Response* r) {
  Key src{};
  if (!ResolveKey(req, 0, &src, r)) return;
  Key dst{};
  if (req.args.size() < 2) {
    r->status = ResponseStatus::kError;
    r->error = ErrorOf("missing_arg",
                        "missing destination key");
    return;
  }
  if (ParseKeyString(req.args[1], &dst) ==
      KeyPrefix::kInvalid) {
    r->status = ResponseStatus::kError;
    r->error = ErrorOf("invalid_key", "bad dst key");
    return;
  }
  {
    std::lock_guard lock(s->hd_peers.mutex);
    if (!HdPeersAddRule(&s->hd_peers, src.data(), dst)) {
      r->status = ResponseStatus::kError;
      r->error = ErrorOf(
          "rule_not_added",
          "peer not found or rule limit reached");
      return;
    }
  }
  DpAddFwdRule(&s->data_plane, src, dst);
  SetBody(r, std::format("status=rule_added\nsrc={}\n"
                          "dst={}\n",
                          KeyToCkString(src),
                          KeyToCkString(dst)));
}

// -- Relay-scoped --------------------------------------

void RelayInit(Server* /*s*/, const Request& /*req*/,
               Response* r) {
  Key k{};
  randombytes_buf(k.data(), kKeySize);
  std::string hex(kKeySize * 2, '\0');
  sodium_bin2hex(hex.data(), hex.size() + 1, k.data(),
                 kKeySize);
  SetBody(r, std::format(
                 "relay_key={}\nrelay_key_str={}\nnote="
                 "one-shot; set hd.relay_key in "
                 "config and redistribute\n",
                 hex, KeyToRkString(k)));
}

// -- Dispatch ------------------------------------------

using Registry = std::unordered_map<std::string, Handler>;

Registry MakeRegistry() {
  Registry m;
  m["show_status"] = ShowStatus;
  m["show_peers"] = ShowPeers;
  m["show_peer"] = ShowPeer;
  m["show_audit"] = ShowAudit;
  m["show_counters"] = ShowCounters;
  m["show_config"] = ShowConfig;
  m["show_fleet"] = ShowFleet;
  m["peer_approve"] = PeerApprove;
  m["peer_deny"] = PeerDeny;
  m["peer_revoke"] = PeerRevoke;
  m["peer_redirect"] = PeerRedirect;
  m["peer_policy_set"] = PeerPolicySet;
  m["peer_policy_clear"] = PeerPolicyClear;
  m["peer_rule_add"] = PeerRuleAdd;
  m["relay_init"] = RelayInit;
  return m;
}

}  // namespace

struct EinheitChannel {
  zmq::context_t zmq_ctx{1};
  std::thread thread;
  std::atomic<bool> running{true};
  Server* server = nullptr;
  Registry registry;
  std::string pub_endpoint;
};

namespace {

void RunLoop(EinheitChannel* ch,
             const std::string& ctl_endpoint) {
  try {
    zmq::socket_t router(ch->zmq_ctx,
                         zmq::socket_type::router);
    router.set(zmq::sockopt::linger, 0);
    router.set(zmq::sockopt::rcvtimeo, 200);
    router.bind(ctl_endpoint);
    spdlog::info("einheit channel listening on {}",
                 ctl_endpoint);

    while (ch->running.load(std::memory_order_acquire)) {
      zmq::message_t identity;
      zmq::message_t delimiter;
      zmq::message_t payload;
      auto res =
          router.recv(identity, zmq::recv_flags::none);
      if (!res) continue;
      res = router.recv(delimiter, zmq::recv_flags::none);
      if (!res) continue;
      res = router.recv(payload, zmq::recv_flags::none);
      if (!res) continue;

      std::span<const std::uint8_t> view(
          static_cast<const std::uint8_t*>(payload.data()),
          payload.size());
      auto decoded = einheit::DecodeRequest(view);
      Response response;
      if (!decoded) {
        response.status = ResponseStatus::kError;
        response.error = ErrorOf(
            "bad_request",
            "request decode failed: " +
                decoded.error().message);
      } else {
        response.id = decoded->id;
        auto it =
            ch->registry.find(decoded->command);
        if (it == ch->registry.end()) {
          response.status = ResponseStatus::kError;
          response.error = ErrorOf(
              "unknown_command",
              "no handler for: " + decoded->command);
        } else {
          try {
            it->second(ch->server, *decoded, &response);
          } catch (const std::exception& e) {
            response.status = ResponseStatus::kError;
            response.error =
                ErrorOf("handler_exception", e.what());
          }
        }
      }
      auto encoded = einheit::EncodeResponse(response);
      if (!encoded) {
        spdlog::error(
            "einheit encode response failed: {}",
            encoded.error().message);
        continue;
      }
      router.send(identity, zmq::send_flags::sndmore);
      router.send(zmq::message_t(),
                  zmq::send_flags::sndmore);
      router.send(zmq::buffer(*encoded),
                  zmq::send_flags::none);
    }
  } catch (const zmq::error_t& e) {
    spdlog::error("einheit channel: {}", e.what());
  }
}

}  // namespace

EinheitChannel* EinheitChannelStart(
    const std::string& ctl_endpoint,
    const std::string& pub_endpoint, Server* server) {
  auto* ch = new EinheitChannel();
  ch->server = server;
  ch->registry = MakeRegistry();
  ch->pub_endpoint = pub_endpoint;
  try {
    ch->thread = std::thread(RunLoop, ch, ctl_endpoint);
  } catch (const std::exception& e) {
    spdlog::error("einheit channel start: {}", e.what());
    delete ch;
    return nullptr;
  }
  return ch;
}

void EinheitChannelStop(EinheitChannel* ch) {
  if (!ch) return;
  ch->running.store(false, std::memory_order_release);
  if (ch->thread.joinable()) ch->thread.join();
  delete ch;
}

}  // namespace hyper_derp
