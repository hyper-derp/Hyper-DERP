# Changelog

All notable changes to this project will be documented in
this file. Format based on [Keep a Changelog](https://keepachangelog.com/).

## [0.2.1] - unreleased

### Added — wg-relay hardening

- **WG-shape filter at XDP**: drops packets whose first byte
  isn't a WireGuard message type (1/2/3/4) or whose length
  doesn't match the type. Fires before the source-IP lookup,
  so non-WG noise on the relay's port stops at the NIC and
  doesn't pollute `drop_unknown_src`. New counter
  `drop_not_wg_shaped`.
- **MAC1 verification for handshakes**: when both ends of a
  link have `wg peer pubkey` stamped, every handshake init
  / response from a registered peer is verified against the
  partner's pubkey via Blake2s-keyed MAC1. Mismatch drops
  with `drop_handshake_pubkey_mismatch`. Catches misconfigured
  clients (wrong relay, NAT collisions, stale endpoint reuse).
  Engages only when the partner pubkey is set, so existing
  operators keep today's behaviour exactly.
- **Automatic peer roaming** (`mode: wireguard`): a peer's
  endpoint auto-updates when their IP changes. Handshake from
  an unknown source is matched against every partner's pubkey
  via MAC1 to identify which peer it's from; a candidate
  endpoint is registered. The committed endpoint stays put
  until transport-data from the candidate confirms the roam,
  at which point the new endpoint is committed, the BPF map
  is refreshed (XDP fast path picks up the new endpoint), and
  the roster is persisted. New per-peer counter
  `endpoint_relearn`. Forged handshakes (attacker who knows
  the pubkey but lacks the private key) tick
  `drop_relearn_unconfirmed` and never commit.
- **Dynamic source-IP blocklist**: source IPs that produce
  repeated failed-confirm strikes (forged handshakes that
  never progressed to transport data) escalate onto a BPF
  blocklist. Defaults: 2 strikes / 60 s → 60 s block;
  5 / 1 h → 1 h block; 10 / 24 h → 24 h block. Blocked
  sources drop at the top of XDP. Closes the relay-as-
  anonymizer attack against the partner. New verb
  `wg blocklist list`. New counters `drop_blocklisted` (XDP)
  and per-IP strike records.
- **Endpoint-hijack defense**: a forged handshake init must receive a partner-attributable response (the partner's type-2 `receiver_index` matches the init's `sender_index`) before the candidate slot is allowed to confirm. A forger who can pass MAC1 but lacks the static-key handshake can no longer bounce the candidate to confirm by sending a matching-shaped transport-data packet of their own.
- **Type-2 from unknown source dropped outright**: legitimate handshake responses come from the committed responder endpoint, so an unknown-source type-2 has no place in the protocol and was an unauthenticated amplifier surface.
- **Retry-init forward rate-limit**: while a candidate is unconfirmed, the no-op-forward branch caps retry forwards at one per second per source. Legitimate `wg.ko` retries every 5 s; a flood of forged retries is clamped and then strikes into the blocklist.
- **Strike-table sweep**: stale strike entries (older than the widest policy window) are pruned during candidate expiry so spoofed-source one-shot strikes can't grow the table without bound.

### Added — Crypto

- **Standalone Blake2s** in `src/crypto/`. RFC 7693 reference
  port; libsodium ships Blake2b only and WireGuard's MAC1
  uses Blake2s specifically. Verified against the published
  "abc" / empty-input vectors.

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
- **`hdcli` operator CLI** (bundled in the deb): driven
  via einheit's REPL with branded chafa banner. Add /
  remove peers and links, inspect counters, render
  ready-to-paste `[Peer]` blocks for new clients.

### Added — Packaging
- **einheit operator CLI baked into the deb** via CMake
  FetchContent. `apt install hyper-derp` is now fully
  functional out of the box: einheit binary, `hdcli`
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
- **`hdwg` daemon**: client-side WireGuard driver.
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
  building, hdwg). Internal design write-ups moved to
  `docs/design/`. Benchmark reports relocated to the
  separate `hyper-derp/HD.Benchmark` repo.
- **WG-relay quickstart slimmed** to a copy-paste
  homelab walkthrough — XDP / cloud / performance
  material moved out to the bench repo.
- `hdwg` flow: configure wg.ko with the direct endpoint
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
  improvement): `hdwg` no longer blocks on
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
- `hdwg` direct: 0.6 ms LAN RTT
- `hdwg` relayed: 1.5 ms RTT (same-host VMs, TLS)

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
