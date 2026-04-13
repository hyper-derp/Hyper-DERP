<p align="center">
  <img src="graphics/hyper-derp.svg" alt="Hyper-DERP" width="600" />
</p>

<h1 align="center">Hyper-DERP</h1>

<p align="center">High-performance DERP relay server in C++23 with <code>io_uring</code>.</p>

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

2-10x throughput, 40% lower tail latency, half the
hardware. 4,903 benchmark runs on GCP c4-highcpu VMs
against Go derper v1.96.4.

Full results: [HD.Benchmark](https://github.com/hyper-derp/HD.Benchmark)
| [hyper-derp.dev/benchmarks](https://hyper-derp.dev/benchmarks/)

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
  liburing-dev libsodium-dev \
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

## License

[MIT](LICENSE)
