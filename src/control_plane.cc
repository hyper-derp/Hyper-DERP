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

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sys/epoll.h>
#include <unistd.h>

#include "hyper_derp/data_plane.h"

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
  CpOnPeerGone(cp, target_key, target->fd,
               PeerGoneReason::kDisconnected);
}

// -- Public API -------------------------------------

void CpInit(ControlPlane* cp, Ctx* dp) {
  std::memset(cp, 0, sizeof(*cp));
  cp->data_plane = dp;
  cp->epoll_fd = -1;
  cp->running = 0;
  cp->peer_count = 0;
  cp->watcher_count = 0;
  std::memset(cp->fd_map, 0, sizeof(cp->fd_map));
  pthread_mutex_init(&cp->mutex, nullptr);
}

void CpDestroy(ControlPlane* cp) {
  if (cp->epoll_fd >= 0) {
    close(cp->epoll_fd);
    cp->epoll_fd = -1;
  }
  pthread_mutex_destroy(&cp->mutex);
}

void CpOnPeerConnect(ControlPlane* cp,
                     const Key& key, int fd) {
  pthread_mutex_lock(&cp->mutex);

  CpRegistryInsert(cp, key, fd);

  // Notify all watchers.
  uint8_t frame[kFrameHeaderSize + kKeySize];
  int n = BuildPeerPresent(frame, key);
  CpNotifyWatchers(cp, frame, n);

  pthread_mutex_unlock(&cp->mutex);
}

void CpOnPeerGone(ControlPlane* cp,
                  const Key& key, int fd,
                  PeerGoneReason reason) {
  pthread_mutex_lock(&cp->mutex);

  CpRemoveWatcher(cp, fd);
  CpRegistryRemove(cp, key.data());

  // Notify all watchers.
  uint8_t frame[kFrameHeaderSize + kKeySize + 1];
  int n = BuildPeerGone(frame, key, reason);
  CpNotifyWatchers(cp, frame, n);

  pthread_mutex_unlock(&cp->mutex);
}

void CpProcessFrame(ControlPlane* cp, int fd,
                    FrameType type,
                    const uint8_t* payload,
                    int payload_len) {
  pthread_mutex_lock(&cp->mutex);

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
        CpOnPeerGone(cp, gone_key, fd,
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

  pthread_mutex_unlock(&cp->mutex);
}

void* CpRunLoop(void* arg) {
  auto* cp = static_cast<ControlPlane*>(arg);
  int nworkers = cp->data_plane->num_workers;

  // Set up epoll to watch all worker control pipes.
  cp->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (cp->epoll_fd < 0) {
    spdlog::error("epoll_create1: {}", strerror(errno));
    return nullptr;
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

  __atomic_store_n(&cp->running, 1, __ATOMIC_RELEASE);
  spdlog::info("control plane running ({} pipes)",
               nworkers);

  epoll_event events[kMaxWorkers];

  while (__atomic_load_n(&cp->running,
                         __ATOMIC_ACQUIRE)) {
    int nev = epoll_wait(cp->epoll_fd, events,
                         kMaxWorkers, 500);
    if (nev < 0) {
      if (errno == EINTR) {
        continue;
      }
      spdlog::error("epoll_wait: {}", strerror(errno));
      break;
    }

    for (int e = 0; e < nev; e++) {
      int wid = static_cast<int>(events[e].data.u32);
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
  return nullptr;
}

void CpStop(ControlPlane* cp) {
  __atomic_store_n(&cp->running, 0, __ATOMIC_RELEASE);
}

}  // namespace hyper_derp
