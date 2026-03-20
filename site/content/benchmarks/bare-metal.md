---
title: "Bare-Metal Profiling"
description: >-
  Haswell profiling, flame graphs, perf stat, kTLS cost
  analysis.
weight: 4
draft: true
---

## Setup

- **Hardware**: Haswell Xeon E5, 8 cores, 32 GB RAM,
  10 GbE NIC
- **Config**: 2 workers, 5 Gbps target throughput
- **Profiling**: `perf record` + `perf stat`

## Key Findings

### CPU Breakdown (2 workers @ 5 Gbps)

- **Hyper-DERP user code**: 2% of CPU cycles
- **kTLS crypto** (AES-GCM in kernel): 25% of cycles
- **Remaining**: kernel network stack, io_uring, scheduling

### Context Switches

| Server | Context Switches / sec |
|--------|----------------------:|
| Hyper-DERP | 3,000 |
| Tailscale derper | 963,000 |

The 300x difference comes from io_uring's batched
submission model versus Go's per-syscall goroutine
scheduling.

### Flame Graph

<!-- TODO: embed flame_hd_2w_5000.svg -->

The flame graph shows that the vast majority of Hyper-DERP
time is spent in kernel TLS encryption paths, with
minimal user-space overhead.

### kTLS Cost

Comparing plain TCP vs kTLS on the same hardware isolates
the cost of kernel TLS:

<!-- TODO: add kTLS vs TCP comparison table -->

### Cache Behavior

At high connection counts, L3 cache pressure increases
noticeably. The "cache cliff" appears around 8,000
connections on this Haswell platform, where per-connection
state exceeds the effective L3 working set.

<!-- TODO: add cache miss data -->
