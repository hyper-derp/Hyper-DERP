/// @file af_xdp_relay.cc
/// @brief AF_XDP dual-port zero-copy HD relay.
///
/// Standalone kernel-bypass relay for HD Protocol frames
/// over UDP. Uses raw AF_XDP syscalls (no libxdp) for
/// portability across build environments. Loads a BPF
/// redirect program via libbpf to steer matching UDP
/// packets into AF_XDP sockets.
///
/// Usage:
///   af-xdp-relay --port0 ens4f0np0 --port1 ens4f1np1
///     --udp-port 4000 --queue 0 --workers 1
///     --zero-copy --auto-fwd --stats-interval 1

#include <linux/if_xdp.h>
#include <linux/if_ether.h>
#include <linux/if_link.h>
#include <linux/ip.h>
#include <linux/udp.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ---------------------------------------------------------------
// Constants
// ---------------------------------------------------------------

/// UMEM frame size (power of two, >= MTU + headroom).
static constexpr uint32_t kFrameSize = 2048;

/// Frames per UMEM region. 16384 * 2048 = 32 MiB.
static constexpr uint32_t kFrameCount = 16384;

/// UMEM total size in bytes.
static constexpr uint64_t kUmemSize =
    static_cast<uint64_t>(kFrameSize) * kFrameCount;

/// Batch size for ring operations.
static constexpr uint32_t kBatchSize = 64;

/// Maximum forwarding table entries.
static constexpr int kMaxFwdEntries = 4096;

/// Maximum ports (interfaces).
static constexpr int kMaxPorts = 2;

/// Ethernet header size.
static constexpr int kEthHdrLen = 14;

/// IPv4 header size (no options).
static constexpr int kIpHdrLen = 20;

/// UDP header size.
static constexpr int kUdpHdrLen = 8;

/// Minimum packet size for HD frame (headers + 4B HD hdr).
static constexpr int kMinPktLen =
    kEthHdrLen + kIpHdrLen + kUdpHdrLen + 4;

// ---------------------------------------------------------------
// Ring access helpers (memory-mapped AF_XDP rings)
// ---------------------------------------------------------------

/// Producer/consumer ring descriptor for AF_XDP.
struct XskRing {
  uint32_t* producer;
  uint32_t* consumer;
  uint32_t* flags;
  uint32_t mask;
  uint32_t size;
  // For RX/TX rings these point to xdp_desc;
  // for fill/comp rings they point to uint64_t addresses.
  void* descs;
};

/// Read producer index with acquire semantics.
static inline uint32_t RingProd(const XskRing* r) {
  return __atomic_load_n(r->producer, __ATOMIC_ACQUIRE);
}

/// Read consumer index with acquire semantics.
static inline uint32_t RingCons(const XskRing* r) {
  return __atomic_load_n(r->consumer, __ATOMIC_ACQUIRE);
}

/// Number of entries available for consumption.
static inline uint32_t RingAvail(const XskRing* r) {
  return RingProd(r) - RingCons(r);
}

/// Free space in the ring for production.
static inline uint32_t RingFree(const XskRing* r) {
  return r->size - (RingProd(r) - RingCons(r));
}

/// Get xdp_desc pointer at index in RX/TX ring.
static inline struct xdp_desc* RingDesc(
    const XskRing* r, uint32_t idx) {
  auto* base =
      static_cast<struct xdp_desc*>(r->descs);
  return &base[idx & r->mask];
}

/// Get address slot at index in fill/comp ring.
static inline uint64_t* RingAddr(
    const XskRing* r, uint32_t idx) {
  auto* base = static_cast<uint64_t*>(r->descs);
  return &base[idx & r->mask];
}

/// Advance producer with release semantics.
static inline void RingSubmitProd(
    XskRing* r, uint32_t count) {
  __atomic_store_n(
      r->producer, *r->producer + count,
      __ATOMIC_RELEASE);
}

/// Advance consumer with release semantics.
static inline void RingSubmitCons(
    XskRing* r, uint32_t count) {
  __atomic_store_n(
      r->consumer, *r->consumer + count,
      __ATOMIC_RELEASE);
}

// ---------------------------------------------------------------
// AF_XDP socket
// ---------------------------------------------------------------

/// AF_XDP socket with UMEM and all four rings.
struct XskSocket {
  int fd;
  void* umem_area;
  uint64_t umem_size;
  XskRing rx;
  XskRing tx;
  XskRing fill;
  XskRing comp;
  int ifindex;
  int queue_id;
  bool zero_copy;

  // Free frame stack for TX sourcing.
  uint64_t* free_frames;
  uint32_t free_count;
};

/// Forwarding table entry.
struct FwdEntry {
  uint32_t dst_ip;
  uint16_t dst_port;
  uint8_t dst_mac[6];
  // Index into the ports array (0 or 1).
  int dst_port_idx;
  uint8_t occupied;
};

/// Forwarding table key: source IP + port.
struct FwdKey {
  uint32_t src_ip;
  uint16_t src_port;
};

// ---------------------------------------------------------------
// Globals
// ---------------------------------------------------------------

static volatile sig_atomic_t g_running = 1;

// CLI options.
static const char* g_port0_iface = nullptr;
static const char* g_port1_iface = nullptr;
static uint16_t g_udp_port = 4000;
static int g_queue_id = 0;
static int g_num_workers = 1;
static bool g_zero_copy = false;
static bool g_auto_fwd = false;
static int g_stats_interval = 1;
static const char* g_bpf_obj_path = nullptr;

// Ports.
static int g_num_ports = 0;
static XskSocket g_ports[kMaxPorts];

// BPF state.
static struct bpf_object* g_bpf_obj = nullptr;
static int g_xsk_map_fd = -1;

// Forwarding table (linear probe hash).
static FwdEntry g_fwd_table[kMaxFwdEntries];

// Static forwarding rules from CLI.
static constexpr int kMaxStaticRules = 64;
struct StaticRule {
  uint32_t src_ip;
  uint16_t src_port;
  uint32_t dst_ip;
  uint16_t dst_port;
};
static StaticRule g_static_rules[kMaxStaticRules];
static int g_num_static_rules = 0;

// Stats (per-port, updated by relay loop).
struct PortStats {
  uint64_t rx_packets;
  uint64_t rx_bytes;
  uint64_t tx_packets;
  uint64_t tx_bytes;
  uint64_t fwd_miss;
  uint64_t rx_empty;
  uint64_t tx_full;
};
static PortStats g_stats[kMaxPorts];

// ---------------------------------------------------------------
// Signal handler
// ---------------------------------------------------------------

static void SigHandler(int) {
  g_running = 0;
}

// ---------------------------------------------------------------
// Forwarding table operations
// ---------------------------------------------------------------

/// Hash a forwarding key to a table index.
static inline uint32_t FwdHash(FwdKey key) {
  // FNV-1a on 6 bytes (4B IP + 2B port).
  uint64_t h = 14695981039346656037ULL;
  auto* p = reinterpret_cast<const uint8_t*>(&key);
  for (int i = 0; i < 6; ++i) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return static_cast<uint32_t>(h) &
         (kMaxFwdEntries - 1);
}

/// Look up a forwarding entry by source IP+port.
static FwdEntry* FwdLookup(uint32_t src_ip,
                           uint16_t src_port) {
  FwdKey key{src_ip, src_port};
  uint32_t idx = FwdHash(key);
  for (int probe = 0; probe < 32; ++probe) {
    uint32_t i = (idx + probe) & (kMaxFwdEntries - 1);
    FwdEntry* e = &g_fwd_table[i];
    if (!e->occupied) return nullptr;
    if (e->dst_ip == 0 && e->dst_port == 0) continue;
    // Check if this entry's key matches. We store the
    // key implicitly via the hash; for auto-fwd we need
    // a reverse lookup. Store source in the unused bits.
    // Actually we need a proper key match. Let's embed
    // the key.
    return nullptr;
  }
  return nullptr;
}

/// Insert a forwarding entry.
static void FwdInsert(uint32_t src_ip, uint16_t src_port,
                      uint32_t dst_ip, uint16_t dst_port,
                      const uint8_t* dst_mac,
                      int dst_port_idx) {
  FwdKey key{src_ip, src_port};
  uint32_t idx = FwdHash(key);
  for (int probe = 0; probe < 32; ++probe) {
    uint32_t i = (idx + probe) & (kMaxFwdEntries - 1);
    FwdEntry* e = &g_fwd_table[i];
    if (!e->occupied) {
      e->dst_ip = dst_ip;
      e->dst_port = dst_port;
      if (dst_mac) {
        std::memcpy(e->dst_mac, dst_mac, 6);
      }
      e->dst_port_idx = dst_port_idx;
      e->occupied = 1;
      return;
    }
  }
  std::fprintf(stderr, "fwd: table full\n");
}

// Extend FwdEntry to include source key for proper lookup.
struct FwdEntryFull {
  FwdKey src;
  FwdEntry entry;
};

// Use a separate full table for actual lookups.
static FwdEntryFull g_fwd_full[kMaxFwdEntries];

/// Look up forwarding by source address.
static FwdEntry* FwdFind(uint32_t src_ip,
                         uint16_t src_port) {
  FwdKey key{src_ip, src_port};
  uint32_t idx = FwdHash(key);
  for (int probe = 0; probe < 32; ++probe) {
    uint32_t i = (idx + probe) & (kMaxFwdEntries - 1);
    auto* e = &g_fwd_full[i];
    if (!e->entry.occupied) return nullptr;
    if (e->src.src_ip == key.src_ip &&
        e->src.src_port == key.src_port) {
      return &e->entry;
    }
  }
  return nullptr;
}

/// Insert into full forwarding table.
static void FwdFullInsert(uint32_t src_ip,
                          uint16_t src_port,
                          uint32_t dst_ip,
                          uint16_t dst_port,
                          const uint8_t* dst_mac,
                          int dst_port_idx) {
  FwdKey key{src_ip, src_port};
  uint32_t idx = FwdHash(key);
  for (int probe = 0; probe < 32; ++probe) {
    uint32_t i = (idx + probe) & (kMaxFwdEntries - 1);
    auto* e = &g_fwd_full[i];
    if (!e->entry.occupied) {
      e->src = key;
      e->entry.dst_ip = dst_ip;
      e->entry.dst_port = dst_port;
      if (dst_mac) {
        std::memcpy(e->entry.dst_mac, dst_mac, 6);
      }
      e->entry.dst_port_idx = dst_port_idx;
      e->entry.occupied = 1;
      return;
    }
    // Update existing entry with same key.
    if (e->src.src_ip == key.src_ip &&
        e->src.src_port == key.src_port) {
      e->entry.dst_ip = dst_ip;
      e->entry.dst_port = dst_port;
      if (dst_mac) {
        std::memcpy(e->entry.dst_mac, dst_mac, 6);
      }
      e->entry.dst_port_idx = dst_port_idx;
      return;
    }
  }
  std::fprintf(stderr, "fwd: full table, cannot insert\n");
}

// ---------------------------------------------------------------
// Auto-forwarding: learn peers from first two packets
// ---------------------------------------------------------------

// For auto-fwd mode, the first two distinct source
// addresses seen are paired: traffic from peer A is
// forwarded to peer B and vice versa.
static constexpr int kAutoFwdMaxPeers = 2;
struct AutoPeer {
  uint32_t ip;
  uint16_t port;
  uint8_t mac[6];
  int port_idx;
  bool seen;
};
static AutoPeer g_auto_peers[kAutoFwdMaxPeers];
static int g_auto_peer_count = 0;

/// Try to auto-learn a forwarding pair from a packet.
static void AutoLearn(uint32_t src_ip, uint16_t src_port,
                      const uint8_t* src_mac,
                      int rx_port_idx) {
  // Already known?
  for (int i = 0; i < g_auto_peer_count; ++i) {
    if (g_auto_peers[i].ip == src_ip &&
        g_auto_peers[i].port == src_port) {
      return;
    }
  }
  if (g_auto_peer_count >= kAutoFwdMaxPeers) return;

  int idx = g_auto_peer_count++;
  g_auto_peers[idx].ip = src_ip;
  g_auto_peers[idx].port = src_port;
  std::memcpy(g_auto_peers[idx].mac, src_mac, 6);
  g_auto_peers[idx].port_idx = rx_port_idx;
  g_auto_peers[idx].seen = true;

  char ip_str[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &src_ip, ip_str, sizeof(ip_str));
  std::fprintf(stderr,
      "auto-fwd: learned peer %d: %s:%u on port%d\n",
      idx, ip_str, ntohs(src_port), rx_port_idx);

  // When we have two peers, create bidirectional rules.
  if (g_auto_peer_count == 2) {
    auto* a = &g_auto_peers[0];
    auto* b = &g_auto_peers[1];

    // A -> B.
    FwdFullInsert(a->ip, a->port,
                  b->ip, b->port, b->mac,
                  b->port_idx);
    // B -> A.
    FwdFullInsert(b->ip, b->port,
                  a->ip, a->port, a->mac,
                  a->port_idx);

    char a_str[INET_ADDRSTRLEN];
    char b_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a->ip, a_str, sizeof(a_str));
    inet_ntop(AF_INET, &b->ip, b_str, sizeof(b_str));
    std::fprintf(stderr,
        "auto-fwd: paired %s:%u <-> %s:%u\n",
        a_str, ntohs(a->port),
        b_str, ntohs(b->port));
  }
}

// ---------------------------------------------------------------
// IPv4 checksum
// ---------------------------------------------------------------

/// Compute IPv4 header checksum (20 bytes, no options).
static inline uint16_t IpChecksum(const void* hdr) {
  auto* p = static_cast<const uint16_t*>(hdr);
  uint32_t sum = 0;
  for (int i = 0; i < 10; ++i) {
    sum += p[i];
  }
  sum = (sum >> 16) + (sum & 0xFFFF);
  sum += sum >> 16;
  return static_cast<uint16_t>(~sum);
}

// ---------------------------------------------------------------
// UMEM and ring setup (raw syscalls)
// ---------------------------------------------------------------

/// Map a single ring after setsockopt has set its size.
/// @param fd AF_XDP socket fd.
/// @param ring Output ring struct.
/// @param off Ring offsets from XDP_MMAP_OFFSETS.
/// @param pgoff mmap page offset for this ring type.
/// @param ring_size Number of entries.
/// @param is_desc True if entries are xdp_desc, false
///   if uint64_t (fill/comp rings).
/// @returns 0 on success, -errno on failure.
static int MapRing(int fd, XskRing* ring,
                   const struct xdp_ring_offset* off,
                   uint64_t pgoff,
                   uint32_t ring_size,
                   bool is_desc) {
  size_t entry_size =
      is_desc ? sizeof(struct xdp_desc) : sizeof(uint64_t);
  size_t mmap_size =
      static_cast<size_t>(off->desc) +
      ring_size * entry_size;

  void* map = mmap(nullptr, mmap_size,
                   PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_POPULATE,
                   fd, static_cast<off_t>(pgoff));
  if (map == MAP_FAILED) {
    std::fprintf(stderr,
        "mmap ring pgoff=0x%" PRIx64 ": %s\n",
        pgoff, strerror(errno));
    return -errno;
  }

  auto* base = static_cast<uint8_t*>(map);
  ring->producer =
      reinterpret_cast<uint32_t*>(base + off->producer);
  ring->consumer =
      reinterpret_cast<uint32_t*>(base + off->consumer);
  ring->flags =
      reinterpret_cast<uint32_t*>(base + off->flags);
  ring->descs = base + off->desc;
  ring->mask = ring_size - 1;
  ring->size = ring_size;
  return 0;
}

/// Allocate a frame from the free stack.
static inline int64_t AllocFrame(XskSocket* xsk) {
  if (xsk->free_count == 0) return -1;
  return static_cast<int64_t>(
      xsk->free_frames[--xsk->free_count]);
}

/// Return a frame to the free stack.
static inline void FreeFrame(XskSocket* xsk,
                             uint64_t addr) {
  if (xsk->free_count < kFrameCount) {
    xsk->free_frames[xsk->free_count++] = addr;
  }
}

/// Create and bind an AF_XDP socket on the given
/// interface and queue.
static int XskCreate(XskSocket* xsk,
                     const char* iface,
                     int queue_id,
                     bool zero_copy) {
  std::memset(xsk, 0, sizeof(*xsk));
  xsk->fd = -1;
  xsk->queue_id = queue_id;
  xsk->zero_copy = zero_copy;

  // Resolve interface index.
  unsigned int ifidx = if_nametoindex(iface);
  if (ifidx == 0) {
    std::fprintf(stderr, "interface '%s' not found: %s\n",
                 iface, strerror(errno));
    return -errno;
  }
  xsk->ifindex = static_cast<int>(ifidx);

  // Allocate UMEM. Try hugepages first.
  xsk->umem_size = kUmemSize;
  xsk->umem_area = mmap(
      nullptr, static_cast<size_t>(kUmemSize),
      PROT_READ | PROT_WRITE,
      MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
      -1, 0);
  if (xsk->umem_area == MAP_FAILED) {
    std::fprintf(stderr,
        "hugepage mmap failed, falling back to "
        "regular pages\n");
    xsk->umem_area = mmap(
        nullptr, static_cast<size_t>(kUmemSize),
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1, 0);
    if (xsk->umem_area == MAP_FAILED) {
      std::fprintf(stderr,
          "UMEM mmap failed: %s\n", strerror(errno));
      return -errno;
    }
  }

  // Create AF_XDP socket.
  xsk->fd = socket(AF_XDP, SOCK_RAW, 0);
  if (xsk->fd < 0) {
    std::fprintf(stderr,
        "socket(AF_XDP): %s\n", strerror(errno));
    munmap(xsk->umem_area,
           static_cast<size_t>(kUmemSize));
    return -errno;
  }

  // Register UMEM.
  struct xdp_umem_reg umem_reg{};
  umem_reg.addr =
      reinterpret_cast<uint64_t>(xsk->umem_area);
  umem_reg.len = kUmemSize;
  umem_reg.chunk_size = kFrameSize;
  umem_reg.headroom = 0;

  int ret = setsockopt(xsk->fd, SOL_XDP, XDP_UMEM_REG,
                       &umem_reg, sizeof(umem_reg));
  if (ret < 0) {
    std::fprintf(stderr,
        "XDP_UMEM_REG: %s\n", strerror(errno));
    close(xsk->fd);
    munmap(xsk->umem_area,
           static_cast<size_t>(kUmemSize));
    return -errno;
  }

  // Set ring sizes (must be power of two).
  int ring_size = static_cast<int>(kFrameCount);
  ret = setsockopt(xsk->fd, SOL_XDP,
                   XDP_UMEM_FILL_RING,
                   &ring_size, sizeof(ring_size));
  if (ret < 0) {
    std::fprintf(stderr,
        "XDP_UMEM_FILL_RING: %s\n", strerror(errno));
    goto fail;
  }
  ret = setsockopt(xsk->fd, SOL_XDP,
                   XDP_UMEM_COMPLETION_RING,
                   &ring_size, sizeof(ring_size));
  if (ret < 0) {
    std::fprintf(stderr,
        "XDP_UMEM_COMPLETION_RING: %s\n",
        strerror(errno));
    goto fail;
  }
  ret = setsockopt(xsk->fd, SOL_XDP, XDP_RX_RING,
                   &ring_size, sizeof(ring_size));
  if (ret < 0) {
    std::fprintf(stderr,
        "XDP_RX_RING: %s\n", strerror(errno));
    goto fail;
  }
  ret = setsockopt(xsk->fd, SOL_XDP, XDP_TX_RING,
                   &ring_size, sizeof(ring_size));
  if (ret < 0) {
    std::fprintf(stderr,
        "XDP_TX_RING: %s\n", strerror(errno));
    goto fail;
  }

  // Get mmap offsets.
  {
    struct xdp_mmap_offsets offsets{};
    socklen_t optlen = sizeof(offsets);
    ret = getsockopt(xsk->fd, SOL_XDP,
                     XDP_MMAP_OFFSETS,
                     &offsets, &optlen);
    if (ret < 0) {
      std::fprintf(stderr,
          "XDP_MMAP_OFFSETS: %s\n", strerror(errno));
      goto fail;
    }

    // Map all four rings.
    uint32_t rs = static_cast<uint32_t>(ring_size);
    ret = MapRing(xsk->fd, &xsk->fill, &offsets.fr,
                  XDP_UMEM_PGOFF_FILL_RING, rs, false);
    if (ret < 0) goto fail;

    ret = MapRing(xsk->fd, &xsk->comp, &offsets.cr,
                  XDP_UMEM_PGOFF_COMPLETION_RING,
                  rs, false);
    if (ret < 0) goto fail;

    ret = MapRing(xsk->fd, &xsk->rx, &offsets.rx,
                  XDP_PGOFF_RX_RING, rs, true);
    if (ret < 0) goto fail;

    ret = MapRing(xsk->fd, &xsk->tx, &offsets.tx,
                  XDP_PGOFF_TX_RING, rs, true);
    if (ret < 0) goto fail;
  }

  // Initialize free frame stack.
  // Split UMEM: first half for RX (fill ring),
  // second half for TX free frames.
  xsk->free_frames = static_cast<uint64_t*>(
      std::malloc(kFrameCount * sizeof(uint64_t)));
  if (!xsk->free_frames) {
    std::fprintf(stderr, "malloc free_frames failed\n");
    goto fail;
  }
  xsk->free_count = 0;

  // Populate fill ring with first half of frames.
  {
    uint32_t fill_count = kFrameCount / 2;
    uint32_t prod = RingProd(&xsk->fill);
    for (uint32_t i = 0; i < fill_count; ++i) {
      *RingAddr(&xsk->fill, prod + i) =
          static_cast<uint64_t>(i) * kFrameSize;
    }
    RingSubmitProd(&xsk->fill, fill_count);

    // Put second half into TX free stack.
    for (uint32_t i = fill_count; i < kFrameCount; ++i) {
      xsk->free_frames[xsk->free_count++] =
          static_cast<uint64_t>(i) * kFrameSize;
    }
  }

  // Bind to interface + queue.
  {
    struct sockaddr_xdp sxdp{};
    sxdp.sxdp_family = AF_XDP;
    sxdp.sxdp_ifindex = ifidx;
    sxdp.sxdp_queue_id =
        static_cast<uint32_t>(queue_id);
    sxdp.sxdp_flags = zero_copy
        ? XDP_ZEROCOPY : XDP_COPY;

    ret = bind(xsk->fd,
               reinterpret_cast<struct sockaddr*>(&sxdp),
               sizeof(sxdp));
    if (ret < 0 && zero_copy) {
      std::fprintf(stderr,
          "AF_XDP zero-copy bind failed (%s), "
          "retrying with copy mode\n",
          strerror(errno));
      sxdp.sxdp_flags = XDP_COPY;
      xsk->zero_copy = false;
      ret = bind(
          xsk->fd,
          reinterpret_cast<struct sockaddr*>(&sxdp),
          sizeof(sxdp));
    }
    if (ret < 0) {
      std::fprintf(stderr,
          "AF_XDP bind: %s\n", strerror(errno));
      goto fail;
    }
  }

  std::fprintf(stderr,
      "xsk: bound %s queue %d (%s mode)\n",
      iface, queue_id,
      xsk->zero_copy ? "zero-copy" : "copy");
  return 0;

fail:
  if (xsk->fd >= 0) close(xsk->fd);
  if (xsk->umem_area) {
    munmap(xsk->umem_area,
           static_cast<size_t>(kUmemSize));
  }
  std::free(xsk->free_frames);
  xsk->free_frames = nullptr;
  return -1;
}

/// Clean up an AF_XDP socket.
static void XskDestroy(XskSocket* xsk) {
  if (xsk->fd >= 0) close(xsk->fd);
  if (xsk->umem_area) {
    munmap(xsk->umem_area,
           static_cast<size_t>(kUmemSize));
  }
  std::free(xsk->free_frames);
  xsk->free_frames = nullptr;
  xsk->fd = -1;
}

// ---------------------------------------------------------------
// BPF program loading
// ---------------------------------------------------------------

/// Load the AF_XDP redirect BPF program and attach to
/// the given interface. Populates xsk_map and udp_port
/// map entries.
static int BpfLoadAndAttach(const char* obj_path,
                            int ifindex,
                            uint16_t udp_port,
                            int queue_id,
                            int xsk_fd) {
  struct bpf_object* obj =
      bpf_object__open_file(obj_path, nullptr);
  if (!obj) {
    std::fprintf(stderr,
        "bpf: open '%s' failed: %s\n",
        obj_path, strerror(errno));
    return -errno;
  }

  int ret = bpf_object__load(obj);
  if (ret < 0) {
    std::fprintf(stderr,
        "bpf: load failed: %s\n", strerror(-ret));
    bpf_object__close(obj);
    return ret;
  }

  // Find program.
  struct bpf_program* prog =
      bpf_object__find_program_by_name(
          obj, "xdp_af_xdp_redirect");
  if (!prog) {
    std::fprintf(stderr,
        "bpf: program 'xdp_af_xdp_redirect' "
        "not found\n");
    bpf_object__close(obj);
    return -ENOENT;
  }
  int prog_fd = bpf_program__fd(prog);

  // Find maps.
  struct bpf_map* xsk_map =
      bpf_object__find_map_by_name(obj, "xsk_map");
  struct bpf_map* port_map =
      bpf_object__find_map_by_name(obj, "udp_port");
  if (!xsk_map || !port_map) {
    std::fprintf(stderr,
        "bpf: required maps not found\n");
    bpf_object__close(obj);
    return -ENOENT;
  }

  // Set UDP port.
  uint32_t key0 = 0;
  ret = bpf_map_update_elem(
      bpf_map__fd(port_map), &key0, &udp_port, BPF_ANY);
  if (ret < 0) {
    std::fprintf(stderr,
        "bpf: set udp_port failed: %s\n",
        strerror(-ret));
    bpf_object__close(obj);
    return ret;
  }

  // Set XSK map entry for this queue.
  uint32_t qkey = static_cast<uint32_t>(queue_id);
  ret = bpf_map_update_elem(
      bpf_map__fd(xsk_map), &qkey, &xsk_fd, BPF_ANY);
  if (ret < 0) {
    std::fprintf(stderr,
        "bpf: set xsk_map[%d] failed: %s\n",
        queue_id, strerror(-ret));
    bpf_object__close(obj);
    return ret;
  }

  // Attach XDP program. Try native first, fall back
  // to generic.
  ret = bpf_xdp_attach(ifindex, prog_fd,
                        XDP_FLAGS_DRV_MODE, nullptr);
  if (ret < 0) {
    std::fprintf(stderr,
        "bpf: native attach failed (%s), "
        "trying generic\n", strerror(-ret));
    ret = bpf_xdp_attach(ifindex, prog_fd,
                          XDP_FLAGS_SKB_MODE, nullptr);
    if (ret < 0) {
      std::fprintf(stderr,
          "bpf: generic attach failed: %s\n",
          strerror(-ret));
      bpf_object__close(obj);
      return ret;
    }
    std::fprintf(stderr,
        "bpf: attached (generic/SKB mode) on "
        "ifindex %d\n", ifindex);
  } else {
    std::fprintf(stderr,
        "bpf: attached (native/DRV mode) on "
        "ifindex %d\n", ifindex);
  }

  g_bpf_obj = obj;
  g_xsk_map_fd = bpf_map__fd(xsk_map);
  return 0;
}

/// Detach BPF from an interface.
static void BpfDetach(int ifindex) {
  if (ifindex > 0) {
    bpf_xdp_detach(ifindex, 0, nullptr);
  }
}

// ---------------------------------------------------------------
// Packet processing
// ---------------------------------------------------------------

/// Refill the fill ring from completed TX frames.
static void RefillFillRing(XskSocket* xsk) {
  // Reclaim completed TX frames.
  uint32_t comp_avail = RingAvail(&xsk->comp);
  if (comp_avail > 0) {
    if (comp_avail > kBatchSize) {
      comp_avail = kBatchSize;
    }
    uint32_t cons = RingCons(&xsk->comp);
    for (uint32_t i = 0; i < comp_avail; ++i) {
      FreeFrame(xsk,
                *RingAddr(&xsk->comp, cons + i));
    }
    RingSubmitCons(&xsk->comp, comp_avail);
  }

  // Refill fill ring from free stack.
  uint32_t fill_free = RingFree(&xsk->fill);
  if (fill_free == 0 || xsk->free_count == 0) return;
  uint32_t to_fill =
      fill_free < xsk->free_count
          ? fill_free : xsk->free_count;
  if (to_fill > kBatchSize) to_fill = kBatchSize;

  uint32_t prod = RingProd(&xsk->fill);
  for (uint32_t i = 0; i < to_fill; ++i) {
    *RingAddr(&xsk->fill, prod + i) =
        xsk->free_frames[--xsk->free_count];
  }
  RingSubmitProd(&xsk->fill, to_fill);
}

/// Kick the TX ring (sendto with MSG_DONTWAIT).
static void KickTx(XskSocket* xsk) {
  sendto(xsk->fd, nullptr, 0,
         MSG_DONTWAIT, nullptr, 0);
}

/// Process received packets on one port, forwarding to
/// the appropriate destination port.
/// @param rx_idx Index of the receiving port in g_ports.
static void ProcessRx(int rx_idx) {
  XskSocket* rx = &g_ports[rx_idx];
  PortStats* stats = &g_stats[rx_idx];

  uint32_t avail = RingAvail(&rx->rx);
  if (avail == 0) {
    stats->rx_empty++;
    return;
  }
  if (avail > kBatchSize) avail = kBatchSize;

  uint32_t cons = RingCons(&rx->rx);
  bool tx_kicked[kMaxPorts] = {};

  for (uint32_t i = 0; i < avail; ++i) {
    struct xdp_desc* desc =
        RingDesc(&rx->rx, cons + i);
    uint64_t addr = desc->addr;
    uint32_t len = desc->len;

    auto* pkt = static_cast<uint8_t*>(rx->umem_area) +
                addr;

    stats->rx_packets++;
    stats->rx_bytes += len;

    // Need at least Eth + IP + UDP + 4B HD header.
    if (len < static_cast<uint32_t>(kMinPktLen)) {
      FreeFrame(rx, addr);
      continue;
    }

    // Parse headers in-place.
    auto* eth =
        reinterpret_cast<struct ethhdr*>(pkt);
    auto* ip = reinterpret_cast<struct iphdr*>(
        pkt + kEthHdrLen);
    auto* udp = reinterpret_cast<struct udphdr*>(
        pkt + kEthHdrLen + kIpHdrLen);

    uint32_t src_ip = ip->saddr;
    uint16_t src_port = udp->source;

    // Auto-learn if enabled.
    if (g_auto_fwd && g_auto_peer_count < 2) {
      AutoLearn(src_ip, src_port,
                eth->h_source, rx_idx);
    }

    // Look up forwarding rule.
    FwdEntry* fwd = FwdFind(src_ip, src_port);
    if (!fwd) {
      stats->fwd_miss++;
      FreeFrame(rx, addr);
      continue;
    }

    // Determine destination port socket.
    int dst_idx = fwd->dst_port_idx;
    if (dst_idx < 0 || dst_idx >= g_num_ports) {
      dst_idx = rx_idx;
    }
    XskSocket* tx = &g_ports[dst_idx];

    // Check TX ring space.
    if (RingFree(&tx->tx) == 0) {
      stats->tx_full++;
      FreeFrame(rx, addr);
      continue;
    }

    // Rewrite packet headers in-place.
    // Destination MAC.
    std::memcpy(eth->h_dest, fwd->dst_mac, 6);
    // Keep source MAC (NIC will rewrite on TX).

    // IP destination.
    ip->daddr = fwd->dst_ip;
    ip->ttl = 64;
    ip->check = 0;
    ip->check = IpChecksum(ip);

    // UDP destination port.
    udp->dest = fwd->dst_port;
    // Zero UDP checksum (valid for IPv4 per RFC 768).
    udp->check = 0;

    // If forwarding to the same port, we can reuse the
    // RX frame directly in the TX ring (zero-copy
    // forwarding). Otherwise we need to copy to a TX
    // frame on the other port's UMEM.
    if (dst_idx == rx_idx) {
      // Same-port forwarding: submit RX buffer to TX.
      uint32_t tx_prod = RingProd(&tx->tx);
      struct xdp_desc* tx_desc =
          RingDesc(&tx->tx, tx_prod);
      tx_desc->addr = addr;
      tx_desc->len = len;
      tx_desc->options = 0;
      RingSubmitProd(&tx->tx, 1);
      // Don't free — frame will be reclaimed via
      // completion ring.
    } else {
      // Cross-port: allocate frame on dst UMEM, copy.
      int64_t tx_addr = AllocFrame(tx);
      if (tx_addr < 0) {
        stats->tx_full++;
        FreeFrame(rx, addr);
        continue;
      }
      auto* tx_pkt =
          static_cast<uint8_t*>(tx->umem_area) +
          tx_addr;
      std::memcpy(tx_pkt, pkt, len);

      uint32_t tx_prod = RingProd(&tx->tx);
      struct xdp_desc* tx_desc =
          RingDesc(&tx->tx, tx_prod);
      tx_desc->addr = static_cast<uint64_t>(tx_addr);
      tx_desc->len = len;
      tx_desc->options = 0;
      RingSubmitProd(&tx->tx, 1);
      // Free the RX frame back to the RX port.
      FreeFrame(rx, addr);
    }

    tx_kicked[dst_idx] = true;
    g_stats[dst_idx].tx_packets++;
    g_stats[dst_idx].tx_bytes += len;
  }

  RingSubmitCons(&rx->rx, avail);

  // Kick TX on any port that had submissions.
  for (int p = 0; p < g_num_ports; ++p) {
    if (tx_kicked[p]) {
      KickTx(&g_ports[p]);
    }
  }
}

// ---------------------------------------------------------------
// Stats printing
// ---------------------------------------------------------------

/// Print stats delta for all ports.
static void PrintStats(const PortStats* prev,
                       double elapsed_sec) {
  for (int p = 0; p < g_num_ports; ++p) {
    PortStats* cur = &g_stats[p];
    uint64_t rx_pps =
        cur->rx_packets - prev[p].rx_packets;
    uint64_t tx_pps =
        cur->tx_packets - prev[p].tx_packets;
    uint64_t rx_bps =
        (cur->rx_bytes - prev[p].rx_bytes) * 8;
    uint64_t tx_bps =
        (cur->tx_bytes - prev[p].tx_bytes) * 8;
    uint64_t miss =
        cur->fwd_miss - prev[p].fwd_miss;

    double rx_gbps =
        static_cast<double>(rx_bps) / elapsed_sec /
        1e9;
    double tx_gbps =
        static_cast<double>(tx_bps) / elapsed_sec /
        1e9;
    double rx_mpps =
        static_cast<double>(rx_pps) / elapsed_sec /
        1e6;
    double tx_mpps =
        static_cast<double>(tx_pps) / elapsed_sec /
        1e6;

    std::fprintf(stderr,
        "port%d: rx %.2f Mpps (%.2f Gbps)  "
        "tx %.2f Mpps (%.2f Gbps)  "
        "miss %" PRIu64 "\n",
        p, rx_mpps, rx_gbps, tx_mpps, tx_gbps, miss);
  }
}

// ---------------------------------------------------------------
// Clock helper
// ---------------------------------------------------------------

static inline uint64_t NowNs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) *
             1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

// ---------------------------------------------------------------
// CLI parsing
// ---------------------------------------------------------------

/// Parse a static forwarding rule string:
///   src_ip:src_port=dst_ip:dst_port
static bool ParseFwdRule(const char* s) {
  if (g_num_static_rules >= kMaxStaticRules) {
    std::fprintf(stderr, "too many --fwd rules\n");
    return false;
  }

  // Format: A.B.C.D:P=E.F.G.H:Q
  char src_ip_str[64];
  char dst_ip_str[64];
  int src_port_int;
  int dst_port_int;

  // Find '=' separator.
  const char* eq = std::strchr(s, '=');
  if (!eq) {
    std::fprintf(stderr,
        "invalid --fwd format: %s\n", s);
    return false;
  }

  // Parse src side.
  const char* colon = std::strchr(s, ':');
  if (!colon || colon > eq) {
    std::fprintf(stderr,
        "invalid --fwd src: %s\n", s);
    return false;
  }
  size_t ip_len =
      static_cast<size_t>(colon - s);
  if (ip_len >= sizeof(src_ip_str)) return false;
  std::memcpy(src_ip_str, s, ip_len);
  src_ip_str[ip_len] = '\0';
  src_port_int = std::atoi(colon + 1);

  // Parse dst side.
  const char* dst_start = eq + 1;
  colon = std::strchr(dst_start, ':');
  if (!colon) {
    std::fprintf(stderr,
        "invalid --fwd dst: %s\n", s);
    return false;
  }
  ip_len = static_cast<size_t>(colon - dst_start);
  if (ip_len >= sizeof(dst_ip_str)) return false;
  std::memcpy(dst_ip_str, dst_start, ip_len);
  dst_ip_str[ip_len] = '\0';
  dst_port_int = std::atoi(colon + 1);

  auto* rule = &g_static_rules[g_num_static_rules++];
  inet_pton(AF_INET, src_ip_str, &rule->src_ip);
  rule->src_port = htons(
      static_cast<uint16_t>(src_port_int));
  inet_pton(AF_INET, dst_ip_str, &rule->dst_ip);
  rule->dst_port = htons(
      static_cast<uint16_t>(dst_port_int));
  return true;
}

static void Usage(const char* prog) {
  std::fprintf(stderr,
      "Usage: %s [options]\n"
      "  --port0 <iface>      First NIC interface\n"
      "  --port1 <iface>      Second NIC (optional)\n"
      "  --udp-port <port>    UDP port (default: 4000)\n"
      "  --queue <n>          NIC queue (default: 0)\n"
      "  --workers <n>        Workers (default: 1)\n"
      "  --zero-copy          Try AF_XDP zero-copy\n"
      "  --fwd <src:port=dst:port>  Static rule\n"
      "  --auto-fwd           Auto-learn forwarding\n"
      "  --stats-interval <s> Stats interval (default: 1)\n"
      "  --bpf-obj <path>     BPF object file path\n",
      prog);
}

static int ParseArgs(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--port0") == 0 &&
        i + 1 < argc) {
      g_port0_iface = argv[++i];
    } else if (std::strcmp(argv[i], "--port1") == 0 &&
               i + 1 < argc) {
      g_port1_iface = argv[++i];
    } else if (std::strcmp(argv[i], "--udp-port") == 0 &&
               i + 1 < argc) {
      g_udp_port =
          static_cast<uint16_t>(std::atoi(argv[++i]));
    } else if (std::strcmp(argv[i], "--queue") == 0 &&
               i + 1 < argc) {
      g_queue_id = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--workers") == 0 &&
               i + 1 < argc) {
      g_num_workers = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--zero-copy") == 0) {
      g_zero_copy = true;
    } else if (std::strcmp(argv[i], "--fwd") == 0 &&
               i + 1 < argc) {
      if (!ParseFwdRule(argv[++i])) return -1;
    } else if (std::strcmp(argv[i], "--auto-fwd") == 0) {
      g_auto_fwd = true;
    } else if (std::strcmp(argv[i],
                           "--stats-interval") == 0 &&
               i + 1 < argc) {
      g_stats_interval = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--bpf-obj") == 0 &&
               i + 1 < argc) {
      g_bpf_obj_path = argv[++i];
    } else if (std::strcmp(argv[i], "--help") == 0 ||
               std::strcmp(argv[i], "-h") == 0) {
      Usage(argv[0]);
      return 1;
    } else {
      std::fprintf(stderr,
          "unknown option: %s\n", argv[i]);
      Usage(argv[0]);
      return -1;
    }
  }

  if (!g_port0_iface) {
    std::fprintf(stderr, "error: --port0 is required\n");
    Usage(argv[0]);
    return -1;
  }
  return 0;
}

// ---------------------------------------------------------------
// Main
// ---------------------------------------------------------------

int main(int argc, char** argv) {
  int ret = ParseArgs(argc, argv);
  if (ret != 0) return ret < 0 ? 1 : 0;

  // Raise locked memory limit for UMEM.
  struct rlimit rl = {RLIM_INFINITY, RLIM_INFINITY};
  if (setrlimit(RLIMIT_MEMLOCK, &rl) != 0) {
    std::fprintf(stderr,
        "warning: setrlimit(MEMLOCK): %s\n",
        strerror(errno));
  }

  signal(SIGINT, SigHandler);
  signal(SIGTERM, SigHandler);

  // Determine BPF object path.
  const char* bpf_path = g_bpf_obj_path;
  // Default: look next to the binary or in build/bpf.
  char default_path[256];
  if (!bpf_path) {
    std::snprintf(default_path, sizeof(default_path),
                  "bpf/hd_af_xdp.bpf.o");
    bpf_path = default_path;
  }

  // Create AF_XDP sockets for each port.
  g_num_ports = 0;
  ret = XskCreate(&g_ports[0], g_port0_iface,
                  g_queue_id, g_zero_copy);
  if (ret < 0) {
    std::fprintf(stderr,
        "failed to create XSK for %s\n",
        g_port0_iface);
    return 1;
  }
  g_num_ports = 1;

  if (g_port1_iface) {
    ret = XskCreate(&g_ports[1], g_port1_iface,
                    g_queue_id, g_zero_copy);
    if (ret < 0) {
      std::fprintf(stderr,
          "failed to create XSK for %s\n",
          g_port1_iface);
      XskDestroy(&g_ports[0]);
      return 1;
    }
    g_num_ports = 2;
  }

  // Load and attach BPF program on each interface.
  for (int p = 0; p < g_num_ports; ++p) {
    ret = BpfLoadAndAttach(bpf_path,
                           g_ports[p].ifindex,
                           g_udp_port,
                           g_queue_id,
                           g_ports[p].fd);
    if (ret < 0) {
      std::fprintf(stderr,
          "failed to attach BPF on port%d\n", p);
      // Continue anyway; might work without BPF
      // (e.g., in test mode with raw sockets).
    }
  }

  // Insert static forwarding rules.
  // Note: static rules don't have MAC addresses;
  // auto-fwd learns them from traffic.
  for (int i = 0; i < g_num_static_rules; ++i) {
    auto* r = &g_static_rules[i];
    uint8_t zero_mac[6] = {};
    // Default to port0 for same-subnet, port1
    // otherwise.
    FwdFullInsert(r->src_ip, r->src_port,
                  r->dst_ip, r->dst_port,
                  zero_mac, 0);
  }

  std::fprintf(stderr,
      "relay: running on %d port(s), "
      "udp=%u, queue=%d, %s\n",
      g_num_ports, g_udp_port, g_queue_id,
      g_auto_fwd ? "auto-fwd" : "static-fwd");
  std::fprintf(stderr,
      "relay: %d static forwarding rules\n",
      g_num_static_rules);

  // Initialize previous stats for delta reporting.
  PortStats prev_stats[kMaxPorts] = {};
  uint64_t last_stats_ns = NowNs();

  // Main relay loop.
  while (g_running) {
    // Poll all ports for readability.
    struct pollfd pfds[kMaxPorts];
    for (int p = 0; p < g_num_ports; ++p) {
      pfds[p].fd = g_ports[p].fd;
      pfds[p].events = POLLIN;
      pfds[p].revents = 0;
    }

    int nready = poll(pfds, g_num_ports, 100);
    if (nready < 0) {
      if (errno == EINTR) continue;
      std::fprintf(stderr,
          "poll: %s\n", strerror(errno));
      break;
    }

    // Process RX on all ports.
    for (int p = 0; p < g_num_ports; ++p) {
      ProcessRx(p);
      RefillFillRing(&g_ports[p]);
    }

    // Print stats periodically.
    uint64_t now = NowNs();
    double elapsed =
        static_cast<double>(now - last_stats_ns) / 1e9;
    if (elapsed >= g_stats_interval) {
      PrintStats(prev_stats, elapsed);
      for (int p = 0; p < g_num_ports; ++p) {
        prev_stats[p] = g_stats[p];
      }
      last_stats_ns = now;
    }
  }

  std::fprintf(stderr, "\nrelay: shutting down\n");

  // Final stats.
  for (int p = 0; p < g_num_ports; ++p) {
    std::fprintf(stderr,
        "port%d total: rx=%" PRIu64 " tx=%" PRIu64
        " miss=%" PRIu64 "\n",
        p, g_stats[p].rx_packets,
        g_stats[p].tx_packets,
        g_stats[p].fwd_miss);
  }

  // Detach BPF and clean up.
  for (int p = 0; p < g_num_ports; ++p) {
    BpfDetach(g_ports[p].ifindex);
    XskDestroy(&g_ports[p]);
  }
  if (g_bpf_obj) {
    bpf_object__close(g_bpf_obj);
  }

  return 0;
}
