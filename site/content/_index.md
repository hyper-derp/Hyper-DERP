---
title: "Hyper-DERP"
description: >-
  A high-performance DERP relay server written in C++23
  using io_uring. Drop-in replacement for Tailscale's
  derper with 1.6--2.2x the throughput.
draft: true
---

Hyper-DERP is a drop-in replacement for Tailscale's DERP
relay server, built in C++23 with io_uring. It delivers
2x the throughput at half the hardware, with near-zero
packet loss where Tailscale's derper shreds up to 93% of
traffic. In real end-to-end tunnel tests through
WireGuard, Hyper-DERP produces 48x fewer TCP retransmits.

Compatible with Tailscale, Headscale, and any standard
DERP client. MIT licensed.

<div class="stats-grid">
  <div class="stat-card">
    <span class="number">2.2x</span>
    <span class="label">throughput at 4 vCPU (kTLS)</span>
  </div>
  <div class="stat-card">
    <span class="number">12 Gbps</span>
    <span class="label">peak at 16 vCPU</span>
  </div>
  <div class="stat-card">
    <span class="number">2%</span>
    <span class="label">user-space CPU at 5 Gbps</span>
  </div>
  <div class="stat-card">
    <span class="number">3K</span>
    <span class="label">context switches (vs 963K)</span>
  </div>
</div>

## Performance (GCP c4-highcpu, kTLS)

| vCPU | HD kTLS (Mbps) | TS TLS (Mbps) | HD/TS |
|-----:|---------------:|--------------:|------:|
|    2 |          2,962 |         1,448 |  2.1x |
|    4 |          5,106 |         2,395 |  2.2x |
|    8 |          7,621 |         4,033 |  1.9x |
|   16 |         12,068 |         7,743 |  1.6x |

[Full benchmark results &rarr;](/benchmarks/)

## Quick Install

{{< install-tabs >}}

[Detailed installation guide &rarr;](/install/)

## Architecture

Hyper-DERP uses a shard-per-core design with io_uring
for all I/O, kernel TLS offload, provided buffer rings
for zero-copy receives, and SPSC rings for cross-worker
communication.

[Architecture docs &rarr;](/docs/architecture/)
&middot;
[Source on GitHub &rarr;](https://github.com/hyper-derp/hyper-derp)
