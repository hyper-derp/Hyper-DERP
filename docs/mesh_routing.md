# Mesh Routing Design

HD Protocol replaces DERP's per-packet key addressing with
connection-time enrollment and compact integer IDs. This
document covers local mesh routing (1:N on a single relay),
cross-relay fleet routing, and the distance-vector protocol
that ties them together.

## Problem: 1:1 vs 1:N Routing

DERP sends every packet with a 32-byte destination key
prepended to the payload. The relay looks up the key in a
hash table on every frame:

```
DERP SendPacket: [1B type][4B len][32B dst_key][payload]
```

This costs 37 bytes of overhead per packet and forces a
hash lookup per frame. For 1:N patterns (screen sharing,
fleet broadcast, site-to-site mesh), the sender must emit
N copies of the same payload, each with a different 32-byte
destination key.

HD Protocol authenticates once at connection time (HMAC
enrollment) and assigns each peer a 2-byte local ID. After
that, per-packet overhead drops to the frame header alone
for point-to-point traffic, or 2-4 extra bytes for mesh
and fleet routing.

## Hierarchical Addressing

HD uses a two-level address space:

| Field | Size | Range | Scope |
|-------|-----:|------:|-------|
| Relay ID | 2 bytes | 0-65535 | Global (fleet-wide) |
| Peer ID | 2 bytes | 1-65535 | Local (per-relay) |

A full fleet address is 4 bytes: `[relay_id][peer_id]`.
This gives 65535 relays x 65535 peers = ~4.3 billion
addressable endpoints.

Relay IDs are configured at startup
(`HdPeerRegistry::relay_id` in `hd_peers.h:63`). Peer IDs
are assigned monotonically during enrollment
(`HdPeerRegistry::next_peer_id` in `hd_peers.h:62`).

## Frame Types

HD Protocol defines three data frame types with increasing
addressing scope. All share the same 4-byte frame header
(`hd_protocol.h:17`, `kHdFrameHeaderSize`):

```
HD header: [1B type][3B big-endian length]
```

### Data (kData = 0x01)

Point-to-point. No destination address in the frame; the
relay forwards based on the connection's forwarding rules.

```
[1B type=0x01][3B len][payload]
Total overhead: 4 bytes
```

The relay looks up the sender's `HdForwardRule` array
(`hd_peers.h:38`) and forwards the payload to all matching
destinations. This is a connection-time decision, not a
per-packet lookup.

### MeshData (kMeshData = 0x04)

Local 1:N with explicit 2-byte peer ID. Allows a sender
to address any peer on the same relay by its local ID.

```
[1B type=0x04][3B len][2B dst_peer_id][payload]
Total overhead: 6 bytes
```

The 2-byte destination is read with `HdReadMeshDst()`
(`hd_protocol.h:186`). The relay resolves the peer ID to
an fd via `HdPeersLookupById()` (`hd_peers.h:86`) and
forwards the payload directly.

### FleetData (kFleetData = 0x05)

Cross-relay routing with 2-byte relay ID + 2-byte peer ID.
Used when the destination is on a different relay in the
fleet.

```
[1B type=0x05][3B len][2B dst_relay_id][2B dst_peer_id][payload]
Total overhead: 8 bytes
```

The relay reads the destination relay ID with
`HdReadFleetRelay()` (`hd_protocol.h:208`) and peer ID
with `HdReadFleetPeer()` (`hd_protocol.h:219`).

### Comparison with DERP

| | DERP SendPacket | HD Data | HD MeshData | HD FleetData |
|-|----------------:|--------:|------------:|-------------:|
| Header | 5B | 4B | 4B | 4B |
| Address | 32B | 0B | 2B | 4B |
| **Total overhead** | **37B** | **4B** | **6B** | **8B** |
| Address scope | Global (key) | Connection rules | Local relay | Fleet-wide |
| Lookup | Hash per pkt | None (rules) | ID lookup | Relay table |

At the target 1400-byte WireGuard MTU payload, DERP wastes
2.6% of each packet on addressing. HD Data wastes 0.3%.

## Relay Forwarding Logic

### Data (point-to-point, rule-based)

```
on recv Data frame from peer P:
  for each rule in P.rules:
    dst = lookup_by_key(rule.dst_key)
    if dst != null and dst.state == Approved:
      send(dst.fd, frame)
```

### MeshData (local 1:N)

```
on recv MeshData frame from peer P:
  dst_id = read_mesh_dst(payload)
  dst = lookup_by_id(dst_id)
  if dst != null and dst.state == Approved:
    send(dst.fd, payload[2:])
```

### FleetData (cross-relay)

```
on recv FleetData frame from peer P:
  relay_id = read_fleet_relay(payload)
  peer_id  = read_fleet_peer(payload)

  if relay_id == self.relay_id:
    # Local delivery.
    dst = lookup_by_id(peer_id)
    if dst != null:
      send(dst.fd, payload[4:])
  else:
    # Cross-relay forwarding.
    route = relay_table_lookup(relay_id)
    if route != null:
      forward(route.fd, frame)
```

## Cross-Relay Forwarding

Relays connect to each other as HD peers. When a relay
receives a FleetData frame destined for a remote relay, it
looks up the route in the relay table (`hd_relay_table.h`)
and forwards the entire frame over the inter-relay
connection.

The relay table stores direct neighbors (hop count 0) and
learned routes (hop count 1+). Each entry tracks:

- `relay_id` -- destination relay's ID
- `fd` -- next-hop connection fd (direct or via neighbor)
- `hops` -- distance to the destination
- `next_hop` -- relay ID of the next hop

See `RelayEntry` in `hd_relay_table.h:21`.

## Distance-Vector Routing

Relays exchange reachability information using
RouteAnnounce frames (`HdFrameType::kRouteAnnounce = 0x30`
in `hd_protocol.h:49`).

### Announcement Format

```
RouteAnnounce: [1B type=0x30][3B len][entries...]
  Entry: [2B relay_id][1B hops]   (kHdRouteEntrySize = 3)
```

Built with `HdBuildRouteAnnounce()` (`hd_protocol.h:235`),
parsed with `HdParseRouteAnnounce()` (`hd_protocol.h:248`).

### Protocol

1. Every 30 seconds, each relay sends a RouteAnnounce
   frame to all direct neighbors listing every relay it
   can reach and the hop count.

2. On receiving an announcement from neighbor N:
   ```
   for each (relay_id, hops) in announcement:
     new_hops = hops + 1
     existing = relay_table_lookup(relay_id)
     if existing == null or new_hops < existing.hops:
       relay_table_update(relay_id, new_hops,
                          next_hop=N.relay_id,
                          next_hop_fd=N.fd)
   ```

3. When a neighbor disconnects, all routes via that
   neighbor are invalidated. The relay sends triggered
   updates to remaining neighbors.

4. Split horizon: a relay does not announce routes back to
   the neighbor it learned them from.

5. Maximum hop count: 15. Routes with hops >= 15 are
   treated as unreachable (poison reverse).

### Convergence

For a fleet of R relays with maximum diameter D:
- Convergence time: D x 30 seconds (worst case)
- Announcement size: R x 3 bytes per neighbor
- Bandwidth: negligible (a 4096-relay fleet produces
  12 KiB announcements every 30 seconds)

## Topology Examples

### Point-to-Point (Data)

```
Client A ──TCP──▶ Relay ◀──TCP── Client B
       Data(payload) ────▶ Data(payload)
```

A sends Data; relay forwards to B via A's forwarding rules.
4 bytes overhead per packet.

### Multi-Viewer (MeshData)

```
Presenter ──TCP──▶ Relay ◀──TCP── Viewer 1 (id=3)
                       ◀──TCP── Viewer 2 (id=4)
                       ◀──TCP── Viewer 3 (id=5)

Presenter sends:
  MeshData(dst=3, payload)
  MeshData(dst=4, payload)
  MeshData(dst=5, payload)
```

6 bytes overhead per recipient. DERP would require 37 bytes
each (32-byte key per viewer).

### Site-to-Site Mesh (Data + rules)

```
Site A peers: A1, A2, A3
Site B peers: B1, B2, B3

Rules on A1: [dst=B1, dst=B2, dst=B3]
Rules on B1: [dst=A1, dst=A2, dst=A3]

A1 sends Data ──▶ Relay forwards to B1, B2, B3
```

Forwarding is rule-based. A single Data frame (4B overhead)
fans out to all rule destinations.

### Cross-Relay Fleet (FleetData)

```
Relay 1 (id=1)          Relay 2 (id=2)
  Client A (id=5)         Client B (id=8)

A sends FleetData(relay=2, peer=8, payload)
  ▶ Relay 1 looks up relay 2 in relay table
  ▶ Forwards frame to Relay 2 via inter-relay link
  ▶ Relay 2 reads peer=8, delivers to Client B
```

8 bytes overhead. DERP has no cross-relay concept.

### Global Multi-Hop

```
Relay 1 ──── Relay 2 ──── Relay 3
  A                          B

A sends FleetData(relay=3, peer=id_B, payload)
  ▶ Relay 1: route to 3 via next_hop=2 (1 hop)
  ▶ Relay 2: route to 3 via next_hop=3 (0 hops, direct)
  ▶ Relay 3: local delivery to B
```

Distance-vector routing handles the path. No per-packet
source routing or overlay headers.

## Client SDK API

The HD client SDK (`hd_client.h`) provides:

| Function | Frame Type | Use Case |
|----------|-----------|----------|
| `HdClientSendData()` | Data (0x01) | Point-to-point via rules |
| `HdClientSendMeshData()` | MeshData (0x04) | Addressed to local peer ID |
| `HdClientRecvFrame()` | Any | Receives next frame |

FleetData is relay-originated (clients send MeshData or
Data; the relay wraps in FleetData for cross-relay hops).

### Connection Lifecycle

```
HdClientInit()         Generate Curve25519 keys
HdClientConnect()      TCP connect to relay
HdClientTlsConnect()   Optional TLS 1.3
HdClientUpgrade()      HTTP upgrade to /hd
HdClientEnroll()       Send Enroll + HMAC, await Approved
HdClientSendData()     Send packets
HdClientRecvFrame()    Receive packets
HdClientClose()        Disconnect
```

## Authorization Model

HD uses HMAC-SHA-512-256 enrollment (`crypto_auth` from
libsodium). The relay and client share a pre-shared key.
During enrollment:

1. Client sends Enroll frame:
   `[32B client_key][32B HMAC(client_key, relay_key)]`
2. Relay verifies HMAC via `HdVerifyEnrollment()`
   (`hd_peers.h:144`)
3. If valid, relay either:
   - Auto-approves (`HdEnrollMode::kAutoApprove`) and sends
     Approved frame immediately
   - Holds in pending state (`HdEnrollMode::kManual`) for
     admin approval via REST API

Forwarding rules are configured per-peer via the REST API
(`/api/v1/peers`). A peer with no rules receives nothing.
A peer can only send to destinations allowed by its rules.

## Scaling

### Per-Relay Limits

| Resource | Limit | Source |
|----------|------:|--------|
| Peers | 4,096 | `kHdMaxPeers` (`hd_peers.h:29`) |
| Rules per peer | 16 | `kHdMaxForwardRules` (`hd_peers.h:32`) |
| Relay table entries | 4,096 | `kMaxRelays` (`hd_relay_table.h:18`) |

### Fleet Scaling

| Fleet Size | Address Space | Announcement Size |
|-----------:|--------------:|------------------:|
| 10 relays | 655K peers | 30 bytes/neighbor |
| 100 relays | 6.5M peers | 300 bytes/neighbor |
| 1,000 relays | 65M peers | 3 KB/neighbor |
| 65,535 relays | 4.3B peers | 192 KB/neighbor |

Route announcements every 30 seconds. At 1,000 relays with
10 neighbors each, total routing traffic is ~30 KB/30s per
relay.

### Connection Scaling

Each peer holds one TCP connection to one relay. Relays
connect to each other for inter-relay forwarding. An R-relay
fleet with full mesh inter-relay connectivity requires
R*(R-1)/2 inter-relay connections. For large fleets, partial
mesh (hub-and-spoke or regional clustering) reduces this.

## Fleet Discovery and Bootstrapping

Relays discover each other through seed relays configured
at startup. The bootstrap process:

1. Relay starts with a list of seed relay addresses in its
   config file.
2. Relay connects to each seed as an HD peer (using
   inter-relay credentials).
3. Seed relays send RouteAnnounce frames listing all known
   relays.
4. New relay populates its relay table from announcements.
5. New relay connects to nearby relays (low hop count)
   for redundancy.
6. After 30 seconds, the new relay sends its own
   RouteAnnounce to all neighbors, making itself
   discoverable.

Seed relays are not special -- any relay can serve as a
seed. The seed list just provides initial contact points.

## Implementation Phases

### Phase 1: Single-Relay HD (done)

- [x] HD wire protocol (`hd_protocol.h`, `hd_protocol.cc`)
- [x] Enrollment handshake (`hd_handshake.h`)
- [x] Peer registry (`hd_peers.h`)
- [x] Data frame forwarding (rule-based)
- [x] MeshData forwarding (peer ID lookup)
- [x] HD client SDK (`hd_client.h`)
- [x] DERP-to-HD bridge (`hd_bridge.h`)
- [x] REST API for peer management
- [x] Unit and integration tests

### Phase 2: Fleet Routing

- [x] Relay table (`hd_relay_table.h`)
- [x] FleetData forwarding
- [x] RouteAnnounce build/parse
- [ ] Distance-vector routing loop
- [ ] Inter-relay connection manager
- [ ] Fleet bootstrap from seed relays
- [ ] Multi-hop forwarding tests

### Phase 3: Direct Path (Level 2)

- [x] STUN codec (`stun.h`)
- [x] TURN allocation manager (`turn.h`)
- [x] ICE agent (`ice.h`)
- [x] XDP STUN/TURN program (`xdp_loader.h`)
- [ ] ICE integration with HD control plane
- [ ] Level 2 upgrade/downgrade state machine

### Phase 4: AF_XDP Data Plane

- [ ] AF_XDP socket wrapper
- [ ] UDP HD frame parser
- [ ] Dual-port relay with cross-port forwarding
- [ ] Integration with TCP enrollment path
