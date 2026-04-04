---
title: "Hyper-DERP"
description: >-
  A high-performance DERP relay server written in C++23
  using io_uring. 2--10x throughput, 40% lower tail
  latency than Tailscale's derper.
draft: false
---

<div class="stats-grid">
  <div class="stat-card">
    <span class="number">16.5 Gbps</span>
    <span class="label">peak at 16 vCPU</span>
  </div>
  <div class="stat-card">
    <span class="number">2--10x</span>
    <span class="label">throughput vs Tailscale derper</span>
  </div>
  <div class="stat-card">
    <span class="number">50%</span>
    <span class="label">compute cost reduction</span>
  </div>
  <div class="stat-card">
    <span class="number">3K</span>
    <span class="label">context switches (vs 963K)</span>
  </div>
</div>

## Peak Throughput (GCP c4-highcpu, kTLS)

4,903 benchmark runs. 4 client VMs, 20 peers, 10 pairs,
20 runs per data point, 95% CIs.

| vCPU | HD Peak (Mbps) | TS Ceiling (Mbps) | HD/TS |
|-----:|---------------:|-------------------:|------:|
|    2 |          3,730 |              1,870 | 10.8x |
|    4 |          6,091 |              2,798 |  3.5x |
|    8 |         12,316 |              4,670 |  2.7x |
|   16 |         16,545 |              7,834 |  2.1x |

HD delivers the same throughput on half the vCPUs --
50% compute cost reduction for a relay fleet.

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
