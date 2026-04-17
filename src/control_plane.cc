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
/// Level 2 sources use higher values.
static constexpr uint32_t kEpollTagIceTimer =
    kMaxWorkers;
static constexpr uint32_t kEpollTagStunUdp =
    kMaxWorkers + 1;

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

// -- Public API -------------------------------------

void CpInit(ControlPlane* cp, Ctx* dp) {
  cp->data_plane = dp;
  cp->epoll_fd = -1;
  cp->peer_count = 0;
  cp->watcher_count = 0;
}

void CpDestroy(ControlPlane* cp) {
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

  // HD PeerInfo frames arrive with type byte 0x20 from
  // the data plane pipe (not a FrameType enum member).
  if (static_cast<uint8_t>(type) ==
      static_cast<uint8_t>(HdFrameType::kPeerInfo)) {
    CpHandleHdPeerInfo(cp, fd, payload, payload_len);
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

  cp->running.store(1, std::memory_order_release);
  spdlog::info("control plane running ({} pipes)",
               nworkers);

  // Extra slots for ICE timer + STUN UDP.
  constexpr int kMaxEpollEvents = kMaxWorkers + 2;
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

}  // namespace hyper_derp
