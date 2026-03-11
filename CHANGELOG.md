# Changelog

All notable changes to this project will be documented in
this file. Format based on [Keep a Changelog](https://keepachangelog.com/).

## [0.1.0] - 2026-03-11

### Added
- io_uring data plane with SEND_ZC, provided buffer rings,
  multishot recv, SINGLE_ISSUER + DEFER_TASKRUN
- DERP wire protocol (Tailscale-compatible): ServerKey,
  ClientInfo, ServerInfo, SendPacket, RecvPacket, KeepAlive,
  Ping, Pong
- NaCl box handshake with Curve25519 (libsodium)
- HTTP/1.1 upgrade path with probe and generate_204 endpoints
- Sharded worker architecture with cross-shard MPSC forwarding
- Control plane with peer presence tracking and watcher
  notifications
- Crow-based metrics server: /metrics (Prometheus), /health
  (JSON), /debug/workers, /debug/peers
- TLS support for metrics endpoints
- Debian packaging with systemd service (hardened unit file)
- CLI: --port, --workers, --pin-workers, --sockbuf,
  --metrics-port, --tls-cert, --tls-key, --log-level,
  --debug-endpoints, --max-accept-rate, --help, --version
- Timing-safe key comparison (sodium_memcmp)
- Async-signal-safe shutdown (atomic flag + poller thread)
- Input validation on all CLI arguments
- Rate-limited handshake failure warnings
- Accept loop connection rate limiting (--max-accept-rate)
- Debug endpoint gating (--debug-endpoints required)
- libFuzzer targets for HTTP parser and protocol codec
- 60 unit + integration tests
- Scale test tool with configurable rate limiting
- Benchmark visualization (plot_results.py)
