/// @file einheit_channel.cc
/// @brief einheit ROUTER/PUB channel + handler registry.
///
/// Wire-command dispatch on the ROUTER; handlers read Server
/// state, format a UTF-8 key=value body, and hand it back in
/// Response::data. The HD adapter's renderers parse that shape
/// today; upgrading individual handlers to MessagePack maps is
/// a follow-up when typed shapes pay off.

#include "hyper_derp/einheit_channel.h"

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

// -- Role + handler registry types ---------------------

// Role required by a handler to pass the server-side
// gate. Mirrors the CLI's RoleGate so an operator can
// deliberately opt in via EINHEIT_ROLE=admin and still
// be rejected if their actual authority is lower.
//
// The gate is advisory today: the caller's role is taken
// from Request::role which the CLI stamps itself. A
// proper SO_PEERCRED-based check on the IPC socket is
// the follow-up; until then the gate mainly prevents
// accidental privilege escalation from a stock CLI,
// not a crafted attacker.
enum class Role {
  kAny,
  kOperator,
  kAdmin,
};

struct ArgInfo {
  const char* name;
  const char* help;
  bool required;
};

struct HandlerEntry {
  Handler fn;
  Role required;
  // Capability metadata. Returned by the `describe`
  // handler so CLI clients can build their command tree
  // at runtime instead of hard-coding an adapter. Empty
  // `path` means "no CLI-visible surface" — useful for
  // internal verbs or ones the framework owns.
  const char* path;
  const char* help;
  bool requires_session;
  std::vector<ArgInfo> args;
};

using Registry =
    std::unordered_map<std::string, HandlerEntry>;

bool RoleOk(const std::string& caller, Role required) {
  if (required == Role::kAny) return true;
  if (caller == "admin") return true;
  if (required == Role::kOperator && caller == "operator") {
    return true;
  }
  return false;
}

const char* RoleNameFor(Role r) {
  switch (r) {
    case Role::kAdmin: return "admin";
    case Role::kOperator: return "operator";
    case Role::kAny: return "any";
  }
  return "any";
}

// -- Key helpers ---------------------------------------

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

// Forward-declared in the header; emits `state.peers.*`
// events from handler call sites.
void EmitPeerEvent(Server* s, const std::string& key_ck,
                   const char* state) {
  if (!s->einheit_channel) return;
  std::string body = std::format(
      "key={}\nstate={}\n", key_ck, state);
  EinheitPublish(s->einheit_channel,
                  std::string("state.peers.") + key_ck,
                  body);
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
  EmitPeerEvent(s, KeyToCkString(k), "approved");
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
  EmitPeerEvent(s, KeyToCkString(k), "denied");
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
  EmitPeerEvent(s, KeyToCkString(k), "revoked");
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

// -- Service control -----------------------------------

// Registry lives on EinheitChannel, whose full struct
// definition follows the handlers in this TU. Accessor
// is defined after that struct.
const Registry& ChannelRegistry(EinheitChannel* ch);

// Fork a detached child that sleeps long enough for the
// ROUTER to flush our reply, then execs `systemctl --user
// <action> hyper-derp`. Returning immediately lets the
// handler serialize a Response; the systemctl call will
// either restart (systemd relaunches) or stop the
// process about 200ms later.
void ScheduleSystemctl(const char* action) {
  pid_t pid = fork();
  if (pid < 0) {
    spdlog::error(
        "daemon_{}: fork failed: {}",
        action, std::strerror(errno));
    return;
  }
  if (pid != 0) return;  // parent — back to handler.

  // Detach from the parent's controlling terminal and fd
  // table; systemctl shouldn't hold onto our logs or
  // ZMQ sockets.
  setsid();
  for (int fd = 0; fd < 3; fd++) close(fd);
  int devnull = ::open("/dev/null", O_RDWR);
  if (devnull >= 0) {
    (void)dup2(devnull, 0);
    (void)dup2(devnull, 1);
    (void)dup2(devnull, 2);
    if (devnull > 2) close(devnull);
  }
  const char* cmd =
      "sleep 0.2 && systemctl --user restart hyper-derp";
  if (std::strcmp(action, "stop") == 0) {
    cmd = "sleep 0.2 && systemctl --user stop hyper-derp";
  }
  execlp("sh", "sh", "-c", cmd, (char*)nullptr);
  _exit(127);  // exec failed
}

void DaemonRestart(Server* /*s*/, const Request& /*req*/,
                    Response* r) {
  SetBody(r, "status=restarting\n");
  ScheduleSystemctl("restart");
}

void DaemonStop(Server* /*s*/, const Request& /*req*/,
                 Response* r) {
  SetBody(r, "status=stopping\n");
  ScheduleSystemctl("stop");
}

// Describe returns the full command catalog so CLI
// clients can build their command tree at runtime
// instead of compiling it in. Text/key=value body to
// stay consistent with every other handler; a typed
// MessagePack shape is a future upgrade if a second
// consumer shows up.
void Describe(Server* s, const Request& /*req*/,
               Response* r) {
  if (!s->einheit_channel) {
    r->status = ResponseStatus::kError;
    r->error = ErrorOf("unavailable",
                       "einheit channel not attached");
    return;
  }
  const auto& reg = ChannelRegistry(s->einheit_channel);
  std::string b;
  b += "v=1\n";
  int idx = 0;
  for (const auto& [wire, e] : reg) {
    if (!e.path || e.path[0] == '\0') continue;
    b += std::format("cmd.{}.path={}\n", idx, e.path);
    b += std::format("cmd.{}.wire={}\n", idx, wire);
    b += std::format("cmd.{}.role={}\n", idx,
                     RoleNameFor(e.required));
    b += std::format("cmd.{}.help={}\n", idx, e.help);
    b += std::format("cmd.{}.requires_session={}\n",
                     idx,
                     e.requires_session ? "true" : "false");
    b += std::format("cmd.{}.arg_count={}\n", idx,
                     e.args.size());
    for (size_t a = 0; a < e.args.size(); ++a) {
      b += std::format("cmd.{}.arg.{}.name={}\n", idx, a,
                       e.args[a].name);
      b += std::format("cmd.{}.arg.{}.help={}\n", idx, a,
                       e.args[a].help);
      b += std::format(
          "cmd.{}.arg.{}.required={}\n", idx, a,
          e.args[a].required ? "true" : "false");
    }
    idx++;
  }
  b += std::format("count={}\n", idx);
  SetBody(r, b);
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

// -- Candidate-config lifecycle ------------------------

// Validators return an empty string on success, otherwise a
// human-readable error message that gets surfaced as
// `validation` / `bad_args` in the Response.

std::string ValidateBool(const std::string& v) {
  if (v == "true" || v == "false") return {};
  return "expected true|false";
}

std::string ValidateIntNonNegative(const std::string& v) {
  try {
    size_t pos = 0;
    int x = std::stoi(v, &pos);
    if (pos != v.size()) return "expected integer";
    if (x < 0) return "expected non-negative integer";
    return {};
  } catch (...) {
    return "expected integer";
  }
}

std::string ValidateUint64(const std::string& v) {
  try {
    size_t pos = 0;
    (void)std::stoull(v, &pos);
    if (pos != v.size()) return "expected integer";
    return {};
  } catch (...) {
    return "expected non-negative integer";
  }
}

std::string ValidateAnyString(const std::string& /*v*/) {
  // Fleet id, audit tags etc. — we accept any byte
  // sequence that survived MessagePack. Further shape
  // checks belong to the handler that consumes it.
  return {};
}

std::string ValidateLogLevel(const std::string& v) {
  if (v == "trace" || v == "debug" || v == "info" ||
      v == "warn" || v == "error" || v == "critical" ||
      v == "off") {
    return {};
  }
  return "expected trace|debug|info|warn|error|critical|off";
}

std::string ValidateEnrollMode(const std::string& v) {
  if (v == "auto" || v == "manual") return {};
  return "expected auto|manual";
}

std::string ValidateIntent(const std::string& v) {
  if (v == "prefer_direct" || v == "require_direct" ||
      v == "prefer_relay" || v == "require_relay") {
    return {};
  }
  return "expected prefer_direct|require_direct|"
         "prefer_relay|require_relay";
}

// Appliers mutate live Server state. Callers that touch
// `s->hd_peers.*` state must hold `s->hd_peers.mutex`; the
// commit handler acquires that lock once around the whole
// batch.

void ApplyLogLevel(Server* /*s*/, const std::string& v) {
  if (v == "trace")
    spdlog::set_level(spdlog::level::trace);
  else if (v == "debug")
    spdlog::set_level(spdlog::level::debug);
  else if (v == "info")
    spdlog::set_level(spdlog::level::info);
  else if (v == "warn")
    spdlog::set_level(spdlog::level::warn);
  else if (v == "error")
    spdlog::set_level(spdlog::level::err);
  else if (v == "critical")
    spdlog::set_level(spdlog::level::critical);
  else if (v == "off")
    spdlog::set_level(spdlog::level::off);
}

void ApplyEnrollMode(Server* s, const std::string& v) {
  s->hd_peers.enroll_mode =
      v == "auto" ? HdEnrollMode::kAutoApprove
                  : HdEnrollMode::kManual;
}

void ApplyForbidDirect(Server* s, const std::string& v) {
  s->hd_peers.relay_policy.forbid_direct = (v == "true");
}

void ApplyForbidRelayed(Server* s, const std::string& v) {
  s->hd_peers.relay_policy.forbid_relayed = (v == "true");
}

void ApplyMaxDirectPeers(Server* s, const std::string& v) {
  s->hd_peers.relay_policy.max_direct_peers = std::stoi(v);
}

void ApplyAuditRelayedTraffic(Server* s,
                               const std::string& v) {
  s->hd_peers.relay_policy.audit_relayed_traffic =
      (v == "true");
}

void ApplyRelayDefaultMode(Server* s,
                            const std::string& v) {
  if (v == "prefer_direct")
    s->hd_peers.relay_policy.default_mode =
        HdIntent::kPreferDirect;
  else if (v == "require_direct")
    s->hd_peers.relay_policy.default_mode =
        HdIntent::kRequireDirect;
  else if (v == "prefer_relay")
    s->hd_peers.relay_policy.default_mode =
        HdIntent::kPreferRelay;
  else if (v == "require_relay")
    s->hd_peers.relay_policy.default_mode =
        HdIntent::kRequireRelay;
}

void ApplyFleetId(Server* s, const std::string& v) {
  s->hd_peers.federation_policy.local_fleet_id = v;
}

void ApplyPeerRateLimit(Server* s, const std::string& v) {
  // Worker threads read this field from Ctx on every
  // frame without holding a lock. Aligned 64-bit writes
  // are atomic on all archs we build for, and relaxed
  // ordering is acceptable — short windows of stale
  // limits can't corrupt state, only briefly mis-count.
  uint64_t x = std::stoull(v);
  __atomic_store_n(&s->data_plane.peer_rate_limit, x,
                   __ATOMIC_RELAXED);
}

struct ConfigPathDef {
  std::string (*validate)(const std::string&);
  void (*apply)(Server*, const std::string&);
  // Does the applier need hd_peers.mutex held?
  bool needs_hd_lock;
  // Value to apply when the path is deleted (either via
  // a `delete` op on commit or as part of a rollback).
  // Kept in sync with the C++ struct defaults so a
  // delete restores real state rather than leaving a
  // stale previously-set value in place.
  const char* default_value;
};

// Allowlist of live-mutable paths. set/delete against
// any path not in this table is rejected at set time
// with a `not_live_mutable` error. Paths not here still
// need a daemon restart + yaml edit to take effect.
const std::unordered_map<std::string, ConfigPathDef>&
ConfigPaths() {
  static const std::unordered_map<std::string,
                                   ConfigPathDef>
      kPaths = {
          {"log_level",
           {ValidateLogLevel, ApplyLogLevel, false,
            "info"}},
          {"hd.enroll_mode",
           {ValidateEnrollMode, ApplyEnrollMode, true,
            "manual"}},
          {"hd.relay_policy.default_mode",
           {ValidateIntent, ApplyRelayDefaultMode, true,
            "prefer_direct"}},
          {"hd.relay_policy.forbid_direct",
           {ValidateBool, ApplyForbidDirect, true,
            "false"}},
          {"hd.relay_policy.forbid_relayed",
           {ValidateBool, ApplyForbidRelayed, true,
            "false"}},
          {"hd.relay_policy.max_direct_peers",
           {ValidateIntNonNegative,
            ApplyMaxDirectPeers, true, "0"}},
          {"hd.relay_policy.audit_relayed_traffic",
           {ValidateBool,
            ApplyAuditRelayedTraffic, true, "true"}},
          {"hd.federation.fleet_id",
           {ValidateAnyString, ApplyFleetId, true, ""}},
          {"peer_rate_limit",
           {ValidateUint64, ApplyPeerRateLimit, false,
            "0"}},
      };
  return kPaths;
}

// Candidate-config session state. Only the ROUTER
// thread touches these fields (ConfigPath handlers
// run one at a time on that thread), so the writes
// need no extra synchronisation.
struct ConfigSession {
  std::optional<std::string> active_id;
  std::unordered_map<std::string, std::string> set_values;
  std::unordered_set<std::string> deleted_paths;
  uint64_t next_id = 1;
  uint64_t commit_count = 0;
};
ConfigSession& Config() {
  static ConfigSession s;
  return s;
}

// -- Persistence ---------------------------------------

// One committed record on disk. Tab-separated fields, one
// record per line:
//   <commit_id>\t<rfc3339_ts>\tS\t<path>\t<value>[\tS..][\tD\t<path>..]\n
// Values in the current allowlist are bool / int / enum
// strings, none of which contain tabs or newlines, so the
// format is reversible without escaping.
struct CommitRecord {
  uint64_t commit_id = 0;
  std::vector<std::pair<std::string, std::string>> sets;
  std::vector<std::string> deletes;
};

std::string EncodeCommitRecord(uint64_t commit_id,
                                const std::string& ts,
                                const CommitRecord& rec) {
  std::string line = std::format("{}\t{}", commit_id, ts);
  for (const auto& [k, v] : rec.sets) {
    line += "\tS\t";
    line += k;
    line += "\t";
    line += v;
  }
  for (const auto& k : rec.deletes) {
    line += "\tD\t";
    line += k;
  }
  line += "\n";
  return line;
}

// Parse one line (no trailing newline). Returns false on
// malformed input; the caller logs and skips.
bool ParseCommitLine(const std::string& line,
                      CommitRecord* out) {
  std::vector<std::string> tok;
  std::string cur;
  for (char c : line) {
    if (c == '\t') {
      tok.push_back(std::move(cur));
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  tok.push_back(std::move(cur));
  if (tok.size() < 2) return false;
  try {
    out->commit_id =
        static_cast<uint64_t>(std::stoull(tok[0]));
  } catch (...) {
    return false;
  }
  // tok[1] is timestamp — stored for observability only.
  for (size_t i = 2; i < tok.size();) {
    if (tok[i] == "S") {
      if (i + 2 >= tok.size()) return false;
      out->sets.emplace_back(tok[i + 1], tok[i + 2]);
      i += 3;
    } else if (tok[i] == "D") {
      if (i + 1 >= tok.size()) return false;
      out->deletes.push_back(tok[i + 1]);
      i += 2;
    } else {
      return false;
    }
  }
  return true;
}

void AppendCommitRecord(const std::string& path,
                         const std::string& line) {
  if (path.empty()) return;
  int fd = ::open(path.c_str(),
                  O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
                  0640);
  if (fd < 0) {
    spdlog::warn(
        "einheit commit log open {} failed: {}",
        path, std::strerror(errno));
    return;
  }
  const char* p = line.data();
  size_t remaining = line.size();
  while (remaining > 0) {
    ssize_t n = ::write(fd, p, remaining);
    if (n < 0) {
      if (errno == EINTR) continue;
      spdlog::warn(
          "einheit commit log write {} failed: {}",
          path, std::strerror(errno));
      break;
    }
    p += n;
    remaining -= n;
  }
  ::fsync(fd);
  ::close(fd);
}

// One on-disk record plus the timestamp field, as loaded
// from the log. Used for replay and for `show commits` /
// `show commit`.
struct StoredCommit {
  CommitRecord rec;
  std::string ts;
};

// Read every complete line in the log. Caller owns the
// returned vector. `bad_count` is incremented for lines
// that fail to parse; partial last lines (no trailing
// newline) are dropped silently. Returns an empty vector
// if the file can't be opened.
std::vector<StoredCommit> LoadCommitLog(
    const std::string& path, int* bad_count) {
  std::vector<StoredCommit> out;
  std::ifstream in(path);
  if (!in.is_open()) return out;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    StoredCommit sc;
    if (!ParseCommitLine(line, &sc.rec)) {
      if (bad_count) (*bad_count)++;
      continue;
    }
    // Recover the timestamp (2nd tab-separated field) for
    // observability. ParseCommitLine already validated
    // structure, so the field always exists here.
    auto first = line.find('\t');
    auto second =
        first == std::string::npos
            ? std::string::npos
            : line.find('\t', first + 1);
    if (first != std::string::npos &&
        second != std::string::npos) {
      sc.ts = line.substr(first + 1, second - first - 1);
    }
    out.push_back(std::move(sc));
  }
  return out;
}

// Replay commits from the log into live state. Called
// once from EinheitChannelStart before the ROUTER loop
// begins accepting requests, so the server comes up with
// the same state it was in before the last restart.
void ReplayCommitLog(Server* s, const std::string& path) {
  int bad_count = 0;
  auto stored = LoadCommitLog(path, &bad_count);
  if (stored.empty() && bad_count == 0) return;

  // Coalesce commits: later writes of the same key win,
  // and a delete clears the key. Then apply each final
  // value once through its applier.
  std::unordered_map<std::string, std::string> final_state;
  uint64_t max_id = 0;
  for (const auto& sc : stored) {
    for (const auto& [k, v] : sc.rec.sets) {
      final_state[k] = v;
    }
    for (const auto& k : sc.rec.deletes) {
      final_state.erase(k);
    }
    if (sc.rec.commit_id > max_id) max_id = sc.rec.commit_id;
  }
  int record_count = static_cast<int>(stored.size());

  const auto& paths = ConfigPaths();
  bool any_hd = false;
  for (const auto& [k, _] : final_state) {
    auto it = paths.find(k);
    if (it != paths.end() && it->second.needs_hd_lock) {
      any_hd = true;
      break;
    }
  }

  auto apply_all = [&]() {
    for (const auto& [k, v] : final_state) {
      auto it = paths.find(k);
      if (it == paths.end()) {
        bad_count++;
        continue;
      }
      auto verr = it->second.validate(v);
      if (!verr.empty()) {
        spdlog::warn(
            "einheit commit log: {}={} failed "
            "validation ({}); skipping",
            k, v, verr);
        bad_count++;
        continue;
      }
      it->second.apply(s, v);
    }
  };
  if (any_hd) {
    std::lock_guard lock(s->hd_peers.mutex);
    apply_all();
  } else {
    apply_all();
  }

  Config().commit_count = max_id;
  spdlog::info(
      "einheit commit log replayed {}: {} records, "
      "{} keys, {} bad",
      path, record_count,
      static_cast<int>(final_state.size()), bad_count);
}

// Defined later in this TU, reused here for commit-log
// timestamps.
std::string NowRfc3339();

// -- Commit-history readers ----------------------------

void ShowCommits(Server* s, const Request& /*req*/,
                  Response* r) {
  const auto& path = s->config.einheit_commit_log_path;
  if (path.empty()) {
    SetBody(r,
            "commit.count=0\nnote=commit log disabled\n");
    return;
  }
  int bad = 0;
  auto stored = LoadCommitLog(path, &bad);
  std::string b;
  for (size_t i = 0; i < stored.size(); ++i) {
    const auto& sc = stored[i];
    b += std::format("commit.{}.id={}\n", i,
                     sc.rec.commit_id);
    b += std::format("commit.{}.ts={}\n", i, sc.ts);
    b += std::format("commit.{}.sets={}\n", i,
                     sc.rec.sets.size());
    b += std::format("commit.{}.deletes={}\n", i,
                     sc.rec.deletes.size());
  }
  b += std::format("commit.count={}\n", stored.size());
  if (bad > 0) {
    b += std::format("commit.bad={}\n", bad);
  }
  SetBody(r, b);
}

void ShowCommit(Server* s, const Request& req,
                 Response* r) {
  if (req.args.empty()) {
    r->status = ResponseStatus::kError;
    r->error = ErrorOf("bad_args",
                       "usage: show commit <id>");
    return;
  }
  const auto& path = s->config.einheit_commit_log_path;
  if (path.empty()) {
    r->status = ResponseStatus::kError;
    r->error = ErrorOf(
        "not_enabled",
        "commit log disabled (set einheit.commit_log_path)");
    return;
  }
  uint64_t target = 0;
  try {
    target =
        static_cast<uint64_t>(std::stoull(req.args[0]));
  } catch (...) {
    r->status = ResponseStatus::kError;
    r->error = ErrorOf("bad_args",
                       "commit id must be an integer");
    return;
  }
  auto stored = LoadCommitLog(path, nullptr);

  // Build the cumulative state up to (but not including)
  // the target commit, so we can diff the target record
  // against its predecessor state.
  std::unordered_map<std::string, std::string> prev_state;
  const StoredCommit* target_rec = nullptr;
  for (const auto& sc : stored) {
    if (sc.rec.commit_id == target) {
      target_rec = &sc;
      break;
    }
    for (const auto& [k, v] : sc.rec.sets) {
      prev_state[k] = v;
    }
    for (const auto& k : sc.rec.deletes) {
      prev_state.erase(k);
    }
  }
  if (target_rec == nullptr) {
    r->status = ResponseStatus::kError;
    r->error = ErrorOf(
        "not_found",
        std::format("no such commit: {}", target));
    return;
  }

  std::string b;
  b += std::format("commit_id={}\n", target_rec->rec.commit_id);
  b += std::format("ts={}\n", target_rec->ts);
  for (const auto& [k, v] : target_rec->rec.sets) {
    auto it = prev_state.find(k);
    if (it == prev_state.end()) {
      b += std::format("+{}={}\n", k, v);
    } else if (it->second != v) {
      b += std::format("~{}={} (was {})\n", k, v,
                       it->second);
    } else {
      b += std::format("={}={}\n", k, v);
    }
  }
  for (const auto& k : target_rec->rec.deletes) {
    auto it = prev_state.find(k);
    if (it != prev_state.end()) {
      b += std::format("-{}={}\n", k, it->second);
    } else {
      b += std::format("-{}=<unset>\n", k);
    }
  }
  SetBody(r, b);
}

bool SessionMatches(const Request& req) {
  auto& cfg = Config();
  if (!cfg.active_id) return false;
  if (!req.session_id) return false;
  return *req.session_id == *cfg.active_id;
}

void Configure(Server* /*s*/, const Request& /*req*/,
                Response* r) {
  auto& cfg = Config();
  if (cfg.active_id) {
    // Opening a new session blows away any orphaned one —
    // single-writer model, last caller wins.
    cfg.set_values.clear();
    cfg.deleted_paths.clear();
  }
  cfg.active_id = std::format("sess_{}", cfg.next_id++);
  SetBody(r, *cfg.active_id);
}

void Set(Server* /*s*/, const Request& req,
          Response* r) {
  auto& cfg = Config();
  if (!cfg.active_id) {
    r->status = ResponseStatus::kError;
    r->error = ErrorOf("no_session",
                       "run `configure` first");
    return;
  }
  if (!SessionMatches(req)) {
    r->status = ResponseStatus::kError;
    r->error = ErrorOf("stale_session",
                       "session id does not match the "
                       "active candidate");
    return;
  }
  if (req.args.size() < 2) {
    r->status = ResponseStatus::kError;
    r->error = ErrorOf(
        "bad_args", "usage: set <path> <value>");
    return;
  }
  // Accept both dotted (`set hd.enroll_mode auto`) and
  // space-separated (`set hd enroll_mode auto`) forms.
  // The last arg is the value; everything before it
  // joined by `.` is the path.
  std::string path = req.args[0];
  for (size_t i = 1; i + 1 < req.args.size(); ++i) {
    path += ".";
    path += req.args[i];
  }
  const std::string& value = req.args.back();
  const auto& paths = ConfigPaths();
  auto it = paths.find(path);
  if (it == paths.end()) {
    r->status = ResponseStatus::kError;
    r->error = ErrorOf(
        "not_live_mutable",
        std::format("path not live-mutable: {}", path),
        "edit the yaml and `daemon restart`");
    return;
  }
  auto err = it->second.validate(value);
  if (!err.empty()) {
    r->status = ResponseStatus::kError;
    r->error = ErrorOf(
        "validation",
        std::format("{}: {}", path, err));
    return;
  }
  cfg.set_values[path] = value;
  cfg.deleted_paths.erase(path);
}

void Delete(Server* /*s*/, const Request& req,
             Response* r) {
  auto& cfg = Config();
  if (!cfg.active_id) {
    r->status = ResponseStatus::kError;
    r->error = ErrorOf("no_session",
                       "run `configure` first");
    return;
  }
  if (!SessionMatches(req)) {
    r->status = ResponseStatus::kError;
    r->error = ErrorOf("stale_session",
                       "session id does not match the "
                       "active candidate");
    return;
  }
  if (req.args.empty()) {
    r->status = ResponseStatus::kError;
    r->error = ErrorOf("bad_args",
                       "usage: delete <path>");
    return;
  }
  // Accept both dotted and space-separated path forms;
  // see Set() for the rationale.
  std::string path = req.args[0];
  for (size_t i = 1; i < req.args.size(); ++i) {
    path += ".";
    path += req.args[i];
  }
  if (!ConfigPaths().contains(path)) {
    r->status = ResponseStatus::kError;
    r->error = ErrorOf(
        "not_live_mutable",
        std::format("path not live-mutable: {}", path),
        "edit the yaml and `daemon restart`");
    return;
  }
  cfg.set_values.erase(path);
  cfg.deleted_paths.insert(path);
}

void Commit(Server* s, const Request& req,
             Response* r) {
  auto& cfg = Config();
  if (!cfg.active_id) {
    r->status = ResponseStatus::kError;
    r->error = ErrorOf(
        "no_session",
        "nothing to commit — run `configure`");
    return;
  }
  if (!SessionMatches(req)) {
    r->status = ResponseStatus::kError;
    r->error = ErrorOf("stale_session",
                       "session id does not match the "
                       "active candidate");
    return;
  }

  bool any_hd = false;
  for (const auto& [p, _] : cfg.set_values) {
    if (ConfigPaths().at(p).needs_hd_lock) {
      any_hd = true;
      break;
    }
  }
  if (!any_hd) {
    for (const auto& p : cfg.deleted_paths) {
      if (ConfigPaths().at(p).needs_hd_lock) {
        any_hd = true;
        break;
      }
    }
  }

  // Lock once for any hd_peers mutation, then apply
  // everything in one pass. A `delete` restores the
  // path's default_value so live state reverts even
  // when it was set by an earlier commit.
  auto apply_all = [&]() {
    for (const auto& [p, v] : cfg.set_values) {
      ConfigPaths().at(p).apply(s, v);
    }
    for (const auto& p : cfg.deleted_paths) {
      const auto& def = ConfigPaths().at(p);
      def.apply(s, def.default_value);
    }
  };
  if (any_hd) {
    std::lock_guard lock(s->hd_peers.mutex);
    apply_all();
  } else {
    apply_all();
  }

  cfg.commit_count += 1;
  uint64_t id = cfg.commit_count;

  // Persist before clearing the session so a crash during
  // log-write leaves the candidate visible (operator can
  // retry). Disabled when commit_log_path is unset.
  if (!s->config.einheit_commit_log_path.empty()) {
    CommitRecord rec;
    rec.commit_id = id;
    for (const auto& [k, v] : cfg.set_values) {
      rec.sets.emplace_back(k, v);
    }
    for (const auto& k : cfg.deleted_paths) {
      rec.deletes.push_back(k);
    }
    AppendCommitRecord(
        s->config.einheit_commit_log_path,
        EncodeCommitRecord(id, NowRfc3339(), rec));
  }

  cfg.active_id.reset();
  cfg.set_values.clear();
  cfg.deleted_paths.clear();
  SetBody(r, std::format("commit_id={}\n", id));

  // Best-effort notification for CLI tails and
  // external controllers. PUB is disabled when the
  // channel has no pub_endpoint configured — in that
  // case this is a no-op.
  if (s->einheit_channel) {
    EinheitPublish(
        s->einheit_channel, "state.config.committed",
        std::format("commit_id={}\n", id));
  }
}

void Rollback(Server* s, const Request& req,
               Response* r) {
  auto& cfg = Config();
  // The CLI routes `rollback candidate` as command
  // "rollback" and `rollback previous` as command
  // "rollback_previous". Default to `candidate` for the
  // plain verb so the older behaviour holds.
  std::string which =
      req.command == "rollback_previous"
          ? std::string("previous")
          : req.args.empty()
              ? std::string("candidate")
              : req.args[0];

  if (which == "candidate") {
    cfg.active_id.reset();
    cfg.set_values.clear();
    cfg.deleted_paths.clear();
    if (s->einheit_channel) {
      EinheitPublish(s->einheit_channel,
                      "state.config.rolled_back",
                      "scope=candidate\n");
    }
    return;
  }

  if (which != "previous") {
    r->status = ResponseStatus::kError;
    r->error = ErrorOf(
        "bad_args",
        "expected `candidate` or `previous`");
    return;
  }

  // `rollback previous` — synthesize a new commit whose
  // ops invert the last record's ops against the state
  // observed before that record. Each inverse op is
  // validated + applied through the same path as a
  // normal commit so the live state and the on-disk log
  // stay in lock-step.
  const auto& path = s->config.einheit_commit_log_path;
  if (path.empty()) {
    r->status = ResponseStatus::kError;
    r->error = ErrorOf(
        "not_enabled",
        "commit log disabled (set "
        "einheit.commit_log_path) — rollback previous "
        "needs history");
    return;
  }
  auto stored = LoadCommitLog(path, nullptr);
  if (stored.empty()) {
    r->status = ResponseStatus::kError;
    r->error = ErrorOf("no_commits",
                       "nothing to roll back");
    return;
  }
  if (cfg.active_id) {
    r->status = ResponseStatus::kError;
    r->error = ErrorOf(
        "session_active",
        "close the candidate session first "
        "(`rollback candidate` or `commit`)");
    return;
  }

  const auto& last = stored.back();
  std::unordered_map<std::string, std::string> prev_state;
  for (size_t i = 0; i + 1 < stored.size(); ++i) {
    for (const auto& [k, v] : stored[i].rec.sets) {
      prev_state[k] = v;
    }
    for (const auto& k : stored[i].rec.deletes) {
      prev_state.erase(k);
    }
  }

  CommitRecord inverse;
  // For each key the last commit SET: restore prior
  // value if there was one, otherwise flag as a delete.
  for (const auto& [k, _] : last.rec.sets) {
    auto it = prev_state.find(k);
    if (it != prev_state.end()) {
      inverse.sets.emplace_back(k, it->second);
    } else {
      inverse.deletes.push_back(k);
    }
  }
  // For each key the last commit DELETED: restore the
  // prior value if we know it; skip when the deletion
  // was a no-op against the prior state.
  for (const auto& k : last.rec.deletes) {
    auto it = prev_state.find(k);
    if (it != prev_state.end()) {
      inverse.sets.emplace_back(k, it->second);
    }
  }

  if (inverse.sets.empty() && inverse.deletes.empty()) {
    r->status = ResponseStatus::kError;
    r->error = ErrorOf(
        "no_op",
        "last commit has no reversible ops");
    return;
  }

  bool any_hd = false;
  const auto& paths = ConfigPaths();
  for (const auto& [k, _] : inverse.sets) {
    auto it = paths.find(k);
    if (it != paths.end() && it->second.needs_hd_lock) {
      any_hd = true;
      break;
    }
  }
  if (!any_hd) {
    for (const auto& k : inverse.deletes) {
      auto it = paths.find(k);
      if (it != paths.end() && it->second.needs_hd_lock) {
        any_hd = true;
        break;
      }
    }
  }

  auto apply_all = [&]() {
    for (const auto& [k, v] : inverse.sets) {
      auto it = paths.find(k);
      if (it == paths.end()) continue;
      it->second.apply(s, v);
    }
    for (const auto& k : inverse.deletes) {
      auto it = paths.find(k);
      if (it == paths.end()) continue;
      it->second.apply(s, it->second.default_value);
    }
  };
  if (any_hd) {
    std::lock_guard lock(s->hd_peers.mutex);
    apply_all();
  } else {
    apply_all();
  }

  cfg.commit_count += 1;
  inverse.commit_id = cfg.commit_count;
  AppendCommitRecord(
      path,
      EncodeCommitRecord(
          inverse.commit_id, NowRfc3339(), inverse));

  SetBody(r, std::format(
                 "commit_id={}\nrolled_back_from={}\n",
                 inverse.commit_id, last.rec.commit_id));
  if (s->einheit_channel) {
    EinheitPublish(
        s->einheit_channel, "state.config.rolled_back",
        std::format("scope=previous\ncommit_id={}\n"
                     "rolled_back_from={}\n",
                     inverse.commit_id, last.rec.commit_id));
  }
}

// -- Dispatch ------------------------------------------

Registry MakeRegistry() {
  Registry m;
  const std::vector<ArgInfo> key_arg = {
      {"key", "ck_/rk_ prefixed or raw-hex peer key",
       true}};
  const std::vector<ArgInfo> set_args = {
      {"path", "schema path (dot or space separated)",
       true},
      {"value", "new value", true}};
  const std::vector<ArgInfo> path_only = {
      {"path", "schema path (dot or space separated)",
       true}};
  const std::vector<ArgInfo> commit_id_arg = {
      {"id", "commit id from `show commits`", true}};

  m["show_status"] = {ShowStatus, Role::kAny,
                      "show status",
                      "Relay + fleet + peer summary",
                      false, {}};
  m["show_peers"] = {ShowPeers, Role::kAny,
                     "show peers", "List HD peers",
                     false, {}};
  m["show_peer"] = {ShowPeer, Role::kAny, "show peer",
                    "One peer's detail + policy + rules",
                    false, key_arg};
  m["show_audit"] = {ShowAudit, Role::kOperator,
                     "show audit",
                     "Recent routing-policy decisions",
                     false, {}};
  m["show_counters"] = {ShowCounters, Role::kOperator,
                         "show counters",
                         "Per-worker + aggregate counters",
                         false, {}};
  m["show_config"] = {ShowConfig, Role::kOperator,
                       "show config",
                       "Redacted runtime configuration",
                       false,
                       {{"prefix",
                         "optional path prefix filter",
                         false}}};
  m["show_fleet"] = {ShowFleet, Role::kOperator,
                     "show fleet",
                     "Fleet controller + relay table "
                     "state",
                     false, {}};
  m["show_commits"] = {ShowCommits, Role::kOperator,
                        "show commits",
                        "List commit history",
                        false, {}};
  m["show_commit"] = {ShowCommit, Role::kOperator,
                       "show commit",
                       "Show a single commit by id",
                       false, commit_id_arg};
  m["peer_approve"] = {PeerApprove, Role::kOperator,
                        "peer approve",
                        "Approve a pending peer",
                        false, key_arg};
  m["peer_deny"] = {PeerDeny, Role::kOperator,
                     "peer deny",
                     "Deny a pending peer",
                     false, key_arg};
  m["peer_revoke"] = {PeerRevoke, Role::kOperator,
                       "peer revoke",
                       "Revoke (denylist + disconnect) "
                       "a peer",
                       false, key_arg};
  m["peer_redirect"] = {PeerRedirect, Role::kOperator,
                         "peer redirect",
                         "Send a Redirect frame to a peer",
                         false,
                         {{"key", "peer key", true},
                          {"target",
                           "destination relay or "
                           "host:port", true}}};
  m["peer_policy_set"] = {PeerPolicySet, Role::kOperator,
                           "peer policy set",
                           "Pin an intent on a peer",
                           false,
                           {{"key", "peer key", true},
                            {"intent",
                             "prefer_direct|require_"
                             "direct|prefer_relay|"
                             "require_relay", true}}};
  m["peer_policy_clear"] = {PeerPolicyClear,
                             Role::kOperator,
                             "peer policy clear",
                             "Remove a peer's intent pin",
                             false, key_arg};
  m["peer_rule_add"] = {PeerRuleAdd, Role::kOperator,
                         "peer rule add",
                         "Add a forwarding rule",
                         false,
                         {{"src", "source peer key", true},
                          {"dst", "destination peer key",
                           true}}};
  m["relay_init"] = {RelayInit, Role::kAdmin,
                     "relay init",
                     "Generate a fresh relay key "
                     "(one-shot)",
                     false, {}};
  m["daemon_restart"] = {DaemonRestart, Role::kAdmin,
                          "daemon restart",
                          "Restart the daemon via "
                          "systemd — session survives "
                          "via DEALER auto-reconnect",
                          false, {}};
  m["daemon_stop"] = {DaemonStop, Role::kAdmin,
                       "daemon stop",
                       "Stop the daemon via systemd — "
                       "the next wire command blocks "
                       "until it starts back up",
                       false, {}};
  m["configure"] = {Configure, Role::kAdmin, "configure",
                    "Enter configure mode and open a "
                    "candidate session",
                    false, {}};
  m["set"] = {Set, Role::kAdmin, "set",
              "Set a candidate-config value at a schema "
              "path",
              true, set_args};
  m["delete"] = {Delete, Role::kAdmin, "delete",
                  "Remove a candidate-config value at a "
                  "schema path",
                  true, path_only};
  m["commit"] = {Commit, Role::kAdmin, "commit",
                 "Apply the candidate configuration",
                 true, {}};
  m["rollback"] = {Rollback, Role::kAdmin,
                    "rollback candidate",
                    "Discard the candidate session",
                    true, {}};
  // Distinct wire verb so the daemon can tell
  // `rollback previous` apart from `rollback
  // candidate` — the CLI shell elides path tokens
  // that aren't bound to arg slots.
  m["rollback_previous"] = {Rollback, Role::kAdmin,
                             "rollback previous",
                             "Roll back to the previous "
                             "commit", false, {}};
  m["describe"] = {Describe, Role::kAny, "",
                    "Return the command catalog for "
                    "CLI discovery",
                    false, {}};
  return m;
}

}  // namespace

struct EinheitChannel {
  zmq::context_t zmq_ctx{1};
  std::thread thread;
  std::thread metrics_thread;
  std::atomic<bool> running{true};
  Server* server = nullptr;
  Registry registry;
  std::string pub_endpoint;
  // PUB socket + guarding mutex. PUB is a single-writer
  // socket; any caller of EinheitPublish serialises on
  // this mutex.
  std::unique_ptr<zmq::socket_t> pub_sock;
  std::mutex pub_mu;
};

namespace {

const Registry& ChannelRegistry(EinheitChannel* ch) {
  return ch->registry;
}

std::string NowRfc3339() {
  auto now = std::chrono::system_clock::now();
  auto secs =
      std::chrono::system_clock::to_time_t(now);
  auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          now.time_since_epoch())
          .count() %
      1000;
  char buf[40];
  struct tm tmv{};
  gmtime_r(&secs, &tmv);
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S",
                &tmv);
  return std::format("{}.{:03d}Z", buf,
                     static_cast<int>(ms));
}

}  // namespace

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
        } else if (!RoleOk(decoded->role,
                            it->second.required)) {
          response.status = ResponseStatus::kError;
          const char* needed =
              it->second.required == Role::kAdmin
                  ? "admin"
              : it->second.required == Role::kOperator
                  ? "operator"
                  : "any";
          response.error = ErrorOf(
              "forbidden",
              std::format(
                  "role '{}' not allowed; need {}",
                  decoded->role.empty() ? "<none>"
                                         : decoded->role,
                  needed));
        } else {
          try {
            it->second.fn(ch->server, *decoded,
                           &response);
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

void MetricsPublishLoop(EinheitChannel* ch) {
  while (ch->running.load(std::memory_order_acquire)) {
    if (ch->server) {
      uint64_t rx = 0, tx = 0;
      int peers = 0;
      for (int i = 0;
           i < ch->server->data_plane.num_workers; i++) {
        auto* w = ch->server->data_plane.workers[i];
        if (!w) continue;
        rx += __atomic_load_n(&w->stats.recv_bytes,
                               __ATOMIC_RELAXED);
        tx += __atomic_load_n(&w->stats.send_bytes,
                               __ATOMIC_RELAXED);
        for (int j = 0; j < kHtCapacity; j++) {
          if (w->ht[j].occupied == 1) peers++;
        }
      }
      EinheitPublish(ch, "state.metrics.recv_bytes",
                      std::to_string(rx));
      EinheitPublish(ch, "state.metrics.send_bytes",
                      std::to_string(tx));
      EinheitPublish(ch, "state.metrics.peers_active",
                      std::to_string(peers));
    }
    for (int i = 0;
         i < 10 && ch->running.load(
                       std::memory_order_acquire);
         i++) {
      usleep(100000);
    }
  }
}

EinheitChannel* EinheitChannelStart(
    const std::string& ctl_endpoint,
    const std::string& pub_endpoint, Server* server) {
  auto* ch = new EinheitChannel();
  ch->server = server;
  ch->registry = MakeRegistry();
  ch->pub_endpoint = pub_endpoint;
  // Replay persisted commits before the ROUTER starts
  // accepting so the daemon comes up with the same live
  // state it had before the restart.
  if (server != nullptr &&
      !server->config.einheit_commit_log_path.empty()) {
    ReplayCommitLog(server,
                     server->config.einheit_commit_log_path);
  }
  if (!pub_endpoint.empty()) {
    try {
      ch->pub_sock = std::make_unique<zmq::socket_t>(
          ch->zmq_ctx, zmq::socket_type::pub);
      ch->pub_sock->set(zmq::sockopt::linger, 0);
      ch->pub_sock->bind(pub_endpoint);
    } catch (const zmq::error_t& e) {
      spdlog::warn("einheit pub bind {} failed: {}",
                   pub_endpoint, e.what());
      ch->pub_sock.reset();
    }
  }
  try {
    ch->thread = std::thread(RunLoop, ch, ctl_endpoint);
    if (ch->pub_sock) {
      ch->metrics_thread =
          std::thread(MetricsPublishLoop, ch);
    }
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
  if (ch->metrics_thread.joinable()) {
    ch->metrics_thread.join();
  }
  delete ch;
}

void EinheitPublish(EinheitChannel* ch,
                    const std::string& topic,
                    const std::string& data) {
  if (!ch || !ch->pub_sock) return;
  einheit::Event ev;
  ev.topic = topic;
  ev.timestamp = NowRfc3339();
  ev.data.assign(data.begin(), data.end());
  auto encoded = einheit::EncodeEventBody(ev);
  if (!encoded) return;
  std::lock_guard lk(ch->pub_mu);
  try {
    zmq::message_t tframe(topic.data(), topic.size());
    ch->pub_sock->send(tframe, zmq::send_flags::sndmore);
    zmq::message_t bframe(encoded->data(),
                          encoded->size());
    ch->pub_sock->send(bframe, zmq::send_flags::none);
  } catch (const zmq::error_t&) {
    // Best-effort publish; nothing to do on transient
    // failure.
  }
}

}  // namespace hyper_derp
