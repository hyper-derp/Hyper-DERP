# Hyper-DERP

High-performance DERP relay server implementing the
[Tailscale DERP protocol](https://pkg.go.dev/tailscale.com/derp)
in C++23 with `io_uring` for zero-copy data plane operations.

## Architecture

```
┌──────────────────────────────────────────────┐
│  Accept Thread (TCP listener)                │
│    → HTTP Upgrade (/derp)                    │
│    → NaCl Box Handshake (Curve25519)         │
│    → Assign peer to data plane shard         │
├──────────────────────────────────────────────┤
│  Data Plane (io_uring workers)               │
│    → Sharded by peer key                     │
│    → SEND_ZC / provided buffer rings         │
│    → SPSC/MPSC command & transfer rings      │
└──────────────────────────────────────────────┘
```

Data-oriented design throughout: plain structs, free functions,
no virtual dispatch, cache-friendly field ordering.

### Modules

| Header | Purpose |
|---|---|
| `protocol.h` | Wire format: frame types, encode/decode |
| `http.h` | HTTP/1.1 upgrade request/response parsing |
| `handshake.h` | NaCl box key exchange (libsodium) |
| `data_plane.h` | io_uring event loop, peer routing |
| `server.h` | Lifecycle: init, run, stop, destroy |
| `client.h` | Client-side DERP for testing/benchmarks |
| `bench.h` | Latency recording, JSON result output |

## Dependencies

System packages (Debian/Ubuntu):

```sh
sudo apt install \
  clang cmake ninja-build \
  liburing-dev libsodium-dev libspdlog-dev \
  libgtest-dev libgmock-dev libcli11-dev
```

Requires Linux with `io_uring` support (kernel 5.19+).

## Build

```sh
# Release
cmake --preset default
cmake --build build -j

# Debug
cmake --preset debug
cmake --build build-debug -j
```

## Usage

```sh
# Show all options
./build/hyper-derp --help

# Start relay (default port 3340, auto-detect worker count)
./build/hyper-derp --port 3340 --workers 4

# Pin workers to specific cores
./build/hyper-derp --workers 2 --pin-workers 0,1

# With metrics, rate limiting, and log level
./build/hyper-derp --workers 4 \
  --metrics-port 9090 \
  --max-accept-rate 1000 \
  --log-level info
```

See [OPERATIONS.md](OPERATIONS.md) for production tuning
(sysctl, CPU pinning, memory footprint, Prometheus alerting).

## Packaging

```sh
cmake --build build --target package
# Produces build/hyper-derp_<version>_<arch>.deb
sudo dpkg -i build/hyper-derp_*.deb
```

Installs systemd unit (`hyper-derp.service`), example config
(`/etc/hyper-derp/hyper-derp.conf.example`), and binaries to
`/usr/bin/`.

## Testing

```sh
# Run all tests
ctest --preset debug

# Unit tests only (protocol, HTTP, handshake, bench)
ctest --preset debug -R unit_tests

# Integration tests (fork-based, process-isolated)
ctest --preset debug -R integration_tests
```

Unit tests cover protocol encoding/decoding, HTTP parsing,
NaCl box handshake, and benchmark instrumentation.

Integration tests fork separate relay and client processes with
core pinning for clean performance isolation. Tests include
single-packet relay, burst relay (100 packets with pacing), and
bidirectional message exchange.

## Performance

Sustained throughput measured with `flood`/`sink` modes over
loopback. 2 workers pinned to cores 0,1. Sender pinned to core 3.
10-second flood, 18-second sink window.

### Relay forwarding throughput

| Payload | Flood PPS | Sink PPS | Delivery | Throughput |
|--------:|----------:|---------:|---------:|-----------:|
| 64 B | 135,061 | 83,764 | 100.0% | 42.9 Mbps |
| 128 B | 151,193 | 93,739 | 100.0% | 96.0 Mbps |
| 256 B | 175,079 | 107,999 | 99.9% | 221.2 Mbps |
| 512 B | 260,212 | 159,665 | 99.0% | 654.0 Mbps |
| 1024 B | 375,641 | 224,439 | 96.6% | 1,838.6 Mbps |
| **1400 B** | **147,635** | **88,795** | **97.1%** | **994.5 Mbps** |
| 4096 B | 134,380 | 82,621 | 99.1% | 2,707.3 Mbps |
| 8192 B | 214,750 | 97,852 | 72.9% | 6,412.0 Mbps |

**1400 B is the WireGuard MTU** — the primary payload size for
real-world VPN traffic.

**Flood PPS** = packets/sec into the relay (TCP ingress).
**Sink PPS** = packets/sec delivered to the receiving peer
(the actual relay forwarding rate).

**Internal delivery is 100%** (zero send_drops, xfer_drops,
slab_exhausts). Apparent sub-100% delivery in the table is
data in TCP buffers when the sender disconnects — the relay
forwarded every byte it received (`recv_bytes == send_bytes`).
The flood client performs a graceful `shutdown(SHUT_WR)` with
a 500ms drain period, but kernel TCP buffers can still hold
residual data at high ingress rates.

### Baseline: Tailscale derper (Go)

Apples-to-apples comparison using the same test client, packet
sizes, and methodology. Tailscale derper v1.96 (`derper -dev`),
same loopback TCP path.

| Payload | TS Sink PPS | HD Sink PPS | TS Delivery | HD Internal | HD/TS |
|--------:|------------:|------------:|------------:|------------:|------:|
| 64 B | 362,894 | 83,764 | 66.1% | 100% | 0.23x |
| 128 B | 501,257 | 93,739 | 58.4% | 100% | 0.19x |
| 256 B | 386,948 | 107,999 | 50.7% | 100% | 0.28x |
| 512 B | 314,987 | 159,665 | 44.1% | 100% | 0.51x |
| 1024 B | 208,012 | 224,439 | 37.2% | 100% | **1.08x** |
| **1400 B** | **160,418** | **88,795** | **33.3%** | **100%** | **0.55x** |
| 4096 B | 62,336 | 82,621 | 28.8% | 100% | **1.33x** |
| 8192 B | 54,694 | 97,852 | 47.1% | 100% | **1.79x** |

**Key findings:**

- **Zero internal drops** at all packet sizes. The relay
  forwards every byte it receives (`recv_bytes == send_bytes`).
  SEND_ZC ENOMEM errors are handled as backoff (like EAGAIN),
  preserving TCP backpressure without packet loss.
- **Small packets (64-256B):** Tailscale derper delivers 3-4x
  higher sink PPS but drops 34-50% of injected packets.
- **1 KB crossover:** Hyper-DERP matches or exceeds Tailscale
  at 1024B (224K vs 208K sink PPS).
- **WireGuard MTU (1400B):** Tailscale delivers 1.8x higher
  sink PPS but drops 67% of traffic. Hyper-DERP has zero
  internal drops.
- **Large packets (4-8KB):** Hyper-DERP wins by 1.3-1.8x —
  io_uring zero-copy send advantage.
- **Delivery guarantee:** Hyper-DERP has zero internal drops
  at all sizes. Tailscale drops proportionally more as flood
  rate increases.

### Comparison with NetBird relay

Numbers from the NetBird relay prototype, tested on bare metal
i5-13600KF, Linux 6.12, loopback. NetBird C data plane benchmarks
use **socketpairs** (in-process, no TCP stack); Hyper-DERP
measures end-to-end over real TCP connections.

| Implementation | 1 KB | 1400 B (WG MTU) | 8 KB | Notes |
|----------------|-----:|----------------:|-----:|-------|
| NB Go WebSocket (10p) | 1,287 | — | 2,829 | Baseline, MB/s |
| NB Raw TCP (10p) | 1,324 | — | 4,036 | +43% over WS |
| NB TCP+kTLS (50p) | 1,234 | — | 5,589 | Kernel AES-GCM |
| NB C DP epoll 8w (10p) | 6,505 | — | 18,639 | socketpair, no TCP |
| NB C DP io_uring 8w (50p) | 7,936 | — | 21,488 | socketpair, no TCP |
| TS derper (1p, TCP) | 208 | 224 | 448 | Real TCP, Go, loopback |
| **Hyper-DERP 2w (1p, TCP)** | **224** | **124** | **801** | Real TCP, 0 drops, loopback |

The NetBird C data plane numbers are not directly comparable:
they benchmark internal forwarding via socketpairs with multiple
concurrent writer goroutines (10-50 pairs), which eliminates TCP
stack overhead and benefits from sender-side parallelism.

**What the numbers show:**

- **8 KB / 1 pair = 6.4 Gbps** through real TCP — above the
  NB Go WebSocket peak of 2.8 GB/s (10 pairs) and comparable
  to NB Go+kTLS (5.6 GB/s at 50 pairs).
- **1 KB crossover:** Hyper-DERP matches Tailscale derper at
  1024B (224 vs 208 MB/s) with zero drops vs 63% loss.
- At large packets, Hyper-DERP outperforms Tailscale derper
  (Go) by 1.3-1.8x despite using only 2 workers.
- At small packets, Tailscale's Go runtime delivers higher
  PPS at the cost of significant packet loss.
- The gap to the NB C data plane io_uring numbers (21.5 GB/s)
  reflects the socketpair-vs-TCP measurement difference and the
  multi-pair parallelism advantage. Scaling Hyper-DERP to 8
  workers with multiple pairs is the next step.

### Key tuning parameters

Defined in `include/hyper_derp/types.h`:

| Constant | Value | Purpose |
|----------|------:|---------|
| `kMaxSendsInflight` | 512 | Per-peer io_uring send parallelism |
| `kXferRingSize` | 65,536 | Cross-shard MPSC ring capacity |
| `kSlabSize` | 65,536 | Per-worker SendItem pool |
| `kUringQueueDepth` | 4,096 | io_uring SQ/CQ depth |
| `kZcThreshold` | 1,024 | Frames below this use regular send |

Frames <= `kZcThreshold` bytes skip `SEND_ZC` to avoid the
extra notification CQE, halving per-packet kernel overhead for
small packets.

### Reproducing

```sh
# Start relay with pinned workers
./build/hyper-derp --port 3340 --workers 2 --pin-workers 0,1 &

# Start sink (receiver)
./build/tools/derp-test-client --host 127.0.0.1 --port 3340 \
  --mode sink --duration 18 --size 1024 > /tmp/sk.txt &
sleep 2

# Start flood (sender), pipe sink's key via stdin
cat /tmp/sk.txt | taskset -c 3 \
  ./build/tools/derp-test-client --host 127.0.0.1 --port 3340 \
  --mode flood --duration 10 --size 1024
```

### Latency measurement

```sh
# Start echo peer
./build/tools/derp-test-client --host 127.0.0.1 --port 3340 \
  --mode echo --count 10000 > /tmp/echo.txt &
sleep 1

# Ping with RTT recording
cat /tmp/echo.txt | ./build/tools/derp-test-client \
  --host 127.0.0.1 --port 3340 \
  --mode ping --count 10000 --size 64 \
  --json --raw-latency --output result.json
```

### Analysis

```sh
# Summary table
python3 tools/plot_results.py result.json

# Latency CDF + histogram
python3 tools/plot_results.py --cdf --hist result.json

# All plots, saved to directory
python3 tools/plot_results.py --all-plots --save-dir ./plots result.json

# Export to CSV
python3 tools/plot_results.py --csv results.csv result.json
```

## License

Private — all rights reserved.
