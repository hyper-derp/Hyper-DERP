---
title: "Hyper-DERP"
---

## What Is This?

[DERP](https://tailscale.com/blog/how-tailscale-works/)
(Designated Encrypted Relay for Packets) is the relay protocol
Tailscale clients fall back to when direct WireGuard connections
fail. Hyper-DERP is a drop-in replacement for Tailscale's
Go-based [derper](https://pkg.go.dev/tailscale.com/derp) that
delivers dramatically higher throughput, fewer TCP retransmits,
and lower tail latency under load.

It is compatible with Tailscale, Headscale, and any standard
DERP client.

## Architecture

Three isolated layers with no shared locks on the forwarding
path:

- **Accept thread** -- TCP accept, kTLS handshake, HTTP
  upgrade, NaCl box authentication, shard assignment.
- **Data plane** -- sharded io_uring workers with multishot
  recv, SPSC cross-shard rings, batched sends, and
  backpressure.
- **Control plane** -- single-threaded epoll for ping/pong,
  watcher notifications, and peer presence.

Read the [architecture docs](/docs/architecture/) for details.

## Quick Start

```sh
cmake --preset default
cmake --build build -j
sudo modprobe tls
./build/hyper-derp --port 443 \
  --cert /path/to/cert.pem --key /path/to/key.pem
```

See the [install page](/install/) for APT repository setup and
the [build docs](/docs/building/) for dependencies and
cross-compilation.
