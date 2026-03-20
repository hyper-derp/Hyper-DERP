---
title: "GCP kTLS Results"
description: >-
  GCP c4-highcpu benchmark results with kernel TLS.
weight: 1
draft: true
---

## Setup

- **Platform**: GCP c4-highcpu instances
- **Region**: us-central1
- **TLS mode**: Hyper-DERP with kTLS, Tailscale derper
  with Go TLS
- **Test configs**: 2, 4, 8, 16 vCPU

## Throughput

| vCPU | HD kTLS (Mbps) | TS TLS (Mbps) | HD/TS |
|-----:|---------------:|--------------:|------:|
|    2 |          2,962 |         1,448 |  2.1x |
|    4 |          5,106 |         2,395 |  2.2x |
|    8 |          7,621 |         4,033 |  1.9x |
|   16 |         12,068 |         7,743 |  1.6x |

<!-- TODO: add throughput plot from static/img/ -->

## Scaling

Hyper-DERP scales near-linearly from 2 to 8 vCPU.
The 8-to-16 gain is sublinear (1.58x), likely due to
memory bandwidth saturation on the c4-highcpu platform.

Tailscale derper shows similar sublinear scaling but
starts from a lower baseline due to Go runtime overhead
and user-space TLS.

<!-- TODO: add scaling plot -->

## Latency

Latency data is available in the full report. P50 and P99
latencies for both servers at each vCPU count will be
added once plots are generated.

<!-- TODO: add latency tables and plots -->
