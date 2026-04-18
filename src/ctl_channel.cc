/// @file ctl_channel.cc
/// @brief ZMQ IPC control channel — ROUTER socket.

#include "hyper_derp/ctl_channel.h"

#include <cstring>
#include <string>
#include <thread>

#include <sodium.h>
#include <zmq.hpp>
#include <spdlog/spdlog.h>

#include "hyper_derp/data_plane.h"
#include "hyper_derp/hd_peers.h"
#include "hyper_derp/protocol.h"

namespace hyper_derp {

struct CtlChannel {
  zmq::context_t zmq_ctx{1};
  std::thread thread;
  std::atomic<bool> running{true};
  Ctx* dp_ctx = nullptr;
  HdPeerRegistry* hd_peers = nullptr;
};

// -- JSON helpers (hand-rolled, no nlohmann dependency) ----------------------

static std::string JsonStr(const char* key,
                           const char* val) {
  return std::string("\"") + key + "\":\"" + val + "\"";
}

static std::string JsonInt(const char* key, uint64_t val) {
  return std::string("\"") + key + "\":" +
         std::to_string(val);
}

static std::string JsonBool(const char* key, bool val) {
  return std::string("\"") + key + "\":" +
         (val ? "true" : "false");
}

// -- Command handlers --------------------------------------------------------

static std::string HandleStatus(CtlChannel* ch) {
  std::string r = "{";
  r += JsonStr("status", "ok") + ",";
  r += JsonInt("workers", ch->dp_ctx->num_workers) + ",";

  uint64_t recv = 0, send = 0;
  int peers = 0;
  for (int i = 0; i < ch->dp_ctx->num_workers; i++) {
    auto* w = ch->dp_ctx->workers[i];
    if (!w) continue;
    recv += __atomic_load_n(&w->stats.recv_bytes,
                            __ATOMIC_RELAXED);
    send += __atomic_load_n(&w->stats.send_bytes,
                            __ATOMIC_RELAXED);
    for (int j = 0; j < kHtCapacity; j++) {
      if (w->ht[j].occupied == 1) peers++;
    }
  }
  r += JsonInt("recv_bytes", recv) + ",";
  r += JsonInt("send_bytes", send) + ",";
  r += JsonInt("peers_active", peers);

  if (ch->hd_peers) {
    r += "," + JsonBool("hd_enabled", true);
    r += "," + JsonInt("hd_peer_count",
                       ch->hd_peers->peer_count);
  }
  r += "}";
  return r;
}

static std::string HandlePeers(CtlChannel* ch) {
  if (!ch->hd_peers) return "{\"peers\":[],\"count\":0}";

  std::lock_guard lock(ch->hd_peers->mutex);
  std::string r = "{\"peers\":[";
  int count = 0;
  for (int i = 0; i < kHdMaxPeers; i++) {
    auto& p = ch->hd_peers->peers[i];
    if (p.occupied != 1) continue;

    if (count > 0) r += ",";
    char hex[65];
    sodium_bin2hex(hex, sizeof(hex),
                   p.key.data(), kKeySize);
    r += "{";
    r += JsonStr("key", hex) + ",";
    r += JsonInt("peer_id", p.peer_id) + ",";
    const char* state_str =
        p.state == HdPeerState::kApproved ? "approved" :
        p.state == HdPeerState::kPending ? "pending" :
        "denied";
    r += JsonStr("state", state_str) + ",";
    r += JsonInt("rules", p.rule_count);
    r += "}";
    count++;
  }
  r += "]," + JsonInt("count", count) + "}";
  return r;
}

static std::string HandleWorkers(CtlChannel* ch) {
  std::string r = "{\"workers\":[";
  for (int i = 0; i < ch->dp_ctx->num_workers; i++) {
    auto* w = ch->dp_ctx->workers[i];
    if (!w) continue;
    if (i > 0) r += ",";
    r += "{";
    r += JsonInt("id", i) + ",";
    r += JsonInt("recv_bytes",
        __atomic_load_n(&w->stats.recv_bytes,
                        __ATOMIC_RELAXED)) + ",";
    r += JsonInt("send_bytes",
        __atomic_load_n(&w->stats.send_bytes,
                        __ATOMIC_RELAXED)) + ",";
    r += JsonInt("send_drops",
        __atomic_load_n(&w->stats.send_drops,
                        __ATOMIC_RELAXED)) + ",";
    r += JsonInt("xfer_drops",
        __atomic_load_n(&w->stats.xfer_drops,
                        __ATOMIC_RELAXED)) + ",";
    r += JsonInt("send_queue_drops",
        __atomic_load_n(&w->stats.send_queue_drops,
                        __ATOMIC_RELAXED)) + ",";
    r += JsonInt("slab_exhausts",
        __atomic_load_n(&w->stats.slab_exhausts,
                        __ATOMIC_RELAXED));
    // Count peers on this worker.
    int peers = 0;
    for (int j = 0; j < kHtCapacity; j++) {
      if (w->ht[j].occupied == 1) peers++;
    }
    r += "," + JsonInt("peers", peers);
    r += "}";
  }
  r += "]}";
  return r;
}

static std::string HandlePeerApprove(CtlChannel* ch,
                                     const std::string& key_hex) {
  if (!ch->hd_peers) return "{\"error\":\"hd disabled\"}";
  Key key{};
  if (key_hex.size() != 64 ||
      sodium_hex2bin(key.data(), kKeySize,
                     key_hex.c_str(), 64,
                     nullptr, nullptr, nullptr) != 0) {
    return "{\"error\":\"invalid key\"}";
  }
  std::lock_guard lock(ch->hd_peers->mutex);
  if (HdPeersApprove(ch->hd_peers, key.data())) {
    return "{\"ok\":true}";
  }
  return "{\"error\":\"peer not found\"}";
}

static std::string HandlePeerDeny(CtlChannel* ch,
                                  const std::string& key_hex) {
  if (!ch->hd_peers) return "{\"error\":\"hd disabled\"}";
  Key key{};
  if (key_hex.size() != 64 ||
      sodium_hex2bin(key.data(), kKeySize,
                     key_hex.c_str(), 64,
                     nullptr, nullptr, nullptr) != 0) {
    return "{\"error\":\"invalid key\"}";
  }
  std::lock_guard lock(ch->hd_peers->mutex);
  if (HdPeersDeny(ch->hd_peers, key.data())) {
    return "{\"ok\":true}";
  }
  return "{\"error\":\"peer not found\"}";
}

static std::string HandlePeerRemove(CtlChannel* ch,
                                    const std::string& key_hex) {
  if (!ch->hd_peers) return "{\"error\":\"hd disabled\"}";
  Key key{};
  if (key_hex.size() != 64 ||
      sodium_hex2bin(key.data(), kKeySize,
                     key_hex.c_str(), 64,
                     nullptr, nullptr, nullptr) != 0) {
    return "{\"error\":\"invalid key\"}";
  }
  std::lock_guard lock(ch->hd_peers->mutex);
  HdPeersRemove(ch->hd_peers, key.data());
  return "{\"ok\":true}";
}

static std::string HandleRulesAdd(CtlChannel* ch,
                                  const std::string& src_hex,
                                  const std::string& dst_hex) {
  if (!ch->hd_peers) return "{\"error\":\"hd disabled\"}";
  Key src{}, dst{};
  if (src_hex.size() != 64 || dst_hex.size() != 64 ||
      sodium_hex2bin(src.data(), kKeySize,
                     src_hex.c_str(), 64,
                     nullptr, nullptr, nullptr) != 0 ||
      sodium_hex2bin(dst.data(), kKeySize,
                     dst_hex.c_str(), 64,
                     nullptr, nullptr, nullptr) != 0) {
    return "{\"error\":\"invalid key\"}";
  }
  {
    std::lock_guard lock(ch->hd_peers->mutex);
    if (!HdPeersAddRule(ch->hd_peers,
                        src.data(), dst)) {
      return "{\"error\":\"rule limit or peer not found\"}";
    }
  }
  DpAddFwdRule(ch->dp_ctx, src, dst);
  return "{\"ok\":true}";
}

// -- Command dispatch --------------------------------------------------------

static std::string ExtractField(const std::string& json,
                                const char* field) {
  std::string key = std::string("\"") + field + "\":\"";
  auto pos = json.find(key);
  if (pos == std::string::npos) return "";
  pos += key.size();
  auto end = json.find('"', pos);
  if (end == std::string::npos) return "";
  return json.substr(pos, end - pos);
}

static std::string Dispatch(CtlChannel* ch,
                            const std::string& req) {
  auto cmd = ExtractField(req, "cmd");
  if (cmd.empty()) return "{\"error\":\"missing cmd\"}";

  if (cmd == "status") return HandleStatus(ch);
  if (cmd == "peers") return HandlePeers(ch);
  if (cmd == "workers") return HandleWorkers(ch);
  if (cmd == "peer_approve") {
    return HandlePeerApprove(ch,
        ExtractField(req, "key"));
  }
  if (cmd == "peer_deny") {
    return HandlePeerDeny(ch,
        ExtractField(req, "key"));
  }
  if (cmd == "peer_remove") {
    return HandlePeerRemove(ch,
        ExtractField(req, "key"));
  }
  if (cmd == "rules_add") {
    return HandleRulesAdd(ch,
        ExtractField(req, "src"),
        ExtractField(req, "dst"));
  }
  return "{\"error\":\"unknown cmd: " + cmd + "\"}";
}

// -- ROUTER thread -----------------------------------------------------------

static void CtlThread(CtlChannel* ch,
                      const char* ipc_path) {
  try {
    zmq::socket_t sock(ch->zmq_ctx,
                       zmq::socket_type::router);
    sock.set(zmq::sockopt::linger, 0);
    sock.set(zmq::sockopt::rcvtimeo, 200);
    sock.bind(ipc_path);
    spdlog::info("ctl channel listening on {}", ipc_path);

    while (ch->running.load()) {
      zmq::message_t identity;
      zmq::message_t delimiter;
      zmq::message_t payload;

      auto res = sock.recv(identity,
                           zmq::recv_flags::none);
      if (!res) continue;

      res = sock.recv(delimiter, zmq::recv_flags::none);
      if (!res) continue;

      res = sock.recv(payload, zmq::recv_flags::none);
      if (!res) continue;

      std::string req(
          static_cast<char*>(payload.data()),
          payload.size());

      std::string resp = Dispatch(ch, req);

      sock.send(identity, zmq::send_flags::sndmore);
      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
      sock.send(zmq::buffer(resp), zmq::send_flags::none);
    }
  } catch (const zmq::error_t& e) {
    spdlog::error("ctl channel: {}", e.what());
  }
}

// -- Public API --------------------------------------------------------------

CtlChannel* CtlChannelStart(const char* ipc_path,
                            Ctx* ctx,
                            HdPeerRegistry* hd_peers) {
  auto* ch = new CtlChannel();
  ch->dp_ctx = ctx;
  ch->hd_peers = hd_peers;
  ch->thread = std::thread(CtlThread, ch, ipc_path);
  return ch;
}

void CtlChannelStop(CtlChannel* ch) {
  if (!ch) return;
  ch->running.store(false);
  if (ch->thread.joinable()) ch->thread.join();
  delete ch;
}

}  // namespace hyper_derp
