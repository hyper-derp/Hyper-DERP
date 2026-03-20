# Hyper-DERP

High-performance DERP relay server in C++23 with `io_uring`.

![Build](https://github.com/hyper-derp/hyper-derp/actions/workflows/build.yml/badge.svg)
![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)
![Platform: Linux](https://img.shields.io/badge/Platform-Linux-lightgrey.svg)

## What Is This?

[DERP](https://tailscale.com/blog/how-tailscale-works/) (Designated
Encrypted Relay for Packets) is the relay protocol that Tailscale
clients fall back to when direct WireGuard connections fail.
Hyper-DERP is a drop-in replacement for Tailscale's Go-based
[derper](https://pkg.go.dev/tailscale.com/derp) that delivers
2-12x higher throughput, up to 48x fewer TCP retransmits, and
significantly lower tail latency under load. It is compatible
with Tailscale, Headscale, and any standard DERP client.

## Performance

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
93% of offered traffic at 5 Gbps while HD delivers 3 Gbps.

### Tail Latency (p99, at TS TLS ceiling)

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

HD retransmits *decrease* under load -- io_uring batched sends
coalesce into smoother TCP flow.

### Cross-Cloud (AWS c7i)

| vCPU | GCP HD/TS | AWS HD/TS |
|-----:|----------:|----------:|
| 8 | 2.0x | 2.0-2.2x |
| 16 | 1.6x | 1.3x |

The advantage is architectural, not platform-specific.
Full benchmark reports are available at
[hyper-derp.dev/benchmarks](https://hyper-derp.dev/benchmarks/).

## Architecture

Three-layer design with complete isolation between planes:

```
Accept Thread
  TCP accept -> kTLS handshake -> HTTP upgrade -> NaCl box
  -> assign peer to data plane shard (FNV-1a hash)

Data Plane (io_uring workers, one per shard)
  Multishot recv with provided buffer rings
  SPSC cross-shard transfer rings (lock-free)
  Batched eventfd signaling
  SEND_ZC for large frames, MSG_MORE coalescing
  Backpressure: pause recv when send queues full

Control Plane (single-threaded, epoll)
  Ping/pong, watcher notifications, peer presence
  Fully isolated from data plane -- no shared locks
```

Data-oriented design: plain structs, free functions, no virtual
dispatch, cache-friendly field ordering, zero allocation on the
hot path (~70 MB pre-allocated per worker).

**Why it's faster:** io_uring batched I/O vs per-packet syscalls,
kTLS kernel-level AES-GCM vs userspace crypto, sharded workers
vs shared-map contention, and send coalescing for smoother TCP
flow. See [docs/architecture.md](docs/architecture.md) for
details.

## Quick Start

```sh
cmake --preset default
cmake --build build -j
sudo modprobe tls
./build/hyper-derp --port 443 \
  --cert /path/to/cert.pem --key /path/to/key.pem
```

## Install from APT

```sh
# Add GPG key
curl -fsSL https://hyper-derp.dev/repo/key.gpg | \
  sudo gpg --dearmor -o /usr/share/keyrings/hyper-derp.gpg

# Add repository
echo "deb [signed-by=/usr/share/keyrings/hyper-derp.gpg] \
  https://hyper-derp.dev/repo stable main" | \
  sudo tee /etc/apt/sources.list.d/hyper-derp.list

# Install
sudo apt update && sudo apt install hyper-derp
```

## Usage

```sh
# Show all options
./build/hyper-derp --help

# Auto-detect worker count and kTLS
./build/hyper-derp --port 443 \
  --cert /path/to/cert.pem --key /path/to/key.pem

# Explicit worker count and CPU pinning
./build/hyper-derp --port 443 --workers 4 --pin-workers 0-3 \
  --cert cert.pem --key key.pem

# With metrics endpoint
./build/hyper-derp --port 443 --workers 4 \
  --cert cert.pem --key key.pem \
  --metrics-port 9090 --debug-endpoints
```

### kTLS Prerequisites

```sh
sudo modprobe tls
lsmod | grep tls
```

HD auto-detects kTLS support via OpenSSL. Without the `tls`
kernel module loaded, OpenSSL falls back to userspace TLS
silently.

### Worker Count Guidance

- **With kTLS (production):** use default (vCPU / 2). More
  workers means more parallel crypto throughput.
- **Without TLS (testing only):** cap at 4 workers.

## Configuration

See [OPERATIONS.md](OPERATIONS.md) for production tuning:
sysctl settings, CPU pinning, memory footprint, and Prometheus
alerting.

## Compatibility

Hyper-DERP implements the Tailscale DERP wire protocol and is
compatible with:

- **Tailscale** clients (all platforms)
- **Headscale** self-hosted control planes
- Any client that speaks the standard DERP protocol

## Building

### Dependencies

System packages (Debian/Ubuntu):

```sh
sudo apt install \
  clang cmake ninja-build \
  liburing-dev libsodium-dev libspdlog-dev \
  libgtest-dev libgmock-dev libcli11-dev
```

Requires Linux with `io_uring` support (kernel 6.1+
recommended for DEFER_TASKRUN and SINGLE_ISSUER).

### Build Commands

```sh
# Release
cmake --preset default
cmake --build build -j

# Debug
cmake --preset debug
cmake --build build-debug -j
```

### ARM64 Cross-Compile

```sh
sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
cmake -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake \
  -B build-arm64
cmake --build build-arm64 -j
```

Targets AWS Graviton and Ampere (Oracle/Azure) instances.
io_uring is architecture-independent on kernel 6.1+.

### Packaging

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
# All tests
ctest --preset debug

# Unit tests only
ctest --preset debug -R unit_tests

# Integration tests (fork-based, process-isolated)
ctest --preset debug -R integration_tests
```

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for build instructions,
code style requirements, and the PR process.

## Benchmark Methodology

All benchmark data collected with:
- 25 runs per high-rate data point, 5 at low rates
- 95% confidence intervals (t-distribution)
- Strict isolation (one server at a time, cache drops between)
- Go derper v1.96.1 release build (-trimpath, stripped)
- Tunnel tests: Headscale control plane, zero Tailscale contact
- Both GCP and AWS to confirm cross-cloud consistency

Full reports at
[hyper-derp.dev/benchmarks](https://hyper-derp.dev/benchmarks/).

## License

[MIT](LICENSE)
