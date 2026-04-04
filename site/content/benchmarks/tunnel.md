---
title: "Tunnel Quality"
description: >-
  WireGuard end-to-end through DERP. iperf3 UDP + TCP +
  ping, 720 runs.
weight: 3
draft: false
---

## Methodology

Measures what applications experience through actual
WireGuard tunnels relayed via DERP.

- **Traffic path**: app -> WireGuard encrypt -> Tailscale
  framing -> DERP relay -> Tailscale deframing -> WireGuard
  decrypt -> app
- **Mesh**: Headscale coordination, 4 Tailscale clients,
  direct UDP blocked (forces DERP)
- **Measurements per run**: iperf3 UDP (throughput + loss +
  jitter), iperf3 TCP (retransmits), ICMP ping (600
  samples)
- **Rates**: 500M, 1G, 2G, 3G, 5G, 8G offered
- **Runs**: 20 per data point
- **Configs**: 4, 8, 16 vCPU
- **Total**: 720 runs

## Key Finding: WireGuard Is the Bottleneck

| Config | HD UDP @ 8G | TS UDP @ 8G | HD Retx | TS Retx | HD Ping | TS Ping |
|--------|------------:|------------:|--------:|--------:|--------:|--------:|
| 4 vCPU | 2,100 Mbps | 2,115 Mbps | 4,852 | 5,217 | 0.90 ms | 0.55 ms |
| 8 vCPU | 2,053 Mbps | 2,060 Mbps | 4,552 | 4,484 | 0.98 ms | 0.90 ms |
| 16 vCPU | 2,059 Mbps | 2,223 Mbps | 4,291 | 4,617 | 0.91 ms | 1.19 ms |

Both relays deliver identical UDP throughput (~2 Gbps)
because Tailscale's userspace WireGuard (wireguard-go,
ChaCha20-Poly1305) is the throughput ceiling, not the
relay. Loss is negligible for both (<0.04%).

TCP retransmits: HD produces 7--8% fewer at max load on
4 and 16 vCPU. Tied at 8 vCPU.

## Rate Scaling

| Rate | HD UDP (4v) | TS UDP (4v) | HD TCP (4v) | TS TCP (4v) |
|-----:|------------:|------------:|------------:|------------:|
| 500M | 500 Mbps | 500 Mbps | 3,911 Mbps | 3,878 Mbps |
| 1G | 975 Mbps | 975 Mbps | 3,931 Mbps | 3,937 Mbps |
| 3G | 1,025 Mbps | 1,062 Mbps | 3,849 Mbps | 3,895 Mbps |
| 5G | 1,318 Mbps | 1,327 Mbps | 3,146 Mbps | 3,154 Mbps |
| 8G | 2,100 Mbps | 2,115 Mbps | 1,217 Mbps | 1,118 Mbps |

UDP throughput plateaus at ~1--2 Gbps regardless of
offered rate -- the WireGuard crypto ceiling.

## Interpretation

Switching from TS to HD as the relay is transparent to
applications running through WireGuard tunnels. The
performance advantages measured in the relay benchmarks
represent additional headroom -- capacity to serve more
tunnels, more peers, or handle traffic bursts without
dropping packets.

With kernel WireGuard (wg.ko) replacing the userspace
client, the tunnel ceiling would move from ~2 Gbps to
10+ Gbps, where HD's relay advantage becomes directly
visible.
