# Architecture

Hyper-DERP is a three-layer DERP relay: an accept thread, a
sharded io_uring data plane, and a single-threaded epoll
control plane. The layers share no locks on the forwarding
path.

## Overview

```
                       +-----------------------+
                       |    Accept Thread      |
                       |                       |
                       | TCP accept            |
                       | kTLS handshake        |
                       | HTTP upgrade (/derp)  |
                       | NaCl box auth         |
                       | Shard assignment      |
                       +-----------+-----------+
                                   |
              DpAddPeer            | CpOnPeerConnect
              (cmd ring)           | (mutex)
            +------------------+   |   +-----------+
            |                  v   v   v           |
   +--------+------+  +-------+------+  +---------+--+
   | Data Plane    |  | Data Plane   |  | Control    |
   | Worker 0      |  | Worker 1..N  |  | Plane      |
   |               |  |              |  |            |
   | io_uring      |  | io_uring     |  | epoll      |
   | recv/send     |  | recv/send    |  | pipes      |
   | peer hash tbl |  | peer hash tbl|  | peer reg   |
   | frame parsing |  | frame parsing|  | watchers   |
   +-------+-------+  +------+-------+  +------+-----+
           |                  |                 ^
           +--SPSC rings------+    pipe writes  |
           |  (cross-shard)   |    (non-xport)  |
           +--eventfd signal--+-----------------+
```

## Accept Thread

**Entry point:** `server.cc:270` (`AcceptLoop`)

The accept thread runs a blocking `accept(2)` loop on the
listening socket. For each connection it performs, in order:

1. **TCP_NODELAY** -- `server.cc:75` (`SetTcpNodelay`)
2. **kTLS handshake** -- if TLS is configured, calls
   `KtlsAccept` (`ktls.h:82`). OpenSSL 3.x performs
   TLS 1.3 negotiation and auto-installs kTLS for both TX
   and RX. After this, `read()`/`write()` on the fd
   operate on plaintext; AES-GCM runs in the kernel.
3. **HTTP upgrade** -- reads the HTTP request
   (`server.cc:84`, `ReadHttpRequest`), routes by path
   (`/derp`, `/derp/probe`, `/generate_204`), and sends
   an HTTP 101 Switching Protocols response.
4. **DERP handshake** -- sends ServerKey frame, receives
   ClientInfo (NaCl box encrypted with Curve25519), sends
   ServerInfo. See `handshake.h:144` (`PerformHandshake`).
5. **Shard assignment** -- the peer's 32-byte public key is
   hashed with FNV-1a (`data_plane.cc:81`, `PeerWorker`)
   to select a worker. `DpAddPeer` (`data_plane.h:34`)
   enqueues a `kCmdAddPeer` command on the target worker's
   command ring.
6. **Control plane registration** -- `CpOnPeerConnect`
   (`control_plane.h:92`) registers the peer and notifies
   all watchers with PeerPresent frames.

Rate limiting is per-second token bucket
(`server.cc:298`). Connections exceeding the limit are
closed immediately.

## Data Plane

**Entry point:** `data_plane.cc:1` (`DpInit`, `DpRun`)

The data plane is a set of `N` worker threads, each owning
an independent `io_uring` instance and a disjoint set of
peers. Workers share no mutable state except cross-shard
transfer rings.

### Worker Structure

Each `Worker` (`types.h:244`) contains:

- **io_uring instance** -- `kUringQueueDepth` (4096) SQ/CQ
  entries. Uses `SINGLE_ISSUER` + `DEFER_TASKRUN` flags on
  kernel 6.0+ for reduced overhead. Optional `SQPOLL` mode
  for kernel-side SQ polling (`types.h:322`).
- **Peer hash table** -- open-addressing linear probe,
  `kHtCapacity` (4096) slots (`types.h:258`). Indexed by
  FNV-1a hash of the peer's Curve25519 key.
- **fd-to-Peer map** -- `kMaxFd` (65536) entry direct
  lookup array for O(1) fd resolution (`types.h:261`).
- **Replicated routing table** -- each worker has a full
  copy of the global routing table (`types.h:271`,
  `Route`). Writers use atomic store with release ordering.
  Readers use acquire loads. No lock needed.
- **Command ring** -- SPSC ring from the control plane
  (`types.h:223`, `CmdRing`). Commands: `kCmdAddPeer`,
  `kCmdRemovePeer`, `kCmdWrite`, `kCmdStop`.
- **Cross-shard transfer inbox** -- one `XferSpscRing`
  (`types.h:233`) per source worker. Worker `i` writes
  to `dest->xfer_inbox[i]`. Ring size is `kXferSpscSize`
  (16384). N workers create N*N rings total.
- **Slab allocator** -- `kSlabSize` (65536) pre-allocated
  `SendItem` nodes for the send queue. Falls back to
  `malloc` on exhaustion (`types.h:324`).
- **Frame pool** -- `kFramePoolCount` (16384) fixed-size
  buffers of `kFramePoolBufSize` (2048) bytes for
  forwarded frames. 32 MiB per worker (`types.h:117`).

### Receive Path

Workers use multishot recv with provided buffer rings
(kernel 6.0+, `types.h:318`). `kPbufCount` (512) buffers
of `kPbufSize` (65541 bytes, matching max DERP frame size)
are registered with io_uring.

Frame parsing uses the DERP 5-byte header (1 byte type +
4 bytes big-endian length, `protocol.h:30`). Per-peer
reassembly buffers handle partial reads (`types.h:173`).

### Send Path

Sends use `SEND_ZC` (zero-copy) for large frames to
eliminate userspace-to-kernel copies (`data_plane.cc:12`).
Per-peer send queues are capped at `kMaxSendQueueDepth`
(2048) items with `kMaxSendsInflight` (16) concurrent
in-flight SQEs per peer (`types.h:51`).

Send coalescing: `MSG_MORE` is set on the first send of a
batch, and `kPollWriteBatch` (16) caps sends per POLLOUT
event to prevent a single peer from monopolizing the SQ
(`types.h:56`).

### Backpressure

Adaptive send-pressure-based recv pausing
(`types.h`, `SendPressureHigh`). When total queued sends
across all peers on a worker exceed a threshold, recv is
paused. The threshold scales linearly with peer count:

```
high = min(kSendPressureMax, peer_count * kPressurePerPeer)
low  = high / PressureResumeDiv(peer_count)
```

With defaults: `kSendPressureMax` = 32768,
`kPressurePerPeer` = 512. The resume divisor adapts to
worker count: 8 for ≤12 peers (1-2 workers), 6 for ≤24
peers (3-4 workers), 4 for 25+ peers. This wider
hysteresis prevents backpressure oscillation that caused
latency stalls on 2-worker configs.

Once recv is paused, it stays paused for at least
`kRecvPauseMinBatches` (8) CQE batch iterations before
checking the Low threshold. This minimum pause duration
prevents rapid toggling when drain rate is high.

The busy-spin count is also reduced for 1-2 worker
configs (`kBusySpinLowWorker` = 64 vs default 256),
giving kTLS more CPU time to drain send queues on cores
shared with the kernel crypto thread.

### Cross-Shard Forwarding

When a peer sends a packet to a destination on a different
worker, the source worker writes a pre-framed `Xfer` item
(`types.h:196`) to the destination worker's SPSC inbox.
Signal delivery is batched: a bitmask
(`xfer_signal_pending`, `types.h:308`) tracks which
destination workers need an eventfd wake-up. Signals are
sent once per CQE batch iteration, not per frame.

The destination worker uses an atomic bitmask
(`pending_sources`, `types.h:315`) to determine which
source rings to drain. This gives O(active sources)
processing instead of O(N-1).

Generation counters (`fd_gen`, `types.h:265`) guard
against fd reuse races in cross-shard transfers.

## Control Plane

**Entry point:** `control_plane.h:124` (`CpRunLoop`)

The control plane is a single thread running an epoll loop
over per-worker pipe read ends (`control_plane.h:69`). It
handles DERP control frames that the data plane does not
process inline:

- **WatchConns** -- registers a watcher (`kCpMaxWatchers`
  = 64 concurrent watchers, `control_plane.h:29`).
- **PeerPresent / PeerGone** -- sent to all watchers when
  peers connect or disconnect.
- **Ping / Pong** -- echoed back to the sender.
- **NotePreferred** -- marks a peer as the client's
  preferred relay.
- **ClosePeer** -- requests disconnection of a peer.

The control plane maintains an open-addressing hash table
of `kCpMaxPeers` (4096) entries (`control_plane.h:26`) and
an fd-to-peer map for O(1) lookup (`control_plane.h:58`).

Data plane workers write non-transport frames to pipes via
per-worker pipe write fds (`types.h:288`). The pipe
message format is a 9-byte header: 4 bytes fd, 1 byte
frame type, 4 bytes payload length
(`control_plane.h:35`).

## Why Shard-Per-Core

The shard-per-core model (similar to Seastar/ScyllaDB)
eliminates contention on the data path:

- **No shared maps.** Each worker owns its peer set. Go
  derper's goroutines contend on a shared `sync.Mutex`-
  protected map for every packet lookup.
- **No cross-thread allocation.** Each worker has its own
  slab and frame pool. No global allocator lock.
- **Batched I/O.** One `io_uring_enter` call submits
  dozens of send/recv operations. Go derper makes one
  `write(2)` syscall per packet per goroutine.
- **Cache locality.** Hot peer state (fd, rbuf_len,
  send_head) is in the first 24 bytes of `Peer`
  (`types.h:152`). Hash table probing touches
  cache-friendly sequential memory.

The trade-off is cross-shard forwarding overhead. When peer
A (on worker 0) sends to peer B (on worker 1), the frame
crosses a SPSC ring and an eventfd signal. This adds ~1-2
microseconds of latency compared to same-shard forwarding.
FNV-1a hashing distributes peers roughly uniformly, so
cross-shard traffic is proportional to `(N-1)/N` of total
traffic.

## Thread Model

| Thread | Count | Role |
|--------|------:|------|
| Accept | 1 | TCP accept, kTLS, HTTP, handshake |
| Data plane workers | N | io_uring recv/send, forwarding |
| Control | 1 | Peer registry, watchers, ping/pong |
| Metrics | 1 | HTTP server (Crow) for /metrics |
| Stop poller | 1 | Bridges signal handler to shutdown |

Total: N + 4 threads (where N = `--workers`, default =
`hardware_concurrency`).

## Key Constants

| Constant | Value | Location |
|----------|------:|----------|
| `kHtCapacity` | 4096 | `types.h:18` |
| `kCmdRingSize` | 1024 | `types.h:21` |
| `kXferSpscSize` | 16384 | `types.h:27` |
| `kMaxWorkers` | 32 | `types.h:30` |
| `kMaxFd` | 65536 | `types.h:33` |
| `kUringQueueDepth` | 4096 | `types.h:46` |
| `kMaxSendsInflight` | 16 | `types.h:51` |
| `kMaxCqeBatch` | 256 | `types.h:60` |
| `kSlabSize` | 65536 | `types.h:42` |
| `kPbufCount` | 512 | `types.h:104` |
| `kFramePoolCount` | 16384 | `types.h:118` |
| `kCpMaxPeers` | 4096 | `control_plane.h:26` |
| `kCpMaxWatchers` | 64 | `control_plane.h:29` |
