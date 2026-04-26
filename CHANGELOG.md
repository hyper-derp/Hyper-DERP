# Changelog

All notable changes to this project will be documented in
this file. Format based on [Keep a Changelog](https://keepachangelog.com/).

## [0.2.0] - 2026-04-26

### Added — WireGuard Relay Mode
- **`mode: wireguard`**: transparent UDP relay for stock
  WireGuard clients. Two NAT'd peers point `Endpoint =`
  at hyper-derp; the relay forwards based on an
  operator-managed peer + link table. No WG semantics in
  the relay — works with `wg`, `wg-quick`, pfSense, the
  mobile WireGuard apps, with no client-side changes.
- **XDP fast path**: optional kernel-level forwarding via
  attached BPF program. Single-NIC `XDP_TX` for one-link
  topologies, dual-NIC `XDP_REDIRECT` for separated
  ingress/egress. Bare-metal 25 GbE: 10.4 Gbit/s
  single-peer TCP. GCP n2-standard-4 with gVNIC GQI:
  3.72 Gbit/s.
- **`hd-cli` operator CLI** (bundled in the deb): driven
  via einheit's REPL with branded chafa banner. Add /
  remove peers and links, inspect counters, render
  ready-to-paste `[Peer]` blocks for new clients.

### Added — Packaging
- **einheit operator CLI baked into the deb** via CMake
  FetchContent. `apt install hyper-derp` is now fully
  functional out of the box: einheit binary, `hd-cli`
  wrapper, chafa-branded REPL, vendored systemd drop-in
  with the WG/XDP capability bits, `/tmp/einheit/` IPC
  dir, kernel modules autoloaded by postinst.

### Added — HD Protocol
- HD Protocol: native relay with connection-time auth,
  zero per-packet header rewrite
- MeshData: 1:N selective routing with 2-byte peer IDs
- FleetData: cross-relay routing with 2-byte relay +
  peer IDs (4.3B addressable endpoints)
- Relay table with distance-vector route announcements
- Cross-relay fleet linking (`--hd-relay-id`,
  `--hd-seed-relay`), with PeerInfo forwarding so tunnels
  cross seed boundaries transparently
- DERP-to-HD bridge for mixed networks
- **HD SDK** (`sdk/`): `hd::sdk::Client` / `Tunnel` event-
  driven API (move-only pimpl, thread-safe callbacks,
  own-or-external I/O thread), zero-copy frame pool, C ABI
  wrapper
- **SDK extensions**: `hd_wg` (WireGuard integration,
  netlink, proxy, state machine), `hd_ice` (NAT traversal),
  `hd_bridge` (TCP/unix ↔ tunnel), `hd_policy`
  (header-only routing intent), `hd_fleet` (topology view)
- **`hd-wg` daemon**: client-side WireGuard driver.
  Signals via HD MeshData (WGEX / CAND / FALL), tries
  direct UDP first, falls back to a local `wg.ko ↔ HD`
  proxy when direct fails. `--force-relay` for
  deterministic relay mode. Runtime direct→relay
  fallback on stalled tunnels (~15s detection via
  `WgNlGetPeerStats`; peer reset + `FALL` signal to the
  remote end to rekey on the new path)
- **`hdcat`**: netcat/socat for HD tunnels. TCP/UDP/unix-
  socket, stdin-stdout, YAML config, wildcard peer names
- **`hdctl`**: ZMQ IPC control CLI for the relay, and a
  YAML-driven bridge runner
- REST API: /api/v1/peers, /api/v1/relay
- STUN/TURN/ICE for Level 2 direct path
- XDP STUN binding + TURN channel forwarding
- AF_XDP dual-port relay (24.6 Gbps, 98% line rate)
- HD scale test tools (pthread + io_uring variants)
- UDP blaster for AF_XDP benchmarking

### Changed
- **Docs reorganised**: `docs/` is operator-facing only
  (quickstart, CLI handbook, configuration, packaging,
  building, hd-wg). Internal design write-ups moved to
  `docs/design/`. Benchmark reports relocated to the
  separate `hyper-derp/HD.Benchmark` repo.
- **WG-relay quickstart slimmed** to a copy-paste
  homelab walkthrough — XDP / cloud / performance
  material moved out to the bench repo.
- `hd-wg` flow: configure wg.ko with the direct endpoint
  *first* (never the proxy), so WG's roaming doesn't
  latch onto `127.0.0.1:<proxy>` before ICE can run.
  Fall back to proxy only on 5s ICE timeout or 500ms
  "no candidates".
- Direct-path promotion verified via fresh WG handshake
  (`WgNlGetPeerHandshake`) instead of the previous 3s
  unconditional timer, so iptables-blocked paths no
  longer show as "promoted to direct" while silently
  failing.

### Fixed
- HD relay-path RTT from ~100ms to ~1.5ms (65×
  improvement): `hd-wg` no longer blocks on
  `SO_RCVTIMEO` after draining the client receive
  buffer; poll wakes it on new TCP data instead.
- `WgNlGetPeerHandshake` parsed netlink attributes
  order-dependently and missed `LAST_HANDSHAKE_TIME`
  when it arrived before `PUBLIC_KEY`; now
  order-independent.
- GET_DEVICE netlink dump interleaved with in-flight
  SET_DEVICE ACKs on the shared socket (returned EPROTO);
  uses a dedicated socket per query.

### Performance
- HD Protocol: 19,880 Mbps (2.55x Go derper)
- AF_XDP relay: 24,600 Mbps (3.15x Go derper, zero loss)
- HD beats DERP by 6.5% with 37% less loss at saturation
- Relay at 3% CPU forwarding 6 Gbps (client-bottlenecked)
- `hd-wg` direct: 0.6 ms LAN RTT
- `hd-wg` relayed: 1.5 ms RTT (same-host VMs, TLS)

## [0.1.5] - 2026-04-13

### Fixed
- ARM64 .deb was named _amd64.deb due to dpkg
  --print-architecture running on the host, not the
  cross-compile target. Now uses CMAKE_SYSTEM_PROCESSOR.

## [0.1.4] - 2026-04-13

### Added
- ARM64 .deb package in release pipeline (amd64 + arm64
  built in parallel, both attached to GitHub release)

## [0.1.3] - 2026-04-13

### Fixed
- SVG logos: removed mm units causing clipping in some
  browsers
- Data accuracy: corrected latency percentages, removed
  unsourced context switch claim from landing page, fixed
  stale libspdlog-dev in build docs
- Blog: μs units, em dashes, publish date set to 2026-04-15

### Changed
- Split configuration docs into practical guide and full
  CLI/config reference page
- Finalized project logo across site, favicon, and README
- Removed architecture section from README (covered on site)

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
- 30-41% lower p99 latency at 8-16 vCPU under load
  (HD 127-153 μs vs TS 214-218 μs at 150% of TS ceiling)
- Peer-count invariant: HD flat at 100 peers, TS -38%
- Tunnel quality: identical ~2 Gbps WireGuard throughput,
  7-8% fewer TCP retransmits at max load
