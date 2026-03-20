---
title: "Benchmarks"
description: "Performance measurements and methodology."
---

All benchmarks use 25 runs per high-rate data point, 15-second
test windows, 95% confidence intervals (t-distribution), and
strict process isolation. Go derper v1.96.1 release build.

## Throughput (GCP c4-highcpu, kTLS)

| vCPU | HD Peak | TS Ceiling | Ratio | HD Loss | TS Loss |
|-----:|--------:|-----------:|------:|--------:|--------:|
| 2 | 2,977 Mbps | 1,448 Mbps | 11.8x at 5G | 2.8% | 93.0% |
| 4 | 5,106 Mbps | 2,395 Mbps | 3.7x at 7.5G | 0.8% | 74.2% |
| 8 | 7,366 Mbps | 3,802 Mbps | 1.9x at 10G | 0.4% | 46.1% |
| 16 | 12,068 Mbps | 7,743 Mbps | 1.6x at 20G | 1.7% | 18.9% |

The advantage grows as resources shrink. At 2 vCPU, TS drops
93% of offered traffic at 5 Gbps while HD delivers nearly
3 Gbps.

## Tail Latency (p99)

Measured at the Go derper's maximum sustainable throughput per
vCPU count.

| vCPU | HD p99 | TS p99 | Ratio |
|-----:|-------:|-------:|------:|
| 2 | 554 us | 2,718 us | 4.9x |
| 4 | 1,601 us | 2,512 us | 1.6x |
| 8 | 1,097 us | 1,408 us | 1.3x |
| 16 | 636 us | 1,268 us | 2.0x |

## Real Tailscale Tunnel Test

End-to-end through WireGuard tunnels with real Tailscale
clients and self-hosted Headscale control plane.

| Metric | Go derper | Hyper-DERP | Ratio |
|--------|----------:|-----------:|------:|
| Throughput (1 pair) | 988 Mbps | 1,682 Mbps | 1.70x |
| Retransmits (1 pair) | 9,184 | 560 | 16x fewer |
| Retransmits (3 pairs) | 9,566 | 399 | 24x fewer |
| Retransmits (5 min) | 192,689 | 3,956 | 48x fewer |

## Detailed Reports

- [GCP Phase B](/benchmarks/gcp/) -- c4-highcpu, kTLS
  configurations
- [AWS c7i](/benchmarks/aws/) -- cross-cloud validation
- [Bare-metal profiling](/benchmarks/bare-metal/) -- Haswell
  microarchitecture analysis

Raw data (223 MB, 13,372 files) is preserved in the
[HD-bench-data](https://github.com/hyper-derp/HD-bench-data)
repository.
