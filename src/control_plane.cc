/// @file control_plane.cc
/// @brief DERP control plane implementation.
///
/// Handles peer registry, watcher notifications, and
/// control frame dispatch. The data plane forwards
/// non-transport frames via per-worker pipes in the format:
///   [4B fd BE][1B type][4B payload_len BE][payload...]
///
/// The control thread multiplexes all worker pipes via
/// epoll and dispatches frames to handlers.

#include "hyper_derp/control_plane.h"

#include <spdlog/spdlog.h>

#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "hyper_derp/data_plane.h"
#include "hyper_derp/hd_protocol.h"
#include "hyper_derp/stun.h"

namespace hyper_derp {

// -- Hash helpers (FNV-1a, same as data plane) ------

static uint32_t CpFnv1a(const uint8_t* data, int len) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < len; i++) {
    h ^= data[i];
    h *= 16777619u;
  }
  return h;
}

// -- Peer registry (open-addressing, linear probe) --
//
// All registry functions are called with the mutex held
// (or from the control thread via DoConnect/DoDisconnect).

static CpPeer* CpRegistryLookup(ControlPlane* cp,
                                 const uint8_t* key) {
  uint32_t idx = CpFnv1a(key, kKeySize) &
                 (kCpMaxPeers - 1);
  for (int i = 0; i < kCpMaxPeers; i++) {
    uint32_t slot = (idx + i) & (kCpMaxPeers - 1);
    CpPeer* p = &cp->peers[slot];
    if (p->occupied == 1 &&
        std::memcmp(p->key.data(), key,
                    kKeySize) == 0) {
      return p;
    }
    if (p->occupied == 0) {
      return nullptr;
    }
  }
  return nullptr;
}

static CpPeer* CpRegistryInsert(ControlPlane* cp,
                                 const Key& key,
                                 int fd) {
  uint32_t idx = CpFnv1a(key.data(), kKeySize) &
                 (kCpMaxPeers - 1);
  for (int i = 0; i < kCpMaxPeers; i++) {
    uint32_t slot = (idx + i) & (kCpMaxPeers - 1);
    CpPeer* p = &cp->peers[slot];
    if (p->occupied == 1 && p->key == key) {
      // Peer reconnected; update fd.
      if (p->fd >= 0 && p->fd < kMaxFd) {
        cp->fd_map[p->fd] = nullptr;
      }
      p->fd = fd;
      if (fd >= 0 && fd < kMaxFd) {
        cp->fd_map[fd] = p;
      }
      return p;
    }
    if (p->occupied != 1) {
      p->key = key;
      p->fd = fd;
      p->occupied = 1;
      p->preferred = 0;
      if (fd >= 0 && fd < kMaxFd) {
        cp->fd_map[fd] = p;
      }
      cp->peer_count++;
      return p;
    }
  }
  return nullptr;
}

static void CpRegistryRemove(ControlPlane* cp,
                              const uint8_t* key) {
  uint32_t idx = CpFnv1a(key, kKeySize) &
                 (kCpMaxPeers - 1);
  for (int i = 0; i < kCpMaxPeers; i++) {
    uint32_t slot = (idx + i) & (kCpMaxPeers - 1);
    CpPeer* p = &cp->peers[slot];
    if (p->occupied == 1 &&
        std::memcmp(p->key.data(), key,
                    kKeySize) == 0) {
      if (p->fd >= 0 && p->fd < kMaxFd) {
        cp->fd_map[p->fd] = nullptr;
      }
      p->occupied = 2;  // tombstone
      p->fd = -1;
      cp->peer_count--;
      return;
    }
    if (p->occupied == 0) {
      return;
    }
  }
}

// -- Watcher management -----------------------------

static void CpAddWatcher(ControlPlane* cp, int fd) {
  for (int i = 0; i < cp->watcher_count; i++) {
    if (cp->watcher_fds[i] == fd) {
      return;
    }
  }
  if (cp->watcher_count >= kCpMaxWatchers) {
    spdlog::warn("watcher limit reached, ignoring fd {}",
                 fd);
    return;
  }
  cp->watcher_fds[cp->watcher_count++] = fd;
}

static void CpRemoveWatcher(ControlPlane* cp, int fd) {
  for (int i = 0; i < cp->watcher_count; i++) {
    if (cp->watcher_fds[i] == fd) {
      cp->watcher_fds[i] =
          cp->watcher_fds[--cp->watcher_count];
      return;
    }
  }
}

// -- Send frame to a peer via the data plane --------

static void CpSendFrame(ControlPlane* cp,
                         const Key& key,
                         const uint8_t* frame,
                         int frame_len) {
  // DpWrite takes ownership of a malloc'd buffer.
  auto* buf = static_cast<uint8_t*>(malloc(frame_len));
  if (!buf) {
    return;
  }
  std::memcpy(buf, frame, frame_len);
  DpWrite(cp->data_plane, key, buf, frame_len);
}

// -- Notify all watchers ----------------------------

static void CpNotifyWatchers(ControlPlane* cp,
                             const uint8_t* frame,
                             int frame_len) {
  for (int i = 0; i < cp->watcher_count; i++) {
    int wfd = cp->watcher_fds[i];
    if (wfd < 0 || wfd >= kMaxFd) {
      continue;
    }
    CpPeer* wp = cp->fd_map[wfd];
    if (!wp || wp->occupied != 1) {
      continue;
    }
    CpSendFrame(cp, wp->key, frame, frame_len);
  }
}

// -- Internal state mutations (called with mutex
//    held) ----------------------------------------

/// Register a peer and notify watchers. Must be called
/// while the mutex is held.
static void DoConnect(ControlPlane* cp,
                      const Key& key, int fd) {
  CpRegistryInsert(cp, key, fd);

  uint8_t frame[kFrameHeaderSize + kKeySize];
  int n = BuildPeerPresent(frame, key);
  CpNotifyWatchers(cp, frame, n);
}

/// Remove a peer, clean up watchers, and notify. Must
/// be called while the mutex is held.
static void DoDisconnect(ControlPlane* cp,
                         const Key& key, int fd,
                         PeerGoneReason reason) {
  CpRemoveWatcher(cp, fd);
  CpRegistryRemove(cp, key.data());

  uint8_t frame[kFrameHeaderSize + kKeySize + 1];
  int n = BuildPeerGone(frame, key, reason);
  CpNotifyWatchers(cp, frame, n);
}

// -- Control frame handlers -------------------------

static void HandleWatchConns(ControlPlane* cp, int fd) {
  CpAddWatcher(cp, fd);

  // Send PeerPresent for all currently connected peers
  // (initial sync).
  uint8_t frame[kFrameHeaderSize + kKeySize];
  for (int i = 0; i < kCpMaxPeers; i++) {
    CpPeer* p = &cp->peers[i];
    if (p->occupied != 1) {
      continue;
    }
    if (p->fd == fd) {
      continue;
    }
    int n = BuildPeerPresent(frame, p->key);
    if (fd >= 0 && fd < kMaxFd && cp->fd_map[fd]) {
      CpSendFrame(cp, cp->fd_map[fd]->key, frame, n);
    }
  }

  spdlog::debug("fd {} now watching connections", fd);
}

static void HandlePing(ControlPlane* cp, int fd,
                       const uint8_t* payload,
                       int payload_len) {
  if (payload_len < kPingDataSize) {
    return;
  }
  uint8_t frame[kFrameHeaderSize + kPingDataSize];
  int n = BuildPong(frame, payload);

  if (fd >= 0 && fd < kMaxFd && cp->fd_map[fd]) {
    CpSendFrame(cp, cp->fd_map[fd]->key, frame, n);
  }
}

static void HandleNotePreferred(ControlPlane* cp, int fd,
                                const uint8_t* payload,
                                int payload_len) {
  if (payload_len < 1) {
    return;
  }
  if (fd >= 0 && fd < kMaxFd && cp->fd_map[fd]) {
    cp->fd_map[fd]->preferred = payload[0];
    spdlog::debug("fd {} preferred={}", fd, payload[0]);
  }
}

static void HandleClosePeer(ControlPlane* cp, int fd,
                            const uint8_t* payload,
                            int payload_len) {
  if (payload_len < kKeySize) {
    return;
  }
  CpPeer* target = CpRegistryLookup(cp, payload);
  if (!target) {
    return;
  }
  spdlog::info("fd {} requested close of peer", fd);
  Key target_key = ToKey(payload);
  DpRemovePeer(cp->data_plane, target_key);
  // Call internal DoDisconnect (mutex already held).
  DoDisconnect(cp, target_key, target->fd,
               PeerGoneReason::kDisconnected);
}

// -- Level 2 ICE helpers --------------------------------

/// epoll data tags for non-pipe sources.
/// Worker pipes use data.u32 = [0, kMaxWorkers).
/// Level 2 and fleet routing sources use higher values.
static constexpr uint32_t kEpollTagIceTimer =
    kMaxWorkers;
static constexpr uint32_t kEpollTagStunUdp =
    kMaxWorkers + 1;
static constexpr uint32_t kEpollTagRouteAnnounce =
    kMaxWorkers + 2;
static constexpr uint32_t kEpollTagOpenConnTimer =
    kMaxWorkers + 3;

static void ScanOpenConnTimeouts(ControlPlane* cp);

/// Process STUN responses received on the UDP socket.
static void HandleStunResponse(ControlPlane* cp) {
  uint8_t buf[1500];
  sockaddr_in from{};
  socklen_t from_len = sizeof(from);
  int n = recvfrom(cp->stun_udp_fd, buf, sizeof(buf),
                   MSG_DONTWAIT,
                   reinterpret_cast<sockaddr*>(&from),
                   &from_len);
  if (n <= 0) return;

  StunMessage msg;
  if (!StunParse(buf, n, &msg)) return;
  if (msg.type != kStunBindingResponse) return;
  if (!msg.has_xor_mapped) return;

  // Find the ICE session matching this transaction ID.
  std::lock_guard lock(cp->ice_agent->mutex);
  for (int i = 0; i < kIceMaxSessions; ++i) {
    IceSession* s = &cp->ice_agent->sessions[i];
    if (!s->occupied) continue;
    if (s->state != IceState::kChecking) continue;
    bool nominated = IceProcessResponse(
        s, msg.transaction_id,
        msg.xor_mapped_ip, msg.xor_mapped_port);
    if (nominated) {
      spdlog::info(
          "ICE: Level 2 nominated for session {}", i);
      break;
    }
  }
}

/// Run one round of ICE connectivity checks across
/// all active sessions.
static void HandleIceTimer(ControlPlane* cp) {
  // Drain the timerfd.
  uint64_t expirations = 0;
  (void)read(cp->ice_timer_fd, &expirations,
             sizeof(expirations));

  if (!cp->ice_agent) return;

  std::lock_guard lock(cp->ice_agent->mutex);
  for (int i = 0; i < kIceMaxSessions; ++i) {
    IceSession* s = &cp->ice_agent->sessions[i];
    if (!s->occupied) continue;
    if (s->state != IceState::kChecking) continue;

    IceCandidatePair* pair = IceNextCheck(s);
    if (!pair) continue;

    // Build and send STUN Binding Request.
    uint8_t stun_buf[256];
    int stun_len = StunBuildBindingRequest(
        stun_buf, sizeof(stun_buf),
        pair->transaction_id);
    if (stun_len <= 0) continue;

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = pair->remote.ip;
    dst.sin_port = pair->remote.port;
    sendto(cp->stun_udp_fd, stun_buf, stun_len,
           MSG_DONTWAIT,
           reinterpret_cast<const sockaddr*>(&dst),
           sizeof(dst));
  }
}

// -- Fleet routing helpers --------------------------

/// Build and send route announcements to all direct
/// neighbor relays (hops == 1).
static void HandleRouteAnnounce(ControlPlane* cp) {
  uint64_t expirations = 0;
  (void)read(cp->route_announce_fd, &expirations,
             sizeof(expirations));

  if (!cp->relay_table) return;

  // Build announcement from the relay table.
  uint16_t ids[kMaxRelays];
  uint8_t hops[kMaxRelays];
  int count = RelayTableList(cp->relay_table,
                             ids, hops, kMaxRelays);

  // Always include ourselves at hop 0.
  bool has_self = false;
  for (int i = 0; i < count; i++) {
    if (ids[i] == cp->relay_table->self_id) {
      has_self = true;
      break;
    }
  }
  if (!has_self && count < kMaxRelays) {
    ids[count] = cp->relay_table->self_id;
    hops[count] = 0;
    count++;
  }

  uint8_t buf[kHdFrameHeaderSize +
              kMaxRelays * kHdRouteEntrySize];
  int frame_len = HdBuildRouteAnnounce(
      buf, static_cast<int>(sizeof(buf)),
      ids, hops, count);
  if (frame_len <= 0) return;

  // Send to all direct neighbor relays.
  std::lock_guard lock(cp->relay_table->mutex);
  for (int i = 0; i < kMaxRelays; i++) {
    auto& e = cp->relay_table->entries[i];
    if (e.occupied != 1 || e.hops != 1) continue;
    CpSendFrame(cp, e.key, buf, frame_len);
  }
}

// -- Public API -------------------------------------

void CpInit(ControlPlane* cp, Ctx* dp) {
  cp->data_plane = dp;
  cp->epoll_fd = -1;
  cp->peer_count = 0;
  cp->watcher_count = 0;
}

void CpDestroy(ControlPlane* cp) {
  HdAuditFlusherStop(&cp->audit_flusher);
  if (cp->open_conn_timer_fd >= 0) {
    close(cp->open_conn_timer_fd);
    cp->open_conn_timer_fd = -1;
  }
  if (cp->route_announce_fd >= 0) {
    close(cp->route_announce_fd);
    cp->route_announce_fd = -1;
  }
  if (cp->ice_timer_fd >= 0) {
    close(cp->ice_timer_fd);
    cp->ice_timer_fd = -1;
  }
  if (cp->stun_udp_fd >= 0) {
    close(cp->stun_udp_fd);
    cp->stun_udp_fd = -1;
  }
  if (cp->epoll_fd >= 0) {
    close(cp->epoll_fd);
    cp->epoll_fd = -1;
  }
}

void CpOnPeerConnect(ControlPlane* cp,
                     const Key& key, int fd) {
  std::lock_guard lock(cp->mutex);
  DoConnect(cp, key, fd);
}

void CpOnPeerGone(ControlPlane* cp,
                  const Key& key, int fd,
                  PeerGoneReason reason) {
  std::lock_guard lock(cp->mutex);
  DoDisconnect(cp, key, fd, reason);
}

void CpProcessFrame(ControlPlane* cp, int fd,
                    FrameType type,
                    const uint8_t* payload,
                    int payload_len) {
  std::lock_guard lock(cp->mutex);

  // HD frames arrive with their type byte from the data
  // plane pipe (not a FrameType enum member).
  if (static_cast<uint8_t>(type) ==
      static_cast<uint8_t>(HdFrameType::kPeerInfo)) {
    CpHandleHdPeerInfo(cp, fd, payload, payload_len);
    return;
  }
  if (static_cast<uint8_t>(type) ==
      static_cast<uint8_t>(HdFrameType::kRouteAnnounce)) {
    CpHandleRouteAnnounce(cp, fd, payload, payload_len);
    return;
  }
  if (static_cast<uint8_t>(type) ==
      static_cast<uint8_t>(HdFrameType::kOpenConnection)) {
    CpHandleOpenConnection(cp, fd, payload, payload_len);
    return;
  }
  if (static_cast<uint8_t>(type) ==
      static_cast<uint8_t>(
          HdFrameType::kIncomingConnResponse)) {
    CpHandleIncomingConnResponse(
        cp, fd, payload, payload_len);
    return;
  }
  if (static_cast<uint8_t>(type) ==
      static_cast<uint8_t>(
          HdFrameType::kFleetOpenConnection)) {
    CpHandleFleetOpenConnection(cp, fd, payload,
                                payload_len);
    return;
  }
  if (static_cast<uint8_t>(type) ==
      static_cast<uint8_t>(
          HdFrameType::kFleetOpenConnectionResult)) {
    CpHandleFleetOpenConnectionResult(cp, fd, payload,
                                      payload_len);
    return;
  }

  switch (type) {
    case FrameType::kWatchConns:
      HandleWatchConns(cp, fd);
      break;
    case FrameType::kPing:
      HandlePing(cp, fd, payload, payload_len);
      break;
    case FrameType::kNotePreferred:
      HandleNotePreferred(cp, fd, payload, payload_len);
      break;
    case FrameType::kClosePeer:
      HandleClosePeer(cp, fd, payload, payload_len);
      break;
    case FrameType::kPeerGone:
      if (payload_len >= kKeySize) {
        Key gone_key = ToKey(payload);
        CpRegistryRemove(cp, payload);
        // Call internal DoDisconnect (mutex already
        // held).
        DoDisconnect(cp, gone_key, fd,
                     PeerGoneReason::kDisconnected);
        DpRemovePeer(cp->data_plane, gone_key);
      }
      break;
    case FrameType::kKeepAlive:
      break;
    default:
      spdlog::debug(
          "unhandled control frame type 0x{:02x}"
          " from fd {}",
          static_cast<int>(type), fd);
      break;
  }
}

void CpRunLoop(ControlPlane* cp) {
  int nworkers = cp->data_plane->num_workers;

  // Set up epoll to watch all worker control pipes.
  cp->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (cp->epoll_fd < 0) {
    spdlog::error("epoll_create1: {}", strerror(errno));
    return;
  }

  for (int i = 0; i < nworkers; i++) {
    cp->readers[i].len = 0;
    int pfd = cp->data_plane->pipe_rds[i];
    if (pfd < 0) {
      continue;
    }
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.u32 = static_cast<uint32_t>(i);
    if (epoll_ctl(cp->epoll_fd, EPOLL_CTL_ADD, pfd,
                  &ev) < 0) {
      spdlog::error("epoll_ctl ADD pipe {}: {}", i,
                    strerror(errno));
    }
  }

  // Register Level 2 fds with epoll if enabled.
  if (cp->level2_enabled) {
    if (cp->ice_timer_fd >= 0) {
      epoll_event tev{};
      tev.events = EPOLLIN;
      tev.data.u32 = kEpollTagIceTimer;
      if (epoll_ctl(cp->epoll_fd, EPOLL_CTL_ADD,
                    cp->ice_timer_fd, &tev) < 0) {
        spdlog::error("epoll_ctl ADD ice_timer: {}",
                      strerror(errno));
      }
    }
    if (cp->stun_udp_fd >= 0) {
      epoll_event sev{};
      sev.events = EPOLLIN;
      sev.data.u32 = kEpollTagStunUdp;
      if (epoll_ctl(cp->epoll_fd, EPOLL_CTL_ADD,
                    cp->stun_udp_fd, &sev) < 0) {
        spdlog::error("epoll_ctl ADD stun_udp: {}",
                      strerror(errno));
      }
    }
  }

  // Register fleet routing timer if enabled.
  if (cp->route_announce_fd >= 0) {
    epoll_event rav{};
    rav.events = EPOLLIN;
    rav.data.u32 = kEpollTagRouteAnnounce;
    if (epoll_ctl(cp->epoll_fd, EPOLL_CTL_ADD,
                  cp->route_announce_fd, &rav) < 0) {
      spdlog::error("epoll_ctl ADD route_announce: {}",
                    strerror(errno));
    }
  }

  // Register routing-policy deadline timer.
  if (cp->open_conn_timer_fd >= 0) {
    epoll_event oct{};
    oct.events = EPOLLIN;
    oct.data.u32 = kEpollTagOpenConnTimer;
    if (epoll_ctl(cp->epoll_fd, EPOLL_CTL_ADD,
                  cp->open_conn_timer_fd, &oct) < 0) {
      spdlog::error("epoll_ctl ADD open_conn_timer: {}",
                    strerror(errno));
    }
  }

  cp->running.store(1, std::memory_order_release);
  spdlog::info("control plane running ({} pipes)",
               nworkers);

  // Extra slots for ICE + STUN + route + open-conn timer.
  constexpr int kMaxEpollEvents = kMaxWorkers + 4;
  epoll_event events[kMaxEpollEvents];

  while (cp->running.load(std::memory_order_acquire)) {
    int nev = epoll_wait(cp->epoll_fd, events,
                         kMaxEpollEvents, 500);
    if (nev < 0) {
      if (errno == EINTR) {
        continue;
      }
      spdlog::error("epoll_wait: {}", strerror(errno));
      break;
    }

    for (int e = 0; e < nev; e++) {
      uint32_t tag = events[e].data.u32;

      // Handle Level 2 epoll sources.
      if (tag == kEpollTagIceTimer) {
        HandleIceTimer(cp);
        continue;
      }
      if (tag == kEpollTagStunUdp) {
        HandleStunResponse(cp);
        continue;
      }
      if (tag == kEpollTagRouteAnnounce) {
        HandleRouteAnnounce(cp);
        continue;
      }
      if (tag == kEpollTagOpenConnTimer) {
        uint64_t ticks;
        (void)read(cp->open_conn_timer_fd,
                   &ticks, sizeof(ticks));
        ScanOpenConnTimeouts(cp);
        continue;
      }

      int wid = static_cast<int>(tag);
      int pfd = cp->data_plane->pipe_rds[wid];
      PipeReader* rdr = &cp->readers[wid];

      int avail = kPipeBufSize - rdr->len;
      if (avail <= 0) {
        spdlog::warn(
            "pipe {} buffer overflow, resetting", wid);
        rdr->len = 0;
        continue;
      }

      int n = read(pfd, rdr->buf + rdr->len, avail);
      if (n <= 0) {
        if (n == 0 || errno == EAGAIN) {
          continue;
        }
        if (errno != EINTR) {
          spdlog::debug("pipe {} read: {}", wid,
                        strerror(errno));
        }
        continue;
      }
      rdr->len += n;

      // Parse complete messages from the buffer.
      // Format: [4B fd BE][1B type][4B len BE][payload]
      while (rdr->len >= kPipeMsgHeader) {
        int src_fd =
            (static_cast<int>(rdr->buf[0]) << 24) |
            (static_cast<int>(rdr->buf[1]) << 16) |
            (static_cast<int>(rdr->buf[2]) << 8) |
            static_cast<int>(rdr->buf[3]);
        auto ftype =
            static_cast<FrameType>(rdr->buf[4]);
        int plen =
            (static_cast<int>(rdr->buf[5]) << 24) |
            (static_cast<int>(rdr->buf[6]) << 16) |
            (static_cast<int>(rdr->buf[7]) << 8) |
            static_cast<int>(rdr->buf[8]);

        int msg_len = kPipeMsgHeader + plen;
        if (rdr->len < msg_len) {
          break;
        }

        CpProcessFrame(cp, src_fd, ftype,
                        rdr->buf + kPipeMsgHeader,
                        plen);

        int remaining = rdr->len - msg_len;
        if (remaining > 0) {
          memmove(rdr->buf, rdr->buf + msg_len,
                  remaining);
        }
        rdr->len = remaining;
      }
    }
  }

  spdlog::info("control plane stopped");
}

void CpStop(ControlPlane* cp) {
  cp->running.store(0, std::memory_order_release);
}

int CpEnableLevel2(ControlPlane* cp,
                   IceAgent* agent,
                   uint16_t stun_port) {
  cp->ice_agent = agent;
  cp->level2_enabled = true;

  // Create timerfd for periodic ICE checks (50ms).
  cp->ice_timer_fd = timerfd_create(
      CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (cp->ice_timer_fd < 0) {
    spdlog::error("timerfd_create: {}",
                  strerror(errno));
    return -1;
  }
  itimerspec its{};
  its.it_interval = {.tv_sec = 0,
                     .tv_nsec = 50000000};  // 50ms
  its.it_value = {.tv_sec = 0,
                  .tv_nsec = 50000000};
  if (timerfd_settime(cp->ice_timer_fd, 0, &its,
                      nullptr) < 0) {
    spdlog::error("timerfd_settime: {}",
                  strerror(errno));
    close(cp->ice_timer_fd);
    cp->ice_timer_fd = -1;
    return -1;
  }

  // Create UDP socket for STUN binding requests and
  // responses.
  cp->stun_udp_fd = socket(AF_INET, SOCK_DGRAM |
                           SOCK_NONBLOCK | SOCK_CLOEXEC,
                           0);
  if (cp->stun_udp_fd < 0) {
    spdlog::error("STUN UDP socket: {}",
                  strerror(errno));
    close(cp->ice_timer_fd);
    cp->ice_timer_fd = -1;
    return -1;
  }
  sockaddr_in bind_addr{};
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_addr.s_addr = INADDR_ANY;
  bind_addr.sin_port = htons(stun_port);
  int opt = 1;
  setsockopt(cp->stun_udp_fd, SOL_SOCKET,
             SO_REUSEADDR, &opt, sizeof(opt));
  if (bind(cp->stun_udp_fd,
           reinterpret_cast<sockaddr*>(&bind_addr),
           sizeof(bind_addr)) < 0) {
    spdlog::error("STUN UDP bind port {}: {}",
                  stun_port, strerror(errno));
    close(cp->stun_udp_fd);
    cp->stun_udp_fd = -1;
    close(cp->ice_timer_fd);
    cp->ice_timer_fd = -1;
    return -1;
  }

  spdlog::info("Level 2 control plane enabled "
               "(STUN port {})", stun_port);
  return 0;
}

void CpEnableFleetRouting(ControlPlane* cp,
                          RelayTable* rt) {
  cp->relay_table = rt;

  // Create 30-second periodic timerfd.
  cp->route_announce_fd = timerfd_create(
      CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (cp->route_announce_fd < 0) {
    spdlog::error("route announce timerfd_create: {}",
                  strerror(errno));
    return;
  }
  itimerspec its{};
  its.it_interval = {.tv_sec = 30, .tv_nsec = 0};
  // First announcement after 5 seconds.
  its.it_value = {.tv_sec = 5, .tv_nsec = 0};
  if (timerfd_settime(cp->route_announce_fd, 0, &its,
                      nullptr) < 0) {
    spdlog::error("route announce timerfd_settime: {}",
                  strerror(errno));
    close(cp->route_announce_fd);
    cp->route_announce_fd = -1;
    return;
  }

  spdlog::info("fleet routing enabled (30s announce)");
}

void CpHandleRouteAnnounce(ControlPlane* cp, int fd,
                           const uint8_t* payload,
                           int payload_len) {
  if (!cp->relay_table) return;

  uint16_t ids[256];
  uint8_t hops[256];
  int count = HdParseRouteAnnounce(
      payload, payload_len, ids, hops, 256);

  // Find which relay sent this announcement by fd.
  uint16_t src_relay = 0;
  {
    std::lock_guard lock(cp->relay_table->mutex);
    for (int i = 0; i < kMaxRelays; i++) {
      auto& e = cp->relay_table->entries[i];
      if (e.occupied == 1 && e.fd == fd) {
        src_relay = e.relay_id;
        break;
      }
    }
  }
  if (src_relay == 0) {
    spdlog::debug("RouteAnnounce from unknown fd {}",
                  fd);
    return;
  }

  // Update routes: for each announced relay, if
  // hops+1 is better than our current route, update.
  for (int i = 0; i < count; i++) {
    if (ids[i] == cp->relay_table->self_id) continue;
    uint8_t new_hops =
        static_cast<uint8_t>(hops[i] + 1);
    if (new_hops >= 255) continue;  // Poison.
    RelayTableUpdateRoute(cp->relay_table, ids[i],
                          new_hops, src_relay, fd);
  }

  spdlog::debug(
      "processed RouteAnnounce from relay {} ({} entries)",
      src_relay, count);
}

void CpHandleHdPeerInfo(ControlPlane* cp, int fd,
                        const uint8_t* payload,
                        int payload_len) {
  if (!cp->level2_enabled || !cp->ice_agent) {
    spdlog::debug("PeerInfo from fd {} ignored "
                  "(Level 2 disabled)", fd);
    return;
  }
  // PeerInfo payload: [32B peer_key][candidate_data...]
  if (payload_len < kKeySize) {
    spdlog::debug("PeerInfo from fd {} too short", fd);
    return;
  }
  const uint8_t* peer_key = payload;
  const uint8_t* cand_data = payload + kKeySize;
  int cand_len = payload_len - kKeySize;

  std::lock_guard lock(cp->ice_agent->mutex);

  IceSession* session =
      IceFindSession(cp->ice_agent, peer_key);
  if (!session) {
    spdlog::debug("PeerInfo: no ICE session for peer");
    return;
  }

  int parsed = IceParseCandidates(session,
                                  cand_data, cand_len);
  if (parsed < 0) {
    spdlog::warn("PeerInfo: failed to parse candidates");
    return;
  }
  spdlog::debug("PeerInfo: parsed {} remote candidates",
                parsed);

  // If we have both local and remote candidates, form
  // pairs and start checking.
  if (session->local_count > 0 &&
      session->remote_count > 0 &&
      session->state == IceState::kGathering) {
    IceFormPairs(session);
    spdlog::info("ICE: formed {} pairs, checking",
                 session->pair_count);
  }
}

// -- Routing policy (Phase 2) -------------------------

namespace {

uint64_t NowNsMono() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

// Blocking write of exactly n bytes; best-effort, no
// retry on partial failures beyond EINTR/EAGAIN.
int WriteFullFrame(int fd, const uint8_t* buf, int n) {
  int total = 0;
  while (total < n) {
    int w = ::write(fd, buf + total, n - total);
    if (w < 0) {
      if (errno == EINTR || errno == EAGAIN) continue;
      return -1;
    }
    if (w == 0) return -1;
    total += w;
  }
  return 0;
}

int OpenConnSlotFind(ControlPlane* cp,
                     uint64_t correlation_id,
                     int initiator_fd) {
  for (int i = 0; i < kCpMaxOpenConns; i++) {
    auto& s = cp->open_conns[i];
    if (s.in_use && s.correlation_id == correlation_id &&
        s.initiator_fd == initiator_fd) {
      return i;
    }
  }
  return -1;
}

int OpenConnSlotAlloc(ControlPlane* cp) {
  for (int i = 0; i < kCpMaxOpenConns; i++) {
    if (!cp->open_conns[i].in_use) return i;
  }
  return -1;
}

void OpenConnSlotFree(ControlPlane* cp, int idx) {
  auto& s = cp->open_conns[idx];
  if (s.in_use && s.initiator_fd >= 0 &&
      s.initiator_fd < kMaxFd) {
    cp->open_conn_per_peer[s.initiator_fd]--;
  }
  s = ControlPlane::OpenConnEntry{};
}

void SendOpenResultAndCleanup(
    ControlPlane* cp, int slot_idx,
    HdConnMode mode, HdDenyReason reason,
    uint8_t sub_reason) {
  auto& s = cp->open_conns[slot_idx];
  uint8_t buf[256];
  // Gateway-side: wrap as FleetOpenConnectionResult back
  // to the origin relay link.
  uint16_t path[1];
  int path_len = 0;
  if (s.target_relay_id != 0 &&
      !s.is_gateway_inbound) {
    path[0] = s.target_relay_id;
    path_len = 1;
  }
  int n = 0;
  if (s.is_gateway_inbound) {
    n = HdBuildFleetOpenConnectionResult(
        buf, sizeof(buf), s.correlation_id, mode, reason,
        sub_reason, nullptr, 0, nullptr, 0);
  } else {
    n = HdBuildOpenConnectionResult(
        buf, sizeof(buf), s.correlation_id, mode, reason,
        sub_reason,
        path_len > 0 ? path : nullptr, path_len,
        nullptr, 0);
  }
  if (n > 0 && s.initiator_fd >= 0) {
    WriteFullFrame(s.initiator_fd, buf, n);
  }
  if (mode != HdConnMode::kDenied && s.target_fd >= 0 &&
      s.target_relay_id == 0 &&
      !s.is_gateway_inbound) {
    // Mirror result to the local target if one was
    // contacted. Skip for cross-relay forwards (the
    // remote gateway handles its own target notification)
    // and for gateway-inbound slots (the target is local
    // and was mirrored when we built the Fleet result).
    uint8_t tbuf[kHdFrameHeaderSize +
                 kHdIncomingResultSize];
    int tn = HdBuildIncomingConnResult(
        tbuf, s.correlation_id, mode, reason, sub_reason);
    WriteFullFrame(s.target_fd, tbuf, tn);
  }
  // Decrement direct cap if we claimed it.
  if (cp->hd_peers &&
      mode == HdConnMode::kDirect) {
    // no-op here; Phase 5 doesn't yet release direct
    // counter on tunnel close. Placeholder.
  }
  OpenConnSlotFree(cp, slot_idx);
}

}  // namespace

void CpEnableAuditFile(ControlPlane* cp,
                       const std::string& path,
                       uint64_t max_bytes, int keep) {
  HdAuditFlusherStart(&cp->audit_flusher,
                      &cp->audit_ring, path, max_bytes,
                      keep);
}

void CpEnableRoutingPolicy(ControlPlane* cp,
                           HdPeerRegistry* hd_peers) {
  cp->hd_peers = hd_peers;
  HdAuditInit(&cp->audit_ring);
  cp->open_conn_timer_fd =
      timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
  if (cp->open_conn_timer_fd < 0) {
    spdlog::error("open_conn timerfd: {}",
                  strerror(errno));
    return;
  }
  itimerspec its{};
  its.it_interval.tv_sec = 1;
  its.it_value.tv_sec = 1;
  if (timerfd_settime(cp->open_conn_timer_fd, 0, &its,
                      nullptr) < 0) {
    spdlog::error("open_conn timer_settime: {}",
                  strerror(errno));
  }
}

static void ScanOpenConnTimeouts(ControlPlane* cp) {
  uint64_t now = NowNsMono();
  std::lock_guard lock(cp->mutex);
  for (int i = 0; i < kCpMaxOpenConns; i++) {
    auto& s = cp->open_conns[i];
    if (!s.in_use) continue;
    if (now < s.deadline_ns) continue;
    SendOpenResultAndCleanup(
        cp, i, HdConnMode::kDenied,
        HdDenyReason::kTargetUnresponsive, 0);
  }
}

void CpHandleOpenConnection(ControlPlane* cp, int fd,
                            const uint8_t* payload,
                            int payload_len) {
  HdOpenConnection req;
  if (!HdParseOpenConnection(payload, payload_len,
                             &req)) {
    spdlog::debug("OpenConnection parse failed from fd {}",
                  fd);
    return;
  }
  spdlog::debug(
      "OpenConnection from fd {}: target_peer_id={} "
      "target_relay_id={} intent={} corr={:x}",
      fd, req.target_peer_id, req.target_relay_id,
      static_cast<int>(req.intent), req.correlation_id);
  if (!cp->hd_peers) return;

  // Per-peer outstanding-cap guard.
  if (fd >= 0 && fd < kMaxFd &&
      cp->open_conn_per_peer[fd] >=
          kCpMaxOpenConnsPerPeer) {
    uint8_t rbuf[64];
    int rn = HdBuildOpenConnectionResult(
        rbuf, sizeof(rbuf), req.correlation_id,
        HdConnMode::kDenied,
        HdDenyReason::kTooManyOpenConns, 0, nullptr, 0,
        nullptr, 0);
    if (rn > 0) WriteFullFrame(fd, rbuf, rn);
    return;
  }

  // Look up initiator key by fd via cp->fd_map.
  Key initiator_key{};
  CpPeer* initiator =
      (fd >= 0 && fd < kMaxFd) ? cp->fd_map[fd] : nullptr;
  if (initiator) initiator_key = initiator->key;

  // Build the client view from the wire request.
  HdClientView cv;
  cv.intent = req.intent;
  cv.allow_upgrade =
      (req.flags & kHdFlagAllowUpgrade) != 0;
  cv.allow_downgrade =
      (req.flags & kHdFlagAllowDowngrade) != 0;

  // Apply the initiator's peer policy to the client view
  // before passing down. If override_client=true, the
  // peer's pinned intent replaces whatever the client
  // asked for — enforced here so the audit chain records
  // the effective intent.
  {
    std::lock_guard plk(cp->hd_peers->mutex);
    auto* ipol = HdPeersLookupPolicy(
        cp->hd_peers, initiator_key.data());
    if (ipol && ipol->override_client && ipol->has_pin) {
      cv.intent = ipol->pinned_intent;
    }
  }

  // Phase 2 capability: direct path (Level 2 / WG) is
  // not wired into the HD data plane yet, so advertise
  // can_direct=false. prefer_direct falls back to
  // relayed; require_direct correctly denies with
  // kNatIncompatible until Phase 3/5 add real probing.
  HdCapability cap{.can_direct = false};

  // Fleet + relay views sourced from the registry.
  HdLayerView fleet_view;
  HdLayerView relay_view;
  {
    std::lock_guard plk(cp->hd_peers->mutex);
    fleet_view =
        HdBuildFleetView(cp->hd_peers->fleet_policy);
    relay_view =
        HdBuildRelayView(cp->hd_peers->relay_policy);
  }

  // Cross-relay path: forward as FleetOpenConnection to
  // the gateway over the relay-mesh link.
  if (req.target_relay_id != 0) {
    if (!cp->relay_table) {
      uint8_t rbuf[64];
      int rn = HdBuildOpenConnectionResult(
          rbuf, sizeof(rbuf), req.correlation_id,
          HdConnMode::kDenied,
          HdDenyReason::kFleetRoutingNotImplemented, 0,
          nullptr, 0, nullptr, 0);
      if (rn > 0) WriteFullFrame(fd, rbuf, rn);
      return;
    }
    int gateway_fd = -1;
    {
      std::lock_guard rtl(cp->relay_table->mutex);
      auto* e = RelayTableLookup(cp->relay_table,
                                  req.target_relay_id);
      if (e) gateway_fd = e->fd;
    }
    if (gateway_fd < 0) {
      uint8_t rbuf[64];
      int rn = HdBuildOpenConnectionResult(
          rbuf, sizeof(rbuf), req.correlation_id,
          HdConnMode::kDenied,
          HdDenyReason::kPeerUnreachable, 0, nullptr,
          0, nullptr, 0);
      if (rn > 0) WriteFullFrame(fd, rbuf, rn);
      return;
    }

    // Allocate a slot that tracks the outbound forward.
    int slot = OpenConnSlotAlloc(cp);
    if (slot < 0) {
      uint8_t rbuf[64];
      int rn = HdBuildOpenConnectionResult(
          rbuf, sizeof(rbuf), req.correlation_id,
          HdConnMode::kDenied,
          HdDenyReason::kTooManyOpenConns, 0, nullptr,
          0, nullptr, 0);
      if (rn > 0) WriteFullFrame(fd, rbuf, rn);
      return;
    }
    auto& s = cp->open_conns[slot];
    s.in_use = 1;
    s.correlation_id = req.correlation_id;
    s.initiator_fd = fd;
    s.target_fd = gateway_fd;
    s.initiator_key = initiator_key;
    s.initiator_view = cv;
    s.target_relay_id = req.target_relay_id;
    s.is_gateway_inbound = false;
    s.deadline_ns = NowNsMono() + kCpOpenConnTimeoutNs;
    if (fd >= 0 && fd < kMaxFd) {
      cp->open_conn_per_peer[fd]++;
    }

    // Build the FleetOpenConnection envelope: rebuild the
    // inner OpenConnection payload with the effective
    // intent (after A's peer-policy override was applied
    // above) and ship to the gateway.
    uint8_t inner_frame[kHdFrameHeaderSize +
                         kHdOpenConnSize];
    uint8_t inner_flags = 0;
    if (cv.allow_upgrade)
      inner_flags |= kHdFlagAllowUpgrade;
    if (cv.allow_downgrade)
      inner_flags |= kHdFlagAllowDowngrade;
    HdBuildOpenConnection(inner_frame,
                          req.target_peer_id, 0,
                          cv.intent, inner_flags,
                          req.correlation_id);

    Key zero_key{};
    const std::string& fid =
        cp->hd_peers->federation_policy
            .local_fleet_id;
    uint8_t out_buf[256];
    int on = HdBuildFleetOpenConnection(
        out_buf, sizeof(out_buf),
        cp->relay_table->self_id, fid.c_str(),
        static_cast<int>(fid.size()), zero_key,
        inner_frame + kHdFrameHeaderSize,
        kHdOpenConnSize);
    if (on <= 0 ||
        WriteFullFrame(gateway_fd, out_buf, on) < 0) {
      SendOpenResultAndCleanup(
          cp, slot, HdConnMode::kDenied,
          HdDenyReason::kPeerUnreachable, 0);
    }
    return;
  }

  // Local target: find by peer_id.
  HdPeer* target = nullptr;
  int target_fd = -1;
  {
    std::lock_guard lock(cp->hd_peers->mutex);
    target = HdPeersLookupById(cp->hd_peers,
                                req.target_peer_id);
    if (target) target_fd = target->fd;
  }
  if (!target || target_fd < 0) {
    HdDecision dec;
    dec.mode = HdConnMode::kDenied;
    dec.deny_reason = HdDenyReason::kPeerUnreachable;
    uint8_t rbuf[64];
    int rn = HdBuildOpenConnectionResult(
        rbuf, sizeof(rbuf), req.correlation_id,
        dec.mode, dec.deny_reason, 0, nullptr, 0,
        nullptr, 0);
    if (rn > 0) WriteFullFrame(fd, rbuf, rn);
    Key tkey{};
    HdAuditRecordDecision(&cp->audit_ring, initiator_key,
                          tkey, cv, dec);
    return;
  }

  // Allocate correlation slot.
  int slot = OpenConnSlotAlloc(cp);
  if (slot < 0) {
    uint8_t rbuf[64];
    int rn = HdBuildOpenConnectionResult(
        rbuf, sizeof(rbuf), req.correlation_id,
        HdConnMode::kDenied,
        HdDenyReason::kTooManyOpenConns, 0, nullptr, 0,
        nullptr, 0);
    if (rn > 0) WriteFullFrame(fd, rbuf, rn);
    return;
  }
  auto& s = cp->open_conns[slot];
  s.in_use = 1;
  s.correlation_id = req.correlation_id;
  s.initiator_fd = fd;
  s.target_fd = target_fd;
  s.initiator_key = initiator_key;
  s.target_key = target->key;
  s.initiator_view = cv;
  s.target_relay_id = req.target_relay_id;
  s.deadline_ns = NowNsMono() + kCpOpenConnTimeoutNs;
  if (fd >= 0 && fd < kMaxFd) {
    cp->open_conn_per_peer[fd]++;
  }

  // Forward IncomingConnection to the target.
  uint8_t buf[kHdFrameHeaderSize + kHdIncomingConnSize];
  int n = HdBuildIncomingConnection(
      buf, initiator_key,
      initiator ? target->peer_id : 0, cv.intent,
      req.flags, req.correlation_id);
  if (WriteFullFrame(target_fd, buf, n) < 0) {
    SendOpenResultAndCleanup(
        cp, slot, HdConnMode::kDenied,
        HdDenyReason::kPeerUnreachable, 0);
  }
}

void CpHandleIncomingConnResponse(
    ControlPlane* cp, int fd,
    const uint8_t* payload, int payload_len) {
  HdIncomingConnResponse resp;
  if (!HdParseIncomingConnResponse(payload, payload_len,
                                   &resp)) {
    return;
  }
  // Find slot by correlation_id + target_fd.
  int slot = -1;
  for (int i = 0; i < kCpMaxOpenConns; i++) {
    auto& s = cp->open_conns[i];
    if (s.in_use &&
        s.correlation_id == resp.correlation_id &&
        s.target_fd == fd) {
      slot = i;
      break;
    }
  }
  if (slot < 0) return;

  auto& entry = cp->open_conns[slot];

  HdClientView b_cv;
  b_cv.intent = resp.intent;
  b_cv.allow_upgrade =
      (resp.flags & kHdFlagAllowUpgrade) != 0;
  b_cv.allow_downgrade =
      (resp.flags & kHdFlagAllowDowngrade) != 0;

  if (!(resp.accept & kHdFlagAccept)) {
    SendOpenResultAndCleanup(
        cp, slot, HdConnMode::kDenied,
        HdDenyReason::kPolicyForbids, 0);
    return;
  }

  // Apply B's server-side peer policy to its wire intent.
  HdPeerPolicy b_policy{};
  {
    std::lock_guard plk(cp->hd_peers->mutex);
    auto* bpol = HdPeersLookupPolicy(
        cp->hd_peers, entry.target_key.data());
    if (bpol) b_policy = *bpol;
  }
  HdIntent effective_b =
      HdApplyPeerPolicyIntent(b_policy, resp.intent);
  HdLayerView peer_view =
      HdBuildPeerView(b_policy, effective_b);

  HdCapability cap{.can_direct = false};
  HdLayerView fleet_view;
  HdLayerView relay_view;
  int max_direct = 0;
  {
    std::lock_guard plk(cp->hd_peers->mutex);
    fleet_view =
        HdBuildFleetView(cp->hd_peers->fleet_policy);
    relay_view =
        HdBuildRelayView(cp->hd_peers->relay_policy);
    max_direct =
        cp->hd_peers->relay_policy.max_direct_peers;
  }
  HdDecision dec = HdResolve(
      fleet_view, HdLayerView{}, relay_view,
      peer_view, entry.initiator_view, cap,
      entry.target_relay_id);

  // Enforce max_direct_peers as an audit-visible denial
  // rather than silently degrading.
  if (dec.mode == HdConnMode::kDirect && max_direct > 0) {
    int cur = cp->hd_peers->direct_in_use.load(
        std::memory_order_acquire);
    if (cur >= max_direct) {
      dec.mode = HdConnMode::kDenied;
      dec.deny_reason =
          HdDenyReason::kDirectCapExceeded;
    } else {
      cp->hd_peers->direct_in_use.fetch_add(
          1, std::memory_order_acq_rel);
    }
  }

  HdAuditRecordDecision(&cp->audit_ring,
                        entry.initiator_key,
                        entry.target_key,
                        entry.initiator_view, dec);

  SendOpenResultAndCleanup(cp, slot, dec.mode,
                           dec.deny_reason,
                           dec.sub_reason);
}

// -- Cross-relay (gateway + origin-forward) --------

void CpHandleFleetOpenConnection(
    ControlPlane* cp, int fd,
    const uint8_t* payload, int payload_len) {
  HdFleetOpenConnection env;
  if (!HdParseFleetOpenConnection(payload, payload_len,
                                  &env)) {
    return;
  }
  if (!cp->hd_peers) return;

  HdOpenConnection inner;
  if (!HdParseOpenConnection(env.inner_conn,
                             kHdOpenConnSize, &inner)) {
    return;
  }

  HdClientView cv;
  cv.intent = inner.intent;
  cv.allow_upgrade =
      (inner.flags & kHdFlagAllowUpgrade) != 0;
  cv.allow_downgrade =
      (inner.flags & kHdFlagAllowDowngrade) != 0;

  // Look up local target (gateway owns peer policy).
  HdPeer* target = nullptr;
  Key target_key{};
  int target_fd = -1;
  {
    std::lock_guard lock(cp->hd_peers->mutex);
    target = HdPeersLookupById(cp->hd_peers,
                                inner.target_peer_id);
    if (target) {
      target_fd = target->fd;
      target_key = target->key;
    }
  }

  // Helper that sends a denial back as
  // FleetOpenConnectionResult.
  auto deny_back = [&](HdDenyReason r,
                       uint8_t sub = 0) {
    uint8_t rbuf[64];
    int rn = HdBuildFleetOpenConnectionResult(
        rbuf, sizeof(rbuf), inner.correlation_id,
        HdConnMode::kDenied, r, sub, nullptr, 0,
        nullptr, 0);
    if (rn > 0) WriteFullFrame(fd, rbuf, rn);
  };

  if (!target || target_fd < 0) {
    deny_back(HdDenyReason::kPeerUnreachable);
    return;
  }

  // Federation check now that we have the resolved key.
  const char* fed_reason = nullptr;
  if (!HdFederationAllows(
          cp->hd_peers->federation_policy,
          env.origin_fleet_id, target_key,
          &fed_reason)) {
    deny_back(HdDenyReason::kFederationDenied);
    return;
  }

  // Apply initiator peer policy if known (unlikely — the
  // initiator is on a remote relay — but harmless if the
  // gateway happens to have a record).
  {
    std::lock_guard plk(cp->hd_peers->mutex);
    // Initiator key isn't carried in FleetOpenConnection
    // today; skip the override step at the gateway.
    (void)plk;
  }

  // Allocate a correlation slot keyed on the remote's
  // correlation_id. A collision with a local client's id
  // is possible; we disambiguate by storing
  // origin_relay_id in the slot.
  int slot = OpenConnSlotAlloc(cp);
  if (slot < 0) {
    deny_back(HdDenyReason::kTooManyOpenConns);
    return;
  }
  auto& s = cp->open_conns[slot];
  s.in_use = 1;
  s.correlation_id = inner.correlation_id;
  s.initiator_fd = fd;       // relay link back to origin
  s.target_fd = target_fd;   // local target peer
  s.target_key = target_key;
  s.initiator_view = cv;
  s.target_relay_id = 0;
  s.is_gateway_inbound = true;
  s.origin_relay_id = env.origin_relay_id;
  s.deadline_ns = NowNsMono() + kCpOpenConnTimeoutNs;

  // Forward IncomingConnection to the local target. The
  // initiator key is zeroed because it lives on a remote
  // relay and isn't available here.
  Key zero_key{};
  uint8_t buf[kHdFrameHeaderSize + kHdIncomingConnSize];
  int n = HdBuildIncomingConnection(
      buf, zero_key, target->peer_id, cv.intent,
      inner.flags, inner.correlation_id);
  if (WriteFullFrame(target_fd, buf, n) < 0) {
    SendOpenResultAndCleanup(
        cp, slot, HdConnMode::kDenied,
        HdDenyReason::kPeerUnreachable, 0);
  }
}

void CpHandleFleetOpenConnectionResult(
    ControlPlane* cp, int fd,
    const uint8_t* payload, int payload_len) {
  HdOpenConnectionResult result;
  if (!HdParseOpenConnectionResult(payload, payload_len,
                                   &result)) {
    return;
  }
  // Find a matching outbound-forward slot on this relay.
  // Slots are keyed by correlation_id + target_fd (the
  // gateway link the request went out on).
  int slot = -1;
  for (int i = 0; i < kCpMaxOpenConns; i++) {
    auto& s = cp->open_conns[i];
    if (s.in_use &&
        s.correlation_id == result.correlation_id &&
        s.target_fd == fd &&
        !s.is_gateway_inbound &&
        s.target_relay_id != 0) {
      slot = i;
      break;
    }
  }
  if (slot < 0) return;

  SendOpenResultAndCleanup(
      cp, slot, result.mode, result.deny_reason,
      result.sub_reason);
}

}  // namespace hyper_derp
