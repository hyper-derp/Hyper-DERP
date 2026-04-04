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
4 client VMs, 20 peers, 10 pairs, 1400-byte messages.
20 runs per data point, 95% CIs.
Go derper v1.96.4 release build.

### Throughput (HD kTLS vs TS TLS)

| vCPU | HD Peak | TS Ceiling | Ratio | HD Loss | TS Loss |
|-----:|--------:|-----------:|------:|--------:|--------:|
| 2 | 3,730 Mbps | 1,870 Mbps | **10.8x** at 5G | 1.7% | 92% |
| 4 | 6,091 Mbps | 2,798 Mbps | **3.5x** at 7.5G | 2.0% | 74% |
| 8 | 12,316 Mbps | 4,670 Mbps | **2.7x** at 15G | 0.7% | 44% |
| 16 | 16,545 Mbps | 7,834 Mbps | **2.1x** at 25G | 1.5% | 16% |

The advantage grows as resources shrink. At 2 vCPU, TS drops
92% of offered traffic at 5 Gbps while HD delivers 3.7 Gbps.

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
./build/hyper-derp --config dist/hyper-derp.yaml
```

## Install (Debian)

```sh
cmake --build build --target package
sudo dpkg -i build/hyper-derp_*.deb
```

This installs the binary to `/usr/bin/`, an example config
to `/etc/hyper-derp/hyper-derp.yaml`, and a systemd unit.
The postinst enables the service and loads the `tls` kernel
module.

```sh
# Edit config
sudo vi /etc/hyper-derp/hyper-derp.yaml

# Start
sudo systemctl start hyper-derp

# Check status
sudo systemctl status hyper-derp
journalctl -u hyper-derp -f
```

## Configuration

Hyper-DERP reads a YAML config file and accepts CLI flag
overrides. CLI flags take precedence over file values.

```sh
# Config file only
hyper-derp --config /etc/hyper-derp/hyper-derp.yaml

# Config file + CLI override
hyper-derp --config /etc/hyper-derp/hyper-derp.yaml \
  --port 443 --workers 4
```

Example config (`/etc/hyper-derp/hyper-derp.yaml`):

```yaml
port: 3340
workers: 0              # 0 = auto (one per core)
# pin_cores: [0, 2, 4, 6]
sqpoll: false

# kTLS — both required to enable
# tls_cert: /etc/hyper-derp/cert.pem
# tls_key: /etc/hyper-derp/key.pem

log_level: info

metrics:
  # port: 9100
  debug_endpoints: false
```

### kTLS Prerequisites

```sh
sudo modprobe tls
lsmod | grep tls
```

HD auto-detects kTLS support via OpenSSL. Without the `tls`
kernel module loaded, OpenSSL falls back to userspace TLS
silently. To persist across reboots:

```sh
echo tls | sudo tee /etc/modules-load.d/tls.conf
```

### Worker Count Guidance

- **With kTLS (production):** use default (vCPU / 2). More
  workers means more parallel crypto throughput.
- **Without TLS (testing only):** cap at 4 workers.

### systemd

The packaged service unit runs as:

```
ExecStart=/usr/bin/hyper-derp --config /etc/hyper-derp/hyper-derp.yaml
```

Hardened with `DynamicUser`, `ProtectSystem=strict`,
`NoNewPrivileges`, and a restricted syscall filter.
`CAP_SYS_NICE` is granted for SQPOLL mode and
`LimitMEMLOCK=infinity` for io_uring buffer rings.

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
(`/etc/hyper-derp/hyper-derp.yaml`), and binaries to
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
