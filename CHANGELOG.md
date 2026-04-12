# Changelog

All notable changes to this project will be documented in
this file. Format based on [Keep a Changelog](https://keepachangelog.com/).

## [0.1.2] - 2026-04-12

### Fixed
- Version string now derived from CMake PROJECT_VERSION
  instead of hardcoded in main.cc

## [0.1.1] - 2026-04-12

### Fixed
- Statically link spdlog and fmt to fix shared library
  mismatch across distros (Ubuntu vs Debian soname
  incompatibility)
- Remove libspdlog runtime dependency from .deb package

### Changed
- APT repository at https://hyper-derp.dev/repo/ with signed
  Release metadata (Origin, Suite, Codename, Architectures)
- Site deployed to https://hyper-derp.dev via GitHub Pages

## [0.1.0] - 2026-04-04

Initial release. Drop-in replacement for Tailscale's
Go-based derper.

### Added
- io_uring data plane with SEND_ZC, provided buffer rings,
  multishot recv, SINGLE_ISSUER + DEFER_TASKRUN
- DERP wire protocol (Tailscale-compatible): ServerKey,
  ClientInfo, ServerInfo, SendPacket, RecvPacket, KeepAlive,
  Ping, Pong
- NaCl box handshake with Curve25519 (libsodium)
- HTTP/1.1 upgrade path with probe and generate_204
  endpoints
- kTLS — kernel handles AES-GCM encrypt/decrypt,
  offloadable to NIC hardware
- Sharded worker architecture with cross-shard SPSC
  forwarding and lock-free rings
- Control plane with peer presence tracking and watcher
  notifications
- Backpressure via recv pause with adaptive hysteresis
  (peer-count-scaled thresholds, minimum pause duration)
- YAML config file support (rapidyaml) with CLI overrides
- Crow-based metrics server: /metrics (Prometheus),
  /health (JSON), /debug/workers, /debug/peers
- Debian packaging with systemd service (hardened unit
  file, DynamicUser, restricted syscall filter)
- CLI: --config, --port, --workers, --pin-workers,
  --sockbuf, --metrics-port, --tls-cert, --tls-key,
  --log-level, --debug-endpoints, --max-accept-rate,
  --sqpoll, --help, --version
- CI/CD: GitHub Actions pipeline (test, lint, package,
  release), ARM64 cross-compile
- 80 unit + integration tests
- Benchmark tools: derp-test-client, derp-scale-test
  (io_uring and pthread variants), derp-tun-proxy
- libFuzzer targets for HTTP parser and protocol codec

### Performance (GCP c4-highcpu, kTLS vs TS TLS)
- 2 vCPU: 3,730 Mbps (HD) vs 1,870 Mbps (TS), 10.8x
- 4 vCPU: 6,091 Mbps vs 2,798 Mbps, 3.5x
- 8 vCPU: 12,316 Mbps vs 4,670 Mbps, 2.7x
- 16 vCPU: 16,545 Mbps vs 7,834 Mbps, 2.1x
- HD sub-2% loss at all configs; TS 16-92% at saturation
- 40% lower p99 latency at 8-16 vCPU under load
  (HD 127-153 us vs TS 214-218 us at 150% of TS ceiling)
- Peer-count invariant: HD flat at 100 peers, TS -38%
- Tunnel quality: identical ~2 Gbps WireGuard throughput,
  7-8% fewer TCP retransmits at max load
