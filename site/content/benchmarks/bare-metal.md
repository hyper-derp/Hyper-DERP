---
title: "Bare-Metal Profiling"
description: >-
  Haswell profiling, flame graphs, perf stat, kTLS cost
  analysis.
weight: 5
draft: false
---

## Setup

- **Relay**: Xeon E5-1650 v3 @ 3.5 GHz, 6C/12T, 15 MB L3,
  ConnectX-4 Lx 25GbE
- **Client**: i5-13600KF, 16C/24T, 30 MB L3, CX4 Lx 25GbE
- **Network**: 25GbE DAC direct link
- **Kernel**: 6.12.74+deb13+1-amd64
- **kTLS**: software only (CX4 Lx has no TLS offload)

## Key Findings

### CPU Breakdown (2 workers @ 5 Gbps)

| Function | % Cycles | Category |
|----------|--------:|----------|
| aes_gcm_dec (kTLS decrypt) | 13.0% | kernel crypto |
| aes_gcm_enc (kTLS encrypt) | 11.8% | kernel crypto |
| rep_movs_alternative (memcpy) | 4.0% | kernel copy |
| skb_release_data | 2.2% | kernel SKB |
| **ForwardMsg (HD user code)** | **2.0%** | user relay |
| memset_orig | 1.7% | kernel alloc |

HD's entire forwarding path -- frame parsing, hash lookup,
SPSC enqueue, frame construction -- consumes 2% of cycles.
kTLS encrypt + decrypt consumes 25%.

### kTLS Cost

Plain TCP vs kTLS on the same hardware isolates the cost
of kernel TLS:

| Workers | kTLS ceiling | TCP ceiling | kTLS tax |
|--------:|------------:|-----------:|--------:|
| 2 | 3,833 Mbps | 7,383 Mbps | 48% |
| 4 | 6,300 Mbps | 8,652 Mbps | 27% |

Plain TCP 2w (7,383 Mbps) exceeds kTLS 4w (6,300 Mbps).
kTLS costs more throughput than doubling workers recovers.

### Cache Cliff

| Load | LLC miss rate |
|------|------------:|
| 3 Gbps (below saturation) | 2.6% |
| 5 Gbps (kTLS 2w saturation) | 40% |

The transition is non-linear. Below ~4 Gbps, kTLS crypto
state and HD's data structures coexist in the 15 MB L3.
Above ~4 Gbps, the crypto working set (cipher state, IV
buffers, SKBs) evicts everything else. Throughput hits a
wall.

This also explains HD's higher CV under kTLS (6-10% vs
TS <1%). The system oscillates around the cliff edge --
crypto pressure evicts the working set, throughput drops,
fewer packets reduce pressure, data fits again, throughput
recovers, and the cycle repeats.

### Comparison to TS

| Config | Throughput | Loss |
|--------|----------:|-----:|
| TS TLS | 4,100 Mbps | 37% |
| HD kTLS 2w | 3,833 Mbps | 3% |
| HD kTLS 4w | 6,680 Mbps | 8% |
| HD TCP 2w | 7,383 Mbps | low |
| HD TCP 4w | 8,652 Mbps | <1% |

HD kTLS 2w delivers slightly less throughput than TS TLS
(3.8 vs 4.1 Gbps) but with dramatically less loss (3% vs
37%). HD kTLS 4w is 1.6x TS. HD TCP 2w is 1.8x TS.
