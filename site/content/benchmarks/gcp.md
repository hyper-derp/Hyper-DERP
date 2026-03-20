---
title: "GCP Benchmarks"
description: "GCP c4-highcpu benchmark results."
weight: 1
---

## Platform

GCP c4-highcpu VMs, Intel Xeon Platinum 8581C. Tested at 2, 4,
8, and 16 vCPU configurations. DERP over kTLS (HD) vs DERP
over userspace TLS (Go derper v1.96.1).

## Throughput

| vCPU | HD Peak | TS Ceiling | Ratio | HD Loss | TS Loss |
|-----:|--------:|-----------:|------:|--------:|--------:|
| 2 | 2,977 Mbps | 1,448 Mbps | 11.8x at 5G | 2.8% | 93.0% |
| 4 | 5,106 Mbps | 2,395 Mbps | 3.7x at 7.5G | 0.8% | 74.2% |
| 8 | 7,366 Mbps | 3,802 Mbps | 1.9x at 10G | 0.4% | 46.1% |
| 16 | 12,068 Mbps | 7,743 Mbps | 1.6x at 20G | 1.7% | 18.9% |

Loss percentages reflect messages remaining in TCP send
buffers when the 15-second test window closes. HD has zero
internal packet drops at all configurations.

## Tail Latency (p99)

| vCPU | HD p99 | TS p99 | Ratio |
|-----:|-------:|-------:|------:|
| 2 | 554 us | 2,718 us | 4.9x |
| 4 | 1,601 us | 2,512 us | 1.6x |
| 8 | 1,097 us | 1,408 us | 1.3x |
| 16 | 636 us | 1,268 us | 2.0x |

## Why HD Is Faster

1. **io_uring batched I/O** -- one `io_uring_enter` handles
   dozens of pending sends. Go derper makes one `write`
   syscall per packet per goroutine.
2. **kTLS** -- AES-GCM runs in the kernel. Workers never
   touch crypto.
3. **Sharded workers** -- each worker owns a disjoint peer
   set. Cross-shard forwarding uses lock-free SPSC rings.
4. **Send coalescing** -- batched submissions produce smoother
   TCP flow, so retransmits decrease under load.

## Full Report

The detailed Phase B report with per-configuration breakdowns
and confidence intervals is in the
[HD-bench-data](https://github.com/hyper-derp/HD-bench-data)
repository.
