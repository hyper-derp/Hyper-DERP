---
title: "Architecture"
description: >-
  Three-layer design, shard-per-core model, and io_uring
  internals.
sidebar_position: 4
---

## Overview

Hyper-DERP is structured as three layers:

1. **Accept layer** -- listens for incoming connections,
   performs TLS handshake (or hands off to kTLS), and
   assigns each connection to a worker shard.
2. **Data plane** -- per-worker io_uring event loops that
   handle all packet forwarding. Each worker owns a
   disjoint set of connections; no locking required on the
   hot path.
3. **Control plane** -- handles DERP protocol control
   messages (mesh key exchange, peer discovery, keep-alive)
   on a separate low-priority ring.

## Shard-per-Core Model

Each worker thread owns:

- One io_uring instance
- A set of client connections (pinned by accept-round-robin
  or SO_INCOMING_CPU)
- A provided buffer ring for zero-copy receives
- SPSC ring endpoints for cross-worker forwarding

No locks on the forwarding path. Cross-worker traffic
(client A on worker 0 sending to client B on worker 1)
goes through a lock-free SPSC ring per worker pair.

## Provided Buffer Rings

io_uring provided buffer rings let the kernel pick a
buffer from a pre-registered pool at completion time,
avoiding per-recv buffer allocation. Hyper-DERP sizes
these to match the expected DERP frame size and recycles
them after forwarding.

## kTLS Offload

When the kernel supports it, Hyper-DERP hands the TLS
session keys to the kernel via `setsockopt(SOL_TLS)`.
This lets the kernel handle encryption/decryption in
`sendfile`-style zero-copy paths, keeping crypto off the
user-space CPU budget.

## Backpressure

If a destination client's send buffer is full, Hyper-DERP
applies backpressure by:

1. Pausing reads on the source connection (removing it from
   the io_uring poll set)
2. Queuing a bounded number of frames in the SPSC ring
3. Dropping frames beyond the queue limit with a counter
   bump (visible in metrics)

This prevents a slow client from consuming unbounded
memory.

