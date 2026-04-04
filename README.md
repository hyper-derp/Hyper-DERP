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
2-10x higher throughput and 40% lower tail latency under load. It is compatible
with Tailscale, Headscale, and any standard DERP client.

## Performance

Measured on GCP c4-highcpu VMs (Intel Xeon Platinum 8581C),
4 client VMs, 20 peers, 10 pairs, 1400-byte messages.
20 runs per data point, 95% CIs.
Go derper v1.96.4 release build.

### Throughput (HD kTLS vs TS TLS)

| vCPU | HD Peak | TS Ceiling | Ratio | HD Loss | TS Loss |
|-----:|--------:|-----------:|------:|--------:|--------:|
| 2 | 3,730 Mbps | 1,870 Mbps | **10.8x** | 1.65% | 92% |
| 4 | 6,091 Mbps | 2,798 Mbps | **3.5x** | 1.97% | 74% |
| 8 | 12,316 Mbps | 4,670 Mbps | **2.7x** | 0.68% | 44% |
| 16 | 16,545 Mbps | 7,834 Mbps | **2.1x** | 1.51% | 17% |

The advantage grows as resources shrink. At 2 vCPU, TS drops
92% of offered traffic at 5 Gbps while HD delivers 3.7 Gbps.

### Tail Latency (p99 at 150% of TS ceiling)

Per-packet DERP relay RTT, 5,000 pings per run, 10 runs
per load level. 2,160,000 total latency samples.

| vCPU | HD p99 | TS p99 | HD advantage |
|-----:|-------:|-------:|-------------:|
| 8 | 153 us | 218 us | **1.42x** |
| 16 | 127 us | 214 us | **1.69x** |

HD p99 is load-invariant (129-153 us from idle through
150% load at 8 vCPU). TS p99 rises from 129 to 218 us
(+69%). At 4 vCPU, HD has a known backpressure stall --
see the [full report](https://github.com/hyper-derp/HD.Benchmark).

### Peer Scaling (8 vCPU, 10G offered)

| Peers | HD (Mbps) | TS (Mbps) | HD/TS |
|------:|----------:|----------:|------:|
| 20 | 8,371 | 4,495 | 1.9x |
| 100 | 7,665 | 2,775 | **2.8x** |

HD throughput is peer-count invariant. TS loses 38% going
from 20 to 100 peers (goroutine scheduling overhead). The
ratio amplifies from 1.9x to 2.8x.

### Tunnel Quality (WireGuard through DERP)

End-to-end through WireGuard tunnels with Tailscale clients
and Headscale control plane. iperf3 UDP + TCP + ping,
20 runs per data point, 720 total runs.

| Config | HD UDP @ 8G | TS UDP @ 8G | HD Retx | TS Retx |
|--------|------------:|------------:|--------:|--------:|
| 4 vCPU | 2,100 Mbps | 2,115 Mbps | 4,852 | 5,217 |
| 8 vCPU | 2,053 Mbps | 2,060 Mbps | 4,552 | 4,484 |
| 16 vCPU | 2,059 Mbps | 2,223 Mbps | 4,291 | 4,617 |

Both relays deliver identical tunnel throughput (~2 Gbps),
limited by WireGuard userspace crypto (wireguard-go,
ChaCha20-Poly1305), not the relay. HD produces 7-8% fewer
TCP retransmits at max load on 4 and 16 vCPU.

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

4,903 total runs (3,703 throughput + 480 latency + 720 tunnel):
- 20 runs per data point, 95% confidence intervals (Welch's t)
- Strict isolation (one server at a time, cache drops between)
- Go derper v1.96.4 release build (-trimpath, stripped)
- Latency: 5,000 pings per run, 2.16M total samples
- Tunnel: iperf3 UDP + TCP + ping through WireGuard/Headscale

Full report and raw data at
[HD.Benchmark](https://github.com/hyper-derp/HD.Benchmark).

## License

[MIT](LICENSE)
