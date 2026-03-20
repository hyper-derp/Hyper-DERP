---
title: "Benchmarks"
description: >-
  Methodology, hardware, and headline comparison numbers.
draft: true
---

## Methodology

All benchmarks use a consistent methodology:

- **Traffic generator**: custom iperf-like tool sending
  DERP-framed traffic over TLS/kTLS
- **Duration**: 60-second runs, 10-second warmup discarded
- **Metrics**: throughput (Mbps), P50/P99 latency, CPU
  utilization, context switches
- **Repetitions**: 3 runs per configuration, median reported
- **Comparison target**: Tailscale's `derper` (latest
  release at time of test)

## Hardware

| Platform | Instance | vCPUs | RAM | Network |
|----------|----------|------:|----:|--------:|
| GCP | c4-highcpu-{2,4,8,16} | 2--16 | 2--32 GB | up to 32 Gbps |
| AWS | c7i.{2xl,4xl} | 8, 16 | 16, 32 GB | up to 25 Gbps |
| Bare metal | Haswell Xeon | 8 | 32 GB | 10 GbE |

## Headline Numbers (GCP c4-highcpu, kTLS)

This is the fair, apples-to-apples comparison: both
servers doing real TLS, Hyper-DERP using kernel TLS
offload.

| vCPU | HD kTLS (Mbps) | TS TLS (Mbps) | HD/TS |
|-----:|---------------:|--------------:|------:|
|    2 |          2,962 |         1,448 |  2.1x |
|    4 |          5,106 |         2,395 |  2.2x |
|    8 |          7,621 |         4,033 |  1.9x |
|   16 |         12,068 |         7,743 |  1.6x |

## Detailed Results

- [GCP kTLS results](/benchmarks/gcp/) -- Phase B fair
  comparison across 2--16 vCPU
- [GCP plain TCP results](/benchmarks/gcp-tcp/) -- full
  sweep without TLS overhead
- [AWS c7i results](/benchmarks/aws/) -- cross-cloud
  validation on 8 and 16 vCPU
- [Bare-metal profiling](/benchmarks/bare-metal/) -- flame
  graphs, perf stat, kTLS cost analysis
