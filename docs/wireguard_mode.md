# WireGuard Relay Mode

## Status

Design. Not implemented. Specifies a second operating mode
for Hyper-DERP that relays vanilla WireGuard UDP traffic
between pre-configured peers. Interoperates with any
conforming WireGuard implementation — pfSense, `wg-quick`,
Windows WireGuard, Android WireGuard, etc. — without a
client-side shim.

## Scope clarification

Hyper-DERP has two distinct use cases that must not be
conflated:

1. **HD-SDK / otc.relay (existing DERP mode).**
   Uses the Hyper-DERP custom protocol packaged in
   TCP + TLS, HTTP-upgraded (same framing envelope as
   Tailscale DERP). Payload is application data, not
   WireGuard. Firewall-friendly: looks like HTTPS on the
   wire. Peer roster is known. This is what HD-SDK
   customers run.
2. **WireGuard relay (this document).**
   Pure vanilla WireGuard over UDP. No envelope. No HD
   protocol framing. Payload is exactly what
   `wireguard.ko` emits. Peer roster is still known
   (pre-configured), but peers identify themselves via
   standard WG static public keys. Anyone running stock
   WireGuard can point their peer endpoint at the relay.

The WG mode does **not** carry HD-SDK traffic. The DERP
mode does **not** carry vanilla WireGuard. One binary
with a mode flag, two separate data planes, no overlap
on the wire.

The Hyper-DERP custom protocol upgrade is a future
enhancement to DERP mode only — unchanged by this
document.

## Goals

- Add `mode: wireguard` alongside `mode: derp`.
- Forward stock WireGuard packets over UDP.
- No client-side changes. If a client can talk to a
  normal WireGuard peer, it can talk to this relay by
  setting the peer endpoint to the relay's IP.
- Peer identity = 32-byte WG static public key, known in
  advance via the relay's config file.
- Reuse HD's data plane machinery: sharded io_uring
  workers, per-shard peer tables, SPSC cross-shard rings,
  frame pool.

## Non-goals

- Not a WireGuard implementation. HD does not hold peer
  private keys, does not perform WG's Noise handshake
  itself, and cannot see plaintext payloads.
- Not firewall-friendly. UDP is the point; corporate
  networks that block non-443 UDP require DERP mode.
- Not roaming-first. Peers with static or stably-prefixed
  endpoints are the v0 target. Full endpoint roaming
  (mobile peers on changing IPs) is v1.
- Not the Hyper-DERP custom protocol — that lives in
  DERP mode. See *Future work*.

## How peer identification works

WireGuard has no addressing field inside its packets that
tells a relay where to forward. A WG packet on the wire
looks like random noise except for four things we can
observe without any crypto key:

| Field | Location | Meaning |
|-------|----------|---------|
| message type | first 4 bytes, LE uint32 | 1=init, 2=response, 3=cookie, 4=data |
| sender_index | next 4 bytes (types 1, 2) | sender's session ID |
| receiver_index | bytes 4–8 (types 2, 3, 4) | receiver's session ID |
| MAC1 | last 32 bytes − 16 (types 1, 2) | keyed MAC, key = `BLAKE2s(LABEL_MAC1 ‖ responder_pubkey)` |

The MAC1 field is what lets us identify *who a handshake
packet is addressed to*. MAC1 is computed using the
receiving peer's static public key as the key
derivation input. If we know the set of candidate
destinations (the roster), we can try each one's pubkey
and the match tells us the destination. This is
cryptographically safe — MAC1 is designed to be
computable by anyone who knows the responder's public key,
and public keys are, well, public.

After a handshake completes, subsequent data packets
(type 4) carry a `receiver_index` that was established
during the handshake. We learn the session mapping
during handshake observation and then route data packets
by session index — no crypto on the hot path.

### Forwarding rules

| Packet type | Identification | Cost |
|-------------|----------------|------|
| 1 (init) | MAC1 scan against roster → dst pubkey | ~100 ns per candidate, <10 µs at 64 peers |
| 2 (response) | `receiver_index` lookup in session table → dst. Optionally MAC1 scan to learn initiator pubkey. | 1 table lookup |
| 3 (cookie) | `receiver_index` lookup | 1 table lookup |
| 4 (data) | `receiver_index` lookup | 1 table lookup |

Handshakes happen roughly every 2 minutes per peer pair
(WG's rekey interval). At a moderate peer count the MAC1
scan cost is negligible. Data packets — the 99.9 % case
— hit a per-worker hash table and that's it.

### Session tracking

Per-worker state:

```cpp
struct WgSession {
  uint32_t local_index;       // Our receiver sees this.
  uint32_t peer_index;        // Remote sends this.
  uint8_t dst_pubkey[32];     // Roster entry.
  uint8_t src_pubkey[32];     // Roster entry (may be zero until response observed).
  uint64_t last_seen_ns;
  int dst_peer_slot;          // Roster index, for shard routing.
  int src_peer_slot;
};

// Per-worker session index table.
std::unordered_map<uint32_t, WgSession> sessions_;
```

Session expiry: on idle > 180 seconds (WG's REJECT_AFTER_TIME).
Sessions cleaned up lazily during recv processing.

## Endpoint learning

Per-peer state:

```cpp
struct WgPeerEndpoint {
  struct sockaddr_in6 addr;
  uint64_t last_valid_packet_ns;
  uint8_t known;
};
```

Written by the owner worker for that peer. Read by any
worker during forwarding (atomic reads over a
cache-aligned struct; per-entry writes are single-writer).

### Bootstrap

Before peer X can receive anything through the relay, X's
endpoint must be known. Two ways:

1. **Static configuration** (v0 default). The roster
   entry includes an endpoint hint:
   ```yaml
   peers:
     - pubkey: abc...
       endpoint: 10.0.0.5:51820
   ```
   Endpoint is populated at startup. Subsequent observed
   4-tuples from matching pubkeys refresh (or reject,
   depending on auth mode) the entry.

2. **First-contact learning** (opportunistic). When a
   handshake initiation or response arrives from a
   previously-unknown peer, the source 4-tuple is
   recorded against the identified pubkey. Works for
   "responding to a known initiator" scenarios.

For full roaming (peer moves to a new IP mid-session),
v0 handles it only if auth mode is `allowlist` with a
wide CIDR. A mobile peer whose IP changes across
`allowlist` CIDRs is dropped until the peer's own WG
stack initiates a new handshake — at which point
bootstrap (1) or (2) re-establishes. This matches how WG
itself handles peer-side endpoint change.

## Peer configuration and authentication

Static roster. Three authentication modes, per peer.
HMAC is gone — WG's own Noise handshake authenticates the
peer cryptographically. A relay layer on top would be
redundant.

| Mode | Enforcement | Cost |
|------|-------------|------|
| `pin` | source 4-tuple must match configured endpoint | 1 comparison |
| `allowlist` | source IP in CIDR list | 1–N CIDR checks |
| `none` | pubkey match via MAC1/session is the only check | 0 |

### Config example

```yaml
mode: wireguard
port: 51820

wireguard:
  # Rate limit per src_pubkey. 0 = unlimited.
  per_peer_pps: 200000

  # Session table size. Covers peak concurrent sessions;
  # each peer pair uses 2 during rekey overlap. 4x the
  # peer count is a safe default.
  session_table_size: 4096

  peers:
    - pubkey: "abc123...0"  # 32-byte hex, WG static pubkey
      label: otc-fra
      auth:
        mode: pin
        endpoint: 10.0.0.5:51820

    - pubkey: "def456...0"
      label: pfsense-customer-a
      auth:
        mode: allowlist
        allowed_from: ["203.0.113.0/24"]

    - pubkey: "aaa111...0"
      label: wg-roaming-laptop
      auth:
        mode: none
```

### Validation pipeline

Each incoming UDP packet, in order:

1. Length check: type 1 = 148 B, type 2 = 92 B, type 3 =
   64 B, type 4 = variable (≥ 32 B). Reject otherwise.
2. Type byte is one of {1, 2, 3, 4}. Reject otherwise.
3. For types 1, 2: MAC1 scan against the roster. The
   matching pubkey is the destination. Reject if no
   match.
   For types 3, 4: look up `receiver_index` in session
   table. The session's recorded `dst_pubkey` is the
   destination. Reject if no session.
4. Per-peer auth check on the **source** pubkey if known
   (for responses and data with established session), or
   on the **source endpoint** for initiations:
   - `pin`: source 4-tuple must equal configured endpoint.
   - `allowlist`: source IP in configured CIDR list.
   - `none`: pass.
5. Per-peer PPS rate limit on the source pubkey (or
   source endpoint for anonymous initiations).

Only after all checks pass does the relay update endpoint
state and forward.

Rejecting before endpoint update matters: a spoofed
initiation that passes (1)–(3) but fails (4) must **not**
cause an endpoint write, or an attacker with knowledge of
peer pubkeys but not endpoint secrets could hijack a
peer's endpoint entry.

### Why no HMAC layer

WG's handshake already provides mutual cryptographic
authentication between peers using Noise_IK. A relay-layer
HMAC would:

- Add CPU cost per packet.
- Add nothing security-wise — a successfully-authenticated
  peer as seen by the relay can still send arbitrary
  payloads that WG will reject at the destination.
- Break vanilla-WG compatibility.

The relay's role is access control (who may *use* the
relay), not message authenticity (who sent *what* payload).
`pin`/`allowlist` do the former. WG does the latter E2E.

## Architecture

### Mode selection

`main.cc` branches on `config.mode`:

- `derp`: existing path (`server.cc` TCP accept →
  kTLS → DERP handshake → DERP data plane).
- `wireguard`: new `udp_server.cc` binds a UDP socket
  with `SO_REUSEPORT`, hands per-worker sockets to
  workers, runs `wg_data_plane.cc`.

### Shard assignment

With `SO_REUSEPORT`, the kernel distributes inbound UDP
across N per-worker sockets by 4-tuple. The arrival
worker is not necessarily the owner of the destination
pubkey. Use the DERP mode's existing pattern: arrival
worker checks shard ownership, forwards directly if
match, SPSC-enqueues to owner worker otherwise.

`shard_of(pubkey)` = FNV-1a over the pubkey modulo
worker count. Consistent with DERP mode's sharding.

### Per-worker state added for WG mode

```cpp
// Session index → session record.
std::unordered_map<uint32_t, WgSession> wg_sessions;

// Pubkey (slot index) → endpoint. Owner-worker writes.
WgPeerEndpoint wg_endpoints[kMaxWgPeers];

// Per-peer token bucket for rate limiting.
struct WgRateBucket {
  uint32_t tokens;
  uint64_t last_refill_ns;
};
WgRateBucket wg_buckets[kMaxWgPeers];
```

`kMaxWgPeers` default 4096.

### Code additions

Added:

- `src/udp_server.cc` (~180 LOC): socket setup,
  `SO_REUSEPORT`, per-worker fd distribution.
- `src/wg_data_plane.cc` (~350 LOC): the packet loop,
  validation pipeline, session tracking, forwarding.
- `src/wg_mac1.cc` (~60 LOC): BLAKE2s MAC1 computation
  and roster scan.
- `src/wg_peer_config.cc` (~120 LOC): roster parsing,
  pubkey → shard slot table, auth mode dispatch.
- `include/hyper_derp/wg_types.h` (~80 LOC): `WgSession`,
  `WgPeerEndpoint`, rate bucket.

Reused unchanged:

- Worker thread / io_uring bring-up.
- SPSC `xfer_inbox` cross-shard rings.
- Command ring.
- Frame pool allocator.
- Stats struct (new counters appended).
- Metrics exporter.

Removed in this mode (compiled but unused at runtime):

- TCP accept, HTTP upgrade, DERP handshake, kTLS, DERP
  frame parser. All still present for `mode: derp`;
  simply not invoked.

**Total: ~790 LOC new. Zero LOC removed from DERP mode.**

### Dependency

BLAKE2s for MAC1 verification. Already available via
OpenSSL (`EVP_blake2s256`). No new dependency.

## Observability

New per-worker counters, exposed via the existing
Prometheus endpoint:

| Counter | Meaning |
|---------|---------|
| `wg_rx_packets` | Packets received on the UDP socket. |
| `wg_rx_bytes` | Bytes received. |
| `wg_tx_packets` | Packets forwarded onward. |
| `wg_tx_bytes` | Bytes forwarded. |
| `wg_handshake_init` | Type-1 packets observed. |
| `wg_handshake_resp` | Type-2 packets observed. |
| `wg_data_packets` | Type-4 packets observed. |
| `wg_reject_length` | Length check failed. |
| `wg_reject_type` | Unknown message type. |
| `wg_reject_mac1_no_match` | MAC1 scan found no candidate. |
| `wg_reject_session_unknown` | `receiver_index` not in session table. |
| `wg_reject_auth_pin` | Source 4-tuple != pinned endpoint. |
| `wg_reject_auth_allowlist` | Source IP not in CIDR list. |
| `wg_rate_limited` | Dropped by per-peer PPS limit. |
| `wg_no_dest` | Destination pubkey has no known endpoint. |
| `wg_session_inserts` | New sessions added. |
| `wg_session_evictions` | Idle sessions cleaned up. |
| `wg_mac1_scan_us_total` | Cumulative time spent in MAC1 scan. |

Per-peer gauges:

- `wg_peer_endpoint_known{pubkey="..."}`: 0 or 1.
- `wg_peer_session_count{pubkey="..."}`: number of
  active sessions for the peer.

## Benchmark plan

Compare HD-WG against HD-DERP on identical hardware, and
against `wireguard-go`-based relays where one exists
(e.g. a simple Tailscale-style userspace relay). The
comparison against TS-derper is not meaningful because
derper cannot do WG relay — they carry different
payloads.

The interesting measurements are:

| Scenario | Expected vs DERP mode |
|----------|----------------------|
| baseline / idle | lower p50 (no TLS, no TCP ACK RTT) |
| steady | higher throughput (no kTLS crypto) |
| burst on/off | flat (no TCP slow-start) |
| 1% / 3% loss | **much better tail latency** (no TCP head-of-line blocking) |
| 50 ms WAN | better (no TCP window ramp) |

The loss columns are the strongest story. This is the
ammunition for the "why would I run this instead of DERP"
question.

## Deployment notes

- Not firewall-friendly. Deploy on a separate port from
  DERP mode. Clients/admins choose which transport to
  use based on the network between them and the relay.
- UDP source-address spoofing is possible on networks
  without BCP 38. The target deployment (known peers,
  pin/allowlist auth) limits exposure; a relay with
  `auth: none` on all peers accessible from the open
  internet is a bad idea.
- `per_peer_pps` default 200000. At 1440 B that's ~2.3
  Gbps per peer ceiling. Adjust for the deployment.
- Socket buffer size: recommend `net.core.rmem_max` /
  `wmem_max` ≥ 16 MiB. HD sets `SO_RCVBUF` to whatever
  the config specifies, capped by the sysctl.
- kTLS is not relevant in this mode. Hosts that do not
  have `CONFIG_TLS` can still run WG mode.

## Future work

- **Full roaming support** (v1). Introduce a "peer
  liveness beacon": clients send a small periodic
  application packet (not vanilla WG) to register
  endpoint changes. Opt-in per peer.
- **Hyper-DERP protocol upgrade.** DERP-mode concern
  only. Unrelated to this document.
- **Dynamic peer roster.** Admin UI / control-plane
  command to add or remove peers at runtime.
- **eBPF / XDP fast path.** Move data-packet
  forwarding (type 4, by `receiver_index`) into a kernel
  XDP program. Handshakes stay in userspace. Separate
  project; same wire format (unchanged — it's just WG),
  so userspace remains the fallback and is always
  compatible.
- **Cookie-reply generation under load.** Not a forward
  problem but a DoS-mitigation: the relay could emit
  WG cookie-reply (type 3) to abusive initiators
  without involving the destination. Out of scope in v0.

## Build phases

1. **Phase 0:** `mode` flag in config + main.cc branch.
   `mode: wireguard` logs "not implemented" and exits.
   ~20 LOC. Ships immediately.
2. **Phase 1:** UDP server bring-up. Per-worker sockets
   via `SO_REUSEPORT`, multishot recv, `wg_rx_*`
   counters. No forwarding.
3. **Phase 2:** Packet-type classification + MAC1 scan.
   Identifies destination pubkey on handshake packets.
   Still no forwarding.
4. **Phase 3:** Session table + `receiver_index` routing
   for types 2/3/4.
5. **Phase 4:** Endpoint learning + static config
   population + own-shard forwarding.
6. **Phase 5:** Cross-worker forwarding via SPSC
   `xfer_inbox`. Full data plane online.
7. **Phase 6:** Auth modes (`pin`, `allowlist`, `none`) +
   rate limiting + full stats.
8. **Phase 7:** WG-mode benchmark harness. A loopback
   smoke test with real `wg-quick` peers on the same
   host, driven through the relay, validates correctness
   before any GCP work.

Each phase is independently testable. Loopback testing
with real WireGuard (two `wg-quick` interfaces in
separate netns pointed at a locally-bound relay) is the
primary correctness tool. GCP re-runs only once phases
0–7 pass on loopback.
