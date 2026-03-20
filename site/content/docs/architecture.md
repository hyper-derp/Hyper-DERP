---
title: "Architecture"
description: "Three-layer design of the Hyper-DERP relay."
weight: 1
---

Hyper-DERP is a three-layer DERP relay: an accept thread, a
sharded io_uring data plane, and a single-threaded epoll
control plane. The layers share no locks on the forwarding
path.

## Layer Overview

```
Accept Thread
  TCP accept -> kTLS handshake -> HTTP upgrade -> NaCl box
  -> assign peer to data plane shard (FNV-1a hash)

Data Plane (io_uring workers, one per shard)
  Multishot recv with provided buffer rings
  SPSC cross-shard transfer rings (lock-free)
  Batched eventfd signaling
  SEND_ZC for large frames, MSG_MORE coalescing
  Backpressure: pause recv when send queues full

Control Plane (single-threaded, epoll)
  Ping/pong, watcher notifications, peer presence
  Fully isolated from data plane -- no shared locks
```

## Why Shard-Per-Core

Each data plane worker owns its peers exclusively. Cross-shard
forwarding uses lock-free SPSC rings with eventfd signaling.
This eliminates contention on the forwarding path, similar to
the Seastar/ScyllaDB approach but applied to a relay server.

## Data-Oriented Design

Plain structs, free functions, no virtual dispatch.
Cache-friendly field ordering and zero allocation on the hot
path (~70 MB pre-allocated per worker).

## Detailed Reference

The full architecture document with source file references is
in the repository at
[docs/architecture.md](https://github.com/hyper-derp/hyper-derp/blob/main/docs/architecture.md).
