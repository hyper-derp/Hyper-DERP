# Hyper-DERP

High-performance DERP relay server written in C++23 with
`io_uring`. Drop-in replacement for Tailscale's Go-based
[derper](https://pkg.go.dev/tailscale.com/derp) with 2-12x
higher throughput, up to 48x fewer TCP retransmits, and
significantly lower tail latency under load.

Compatible with Tailscale and Headscale clients.

## Benchmark Highlights

Measured on GCP c4-highcpu VMs (Intel Xeon Platinum 8581C),
DERP over kTLS, 25 runs per data point.
Go derper v1.96.1 release build.

### Throughput (HD kTLS vs TS TLS)

| vCPU | HD Peak | TS Ceiling | Ratio | HD Loss | TS Loss |
|-----:|--------:|-----------:|------:|--------:|--------:|
| 2 | 2,977 Mbps | 1,448 Mbps | **11.8x** at 5G | 2.8% | 93.0% |
| 4 | 5,106 Mbps | 2,395 Mbps | **3.7x** at 7.5G | 0.8% | 74.2% |
| 8 | 7,366 Mbps | 3,802 Mbps | **1.9x** at 10G | 0.4% | 46.1% |
| 16 | 12,068 Mbps | 7,743 Mbps | **1.6x** at 20G | 1.7% | 18.9% |

The advantage grows as resources shrink. At 2 vCPU, TS drops
93% of offered traffic at 5 Gbps. HD delivers 3 Gbps.

Loss percentages are measured at the offered rate shown. HD has
zero internal packet drops at all configurations (no xfer_drops,
send_drops, or slab_exhausts). Reported loss reflects messages
in TCP send buffers when the 15-second test window closes — the
gap between offered rate and relay throughput, not relay packet
loss.

### Tail Latency Under Load (p99, at TS TLS ceiling)

| vCPU | HD p99 | TS p99 | Ratio |
|-----:|-------:|-------:|------:|
| 2 | 554 us | 2,718 us | **4.9x** |
| 4 | 1,601 us | 2,512 us | **1.6x** |
| 8 | 1,097 us | 1,408 us | **1.3x** |
| 16 | 636 us | 1,268 us | **2.0x** |

### Real Tailscale Tunnel Test

End-to-end through WireGuard tunnels with real Tailscale clients
and self-hosted Headscale control plane. 10 runs, 15s each.

| Metric | Go derper | Hyper-DERP | Ratio |
|--------|----------:|-----------:|------:|
| Throughput (1 pair) | 988 Mbps | 1,682 Mbps | **1.70x** |
| Retransmits (1 pair) | 9,184 | 560 | **16x fewer** |
| Retransmits (3 pairs) | 9,566 | 399 | **24x fewer** |
| Retransmits (5 min) | 192,689 | 3,956 | **48x fewer** |
| Throughput under churn | 1,007 Mbps | 1,730 Mbps | **1.72x** |

HD retransmits *decrease* under load — io_uring batched sends
coalesce into smoother TCP flow. TS retransmits stay constant
regardless of load.

### Cross-Cloud (AWS c7i, eu-central-1)

| vCPU | GCP HD/TS | AWS HD/TS |
|-----:|----------:|----------:|
| 8 | 2.0x | 2.0-2.2x |
| 16 | 1.6x | 1.3x |

The advantage is architectural, not platform-specific.

## Architecture

Three-layer design:

```
Accept Thread
  TCP accept → kTLS handshake → HTTP upgrade → NaCl box
  → assign peer to data plane shard (FNV-1a hash)

Data Plane (io_uring workers, one per shard)
  Multishot recv with provided buffer rings
  SPSC cross-shard transfer rings (lock-free)
  Batched eventfd signaling (one per dest per CQE batch)
  P3 bitmask: O(active sources) ProcessXfer, not O(N-1)
  SEND_ZC for frames >4KB, regular send for WireGuard MTU
  MSG_MORE coalescing on first-send
  Backpressure: pause recv when send queues exceed threshold

Control Plane (single-threaded, epoll)
  Ping/pong, watcher notifications, peer presence
  Fully isolated from data plane — no shared locks
```

Data-oriented design: plain structs, free functions, no virtual
dispatch, cache-friendly field ordering, zero allocation on the
hot path (~70 MB pre-allocated per worker).

### Why It's Faster

1. **io_uring vs goroutines.** Batched I/O submission amortizes
   syscall overhead. One `io_uring_enter` handles dozens of
   pending sends. Go derper makes one `write` syscall per packet
   per goroutine.

2. **kTLS vs userspace TLS.** AES-GCM encryption runs in the
   kernel. Worker threads never touch crypto. Go's `crypto/tls`
   encrypts in userspace per goroutine, competing for CPU.

3. **Sharded workers vs shared state.** Each worker owns a
   disjoint peer set. Cross-shard forwarding uses lock-free
   SPSC rings. Go derper's goroutines contend on shared maps.

4. **Send coalescing.** Batched io_uring submissions produce
   smoother TCP flow — fewer congestion events, fewer
   retransmits. This is why retransmits *decrease* under load.

## Dependencies

System packages (Debian/Ubuntu):

```sh
sudo apt install \
  clang cmake ninja-build \
  liburing-dev libsodium-dev libspdlog-dev \
  libgtest-dev libgmock-dev libcli11-dev
```

Requires Linux with `io_uring` support (kernel 6.1+ recommended
for DEFER_TASKRUN and SINGLE_ISSUER).

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

# Start relay (auto-detect worker count, kTLS)
./build/hyper-derp --port 443 \
  --cert /path/to/cert.pem --key /path/to/key.pem

# Explicit worker count and CPU pinning
./build/hyper-derp --port 443 --workers 4 --pin-workers 0-3 \
  --cert cert.pem --key key.pem

# With metrics endpoint (separate plain HTTP port)
./build/hyper-derp --port 443 --workers 4 \
  --cert cert.pem --key key.pem \
  --metrics-port 9090 --debug-endpoints
```

### kTLS Prerequisites

```sh
# Load kernel TLS module (required before starting)
sudo modprobe tls

# Verify
lsmod | grep tls
```

HD auto-detects kTLS support via OpenSSL. Check startup log for
`BIO_get_ktls_send` confirmation. Without the TLS module loaded,
OpenSSL falls back to userspace TLS silently.

### Worker Count Guidance

- **With kTLS (production):** use default (vCPU / 2). More
  workers = more parallel crypto throughput. 8 workers on 16
  vCPU outperforms 4 workers under kTLS.
- **Without TLS (testing only):** cap at 4 workers. Higher
  counts cause cross-shard backpressure oscillation under
  extreme load.

## Testing

```sh
# All tests
ctest --preset debug

# Unit tests (protocol, HTTP, handshake)
ctest --preset debug -R unit_tests

# Integration tests (fork-based, process-isolated)
ctest --preset debug -R integration_tests
```

## Packaging

```sh
cmake --build build --target package
# Produces build/hyper-derp_<version>_<arch>.deb
sudo dpkg -i build/hyper-derp_*.deb
```

Installs systemd unit (`hyper-derp.service`), example config
(`/etc/hyper-derp/hyper-derp.conf.example`), and binaries to
`/usr/bin/`.

See [OPERATIONS.md](OPERATIONS.md) for production tuning
(sysctl, CPU pinning, memory footprint, Prometheus alerting).

## Benchmark Methodology

All benchmark data collected with:
- 25 runs per high-rate data point, 5 at low rates
- 95% confidence intervals (t-distribution)
- TS ceiling probes to scale latency loads per config
- Strict isolation (one server at a time, cache drops)
- Go derper: v1.96.1 release build (-trimpath, stripped)
- Tunnel tests: Headscale control plane, zero Tailscale contact
- Both GCP and AWS to confirm cross-cloud consistency

Full reports: `bench_results/gcp-c4-phase-b/REPORT.md`,
`bench_results/aws-c7i-phase-b/REPORT.md`

## License

MIT
