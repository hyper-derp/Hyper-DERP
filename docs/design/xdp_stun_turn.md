# Level 2: XDP STUN/TURN — Design Document

## Overview

Level 2 upgrades peer connections from relay-forwarded
(Level 1 HD / Level 0 DERP) to direct WireGuard UDP.
The relay assists with NAT traversal via STUN and
provides fallback allocation via TURN.

The STUN/TURN data plane runs in XDP (eXpress Data Path)
at the NIC driver level. Packet inspection, binding
responses, and channel data forwarding happen without
entering the kernel network stack. The userspace relay
handles only allocation setup and permission management
via BPF maps.

## Architecture

```
                    ┌─────────────────────────┐
                    │     Userspace (HD)       │
                    │                          │
                    │  TURN allocation setup   │
                    │  Permission management   │
                    │  STUN credential gen     │
                    │  Peer signaling (L1)     │
                    │          │               │
                    │    BPF map updates       │
                    │          │               │
                    └──────────┼───────────────┘
                               │
              ┌────────────────┼────────────────┐
              │           BPF Maps              │
              │                                 │
              │  stun_creds: nonce → valid       │
              │  turn_alloc: 5-tuple → alloc_id  │
              │  turn_chan:  channel# → peer_addr │
              │  turn_perm: peer_addr → allowed   │
              └────────────────┼────────────────┘
                               │
              ┌────────────────┼────────────────┐
              │         XDP Program             │
              │                                 │
              │  1. Parse UDP header            │
              │  2. Check STUN magic cookie     │
              │  3. STUN Binding? → reply       │
              │  4. TURN ChannelData? → forward │
              │  5. Else → XDP_PASS (userspace) │
              └─────────────────────────────────┘
```

## Control/Data Plane Split

| Operation | Where | Why |
|-----------|-------|-----|
| STUN Binding Request/Response | XDP | Pure stateless: parse, flip src/dst, write XOR-MAPPED-ADDRESS |
| STUN credential validation | XDP (map lookup) | Userspace generates short-lived creds, XDP validates via map |
| TURN Allocate | Userspace | Stateful: allocates relay transport address, creates permissions |
| TURN CreatePermission | Userspace | Policy decision, updates BPF map |
| TURN ChannelBind | Userspace | Maps channel number → peer address in BPF map |
| TURN ChannelData forwarding | XDP | Stateless after bind: lookup channel → rewrite → TX |
| TURN Send/Data indication | Userspace | Rare, only for non-channel-bound peers |
| ICE candidate exchange | L1 signaling | Peers exchange candidates via HD PeerInfo frames |

## XDP Program Design

### Packet Classification

```c
SEC("xdp")
int hd_stun_turn(struct xdp_md *ctx) {
  void *data = (void *)(long)ctx->data;
  void *data_end = (void *)(long)ctx->data_end;

  // Parse Ethernet + IP + UDP headers.
  struct ethhdr *eth = data;
  if ((void *)(eth + 1) > data_end)
    return XDP_PASS;
  if (eth->h_proto != htons(ETH_P_IP))
    return XDP_PASS;

  struct iphdr *ip = (void *)(eth + 1);
  if ((void *)(ip + 1) > data_end)
    return XDP_PASS;
  if (ip->protocol != IPPROTO_UDP)
    return XDP_PASS;

  struct udphdr *udp = (void *)ip + (ip->ihl * 4);
  if ((void *)(udp + 1) > data_end)
    return XDP_PASS;

  // Check destination port matches STUN/TURN port.
  if (udp->dest != htons(STUN_PORT))
    return XDP_PASS;

  void *payload = (void *)(udp + 1);
  int payload_len = ntohs(udp->len) - sizeof(*udp);

  // STUN magic cookie check (bytes 4-7 of STUN header).
  if (payload_len < 20)
    return XDP_PASS;
  __u32 *cookie = payload + 4;
  if (*cookie == htonl(0x2112A442)) {
    return handle_stun(ctx, eth, ip, udp, payload,
                       payload_len);
  }

  // TURN ChannelData: first 2 bytes are channel number
  // (0x4000-0x7FFF range).
  __u16 chan = *(__u16 *)payload;
  if (ntohs(chan) >= 0x4000 && ntohs(chan) <= 0x7FFF) {
    return handle_channel_data(ctx, eth, ip, udp,
                               payload, payload_len);
  }

  // Unknown — pass to userspace.
  return XDP_PASS;
}
```

### STUN Binding Response (XDP)

```c
static int handle_stun(struct xdp_md *ctx, ...) {
  struct stun_hdr *stun = payload;

  // Only handle Binding Request (type 0x0001).
  if (ntohs(stun->type) != 0x0001)
    return XDP_PASS;  // Let userspace handle other types.

  // Validate MESSAGE-INTEGRITY if credentials required.
  // Lookup transaction ID in stun_creds map.
  // If not found → XDP_PASS (userspace handles).

  // Build Binding Response in-place:
  // 1. Swap Ethernet src/dst MAC
  // 2. Swap IP src/dst
  // 3. Swap UDP src/dst ports
  // 4. Set STUN type to 0x0101 (Binding Response)
  // 5. Write XOR-MAPPED-ADDRESS attribute:
  //    - XOR the client's IP with magic cookie
  //    - XOR the client's port with magic cookie[0:2]
  // 6. Recalculate UDP checksum (or set to 0 for IPv4)
  // 7. Return XDP_TX (send back out same NIC)

  swap_mac(eth);
  swap_ip(ip);
  swap_udp_ports(udp);

  stun->type = htons(0x0101);  // Binding Response

  // Write XOR-MAPPED-ADDRESS (type 0x0020).
  struct xor_mapped_addr *attr = (void *)(stun + 1);
  attr->type = htons(0x0020);
  attr->length = htons(8);
  attr->family = 0x01;  // IPv4
  attr->port = client_port ^ htons(0x2112);
  attr->addr = client_ip ^ htonl(0x2112A442);

  // Update lengths.
  stun->length = htons(12);  // One attribute
  udp->len = htons(sizeof(*udp) + 20 + 12);
  ip->tot_len = htons(ntohs(ip->tot_len));

  // Recalculate IP checksum.
  ip->check = 0;
  ip->check = ip_checksum(ip);

  return XDP_TX;
}
```

### TURN ChannelData Forwarding (XDP)

```c
struct turn_channel_key {
  __u16 channel;
  __u32 alloc_src_ip;
  __u16 alloc_src_port;
};

struct turn_channel_val {
  __u32 peer_ip;
  __u16 peer_port;
  __u8  peer_mac[6];
};

struct {
  __uint(type, BPF_MAP_TYPE_HASH);
  __uint(max_entries, 65536);
  __type(key, struct turn_channel_key);
  __type(value, struct turn_channel_val);
} turn_channels SEC(".maps");

static int handle_channel_data(struct xdp_md *ctx, ...) {
  // ChannelData: [2B channel][2B length][data...]
  __u16 chan = ntohs(*(__u16 *)payload);
  __u16 data_len = ntohs(*(__u16 *)(payload + 2));

  // Lookup channel → peer address.
  struct turn_channel_key key = {
    .channel = chan,
    .alloc_src_ip = ip->saddr,
    .alloc_src_port = udp->source,
  };
  struct turn_channel_val *val =
      bpf_map_lookup_elem(&turn_channels, &key);
  if (!val)
    return XDP_PASS;  // Unknown channel → userspace.

  // Rewrite packet: send ChannelData to peer.
  // 1. Set dst MAC to peer's MAC (from map, or ARP).
  // 2. Set dst IP to peer's IP.
  // 3. Set dst port to peer's port.
  // 4. Set src IP to relay's allocated address.
  // 5. Recalculate checksums.
  // 6. XDP_TX.

  memcpy(eth->h_dest, val->peer_mac, 6);
  ip->daddr = val->peer_ip;
  udp->dest = htons(val->peer_port);

  // Recalculate.
  ip->check = 0;
  ip->check = ip_checksum(ip);
  udp->check = 0;  // Optional for IPv4 UDP.

  return XDP_TX;
}
```

## BPF Maps

| Map | Type | Key | Value | Updated By |
|-----|------|-----|-------|------------|
| `stun_creds` | HASH | transaction_id (12B) | {username, realm, nonce_valid} | Userspace (on ICE start) |
| `turn_allocs` | HASH | 5-tuple (src_ip, src_port, proto, relay_ip, relay_port) | {alloc_id, lifetime, created_at} | Userspace (on Allocate) |
| `turn_channels` | HASH | {channel#, alloc_src_ip, alloc_src_port} | {peer_ip, peer_port, peer_mac} | Userspace (on ChannelBind) |
| `turn_perms` | HASH | {alloc_id, peer_ip} | {expires_at} | Userspace (on CreatePermission) |
| `stats` | PERCPU_ARRAY | index (0=stun_req, 1=stun_resp, 2=chan_fwd, 3=dropped) | counter | XDP (atomic increment) |

## ICE Integration

The HD relay acts as the ICE signaling channel. Peers
exchange candidates via HD PeerInfo frames (Level 1):

```
1. Peer A connects to relay (Level 1 HD)
2. Peer B connects to relay (Level 1 HD)
3. Relay sends PeerInfo to A with B's public endpoint
4. Relay sends PeerInfo to B with A's public endpoint
5. Both peers gather ICE candidates:
   - Host candidates (local IPs)
   - Server-reflexive (via STUN to relay)
   - Relay candidates (via TURN allocation on relay)
6. Candidates exchanged through relay's L1 channel
7. ICE connectivity checks (STUN Binding) — handled by XDP
8. ICE succeeds → direct WireGuard UDP established
9. Relay detects direct path active (no L1 data traffic)
10. Level 2 active — relay only handles keepalives
```

### Fallback

```
Direct path fails (firewall, symmetric NAT):
1. ICE connectivity checks timeout
2. TURN allocation activated as fallback
3. WireGuard UDP flows through TURN ChannelData
4. XDP forwards ChannelData at line rate
5. Still faster than Level 1 (no TLS, no TCP)
```

## Userspace Components

### STUN/TURN Server (new module)

```
include/hyper_derp/stun.h    — STUN message codec
include/hyper_derp/turn.h    — TURN allocation manager
src/stun.cc                  — Message parsing, credential gen
src/turn.cc                  — Allocate, CreatePermission,
                               ChannelBind, BPF map updates
```

The userspace TURN server handles:
- Allocate requests (creates relay transport address)
- CreatePermission (updates turn_perms BPF map)
- ChannelBind (updates turn_channels BPF map)
- Allocation refresh and expiry
- Credential generation for ICE

It does NOT handle:
- STUN Binding Requests (XDP)
- ChannelData forwarding (XDP)
- Data indications for channel-bound peers (XDP)

### XDP Loader

```
src/xdp_loader.cc            — Load/attach XDP program
bpf/hd_stun_turn.bpf.c       — XDP program source
```

Uses libbpf to:
1. Load the compiled XDP program
2. Attach to the relay's NIC interface
3. Pin BPF maps for userspace access
4. Detach on shutdown

### ICE Agent

```
include/hyper_derp/ice.h     — ICE candidate gathering,
                               connectivity checks, nomination
src/ice.cc                   — ICE state machine per peer pair
```

Integrated into the control plane. When two HD peers are
both connected to the relay, the ICE agent:
1. Gathers candidates for each peer
2. Exchanges them via PeerInfo frames
3. Monitors connectivity check results
4. Nominates the best path
5. Signals Level 2 active to both peers

## Performance Expectations

| Operation | Path | Expected Latency |
|-----------|------|-----------------|
| STUN Binding | XDP | <1 μs (no kernel stack) |
| TURN ChannelData forward | XDP | <2 μs (map lookup + rewrite) |
| TURN Allocate | Userspace | ~100 μs (one-time) |
| ICE candidate exchange | L1 HD | ~200 μs (relay RTT) |
| Direct WireGuard (L2) | Kernel | Same as native WireGuard |

At 10 Gbps line rate with 1400B packets:
- ~893K packets/sec
- XDP can handle this on a single core
- No context switches, no socket buffers, no copies

## Build Requirements

- Linux kernel 5.15+ (XDP, BPF CO-RE)
- libbpf 1.0+
- clang (for BPF program compilation)
- bpftool (for skeleton generation)

```cmake
# bpf/ directory
add_custom_command(
  OUTPUT hd_stun_turn.skel.h
  COMMAND clang -O2 -target bpf -c
    ${CMAKE_SOURCE_DIR}/bpf/hd_stun_turn.bpf.c
    -o hd_stun_turn.bpf.o
  COMMAND bpftool gen skeleton hd_stun_turn.bpf.o
    > hd_stun_turn.skel.h
  DEPENDS bpf/hd_stun_turn.bpf.c
)
```

## Deployment

XDP mode selection:

| Mode | Requirement | Performance |
|------|------------|------------|
| Native (DRV) | NIC driver support | Best (~10M pps) |
| Generic (SKB) | Any NIC | Good (~2M pps) |
| Offload (HW) | SmartNIC (ConnectX-5+) | Line rate |

The relay auto-detects: try native first, fall back to
generic. Hardware offload is opt-in via config.

```yaml
# hyper-derp.yaml
level2:
  enabled: true
  stun_port: 3478
  xdp_mode: auto        # auto, native, generic, offload
  xdp_interface: eth0   # NIC to attach XDP program
  turn:
    realm: relay.example.com
    max_allocations: 10000
    default_lifetime: 600
```

## Implementation Order

1. STUN message codec (stun.h/cc) + tests
2. XDP program — STUN Binding only
3. XDP loader + BPF map setup
4. TURN allocation manager (turn.h/cc) + tests
5. XDP program — ChannelData forwarding
6. ICE agent + PeerInfo signaling
7. Level 2 upgrade/downgrade state machine
8. Integration with HD relay control plane
