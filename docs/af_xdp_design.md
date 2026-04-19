# AF_XDP Dual-Port Zero-Copy Relay

## Goal

Bypass the kernel TCP/IP stack entirely for HD Protocol
forwarding. Use AF_XDP sockets on both ConnectX-4 25GbE
ports for a combined 50 Gbps relay path.

## Architecture

```
Port 0 (ens4f0np0)          Port 1 (ens4f1np1)
  25GbE recv                  25GbE recv
       │                           │
       ▼                           ▼
  XDP program                 XDP program
  (UMEM rx ring)              (UMEM rx ring)
       │                           │
       ▼                           ▼
  ┌─────────────────────────────────────┐
  │         HD Relay (userspace)        │
  │                                     │
  │  AF_XDP rx poll → parse HD frame    │
  │  → lookup cached Peer* → memcpy    │
  │  → AF_XDP tx submit               │
  │                                     │
  │  No kernel TCP. No syscalls.       │
  │  No socket buffers. No copies      │
  │  beyond the UMEM ring.             │
  └─────────────────────────────────────┘
       │                           │
       ▼                           ▼
  XDP program                 XDP program
  (UMEM tx ring)              (UMEM tx ring)
       │                           │
       ▼                           ▼
  Port 0 send                 Port 1 send
```

## Problem: TCP

HD Protocol runs over TCP. AF_XDP bypasses TCP entirely —
it's raw Ethernet frames. We'd need to either:

1. **Implement TCP in userspace** (DPDK-style) — massive
   effort, reimplements the wheel
2. **Use AF_XDP for UDP only** — HD over UDP instead of
   TCP, with our own reliability layer
3. **Use AF_XDP for the relay hop only** — clients connect
   via normal TCP, but the relay-to-relay or internal
   forwarding uses AF_XDP raw frames
4. **Hybrid: kernel TCP for connection setup, AF_XDP for
   data** — splice the fd to AF_XDP after handshake

Option 4 is what we want but it doesn't exist — you can't
splice a TCP connection into AF_XDP.

## Realistic Architecture: UDP Mode

HD Protocol over UDP with AF_XDP:

```
Client A ──TCP──▶ Relay (enrollment, rules)
Client A ──UDP──▶ Relay ──AF_XDP──▶ Client B ◀──UDP──
```

1. Clients connect via TCP for enrollment (existing path)
2. After approval, clients send HD Data frames over UDP
3. Relay receives UDP via AF_XDP (zero-copy)
4. Relay forwards to destination's UDP endpoint via AF_XDP
5. No TCP on the data path at all

This is essentially TURN ChannelData but without the TURN
framing — just raw HD frames in UDP packets.

### Why This Works

- WireGuard is already UDP — the payload is encrypted
  WireGuard packets, which handle their own reliability
- The relay doesn't need TCP reliability — it's a
  stateless forwarder
- AF_XDP gives us zero-copy UMEM ring access
- The ConnectX-4 mlx5 driver supports AF_XDP natively

### Frame Format (UDP mode)

```
[Ethernet][IP][UDP][HD Frame]
                   ├─ Type (1B)
                   ├─ Len (3B)
                   └─ Payload (WireGuard packet)
```

No TCP framing, no TLS, no reassembly buffers needed.
Each UDP packet = one HD frame = one WireGuard packet.

## Implementation

### 1. AF_XDP Socket Setup

```cpp
struct AfXdpPort {
  int xsk_fd;
  struct xsk_socket *xsk;
  struct xsk_umem *umem;
  void *umem_area;       // mmap'd UMEM region
  struct xsk_ring_prod tx;
  struct xsk_ring_cons rx;
  struct xsk_ring_prod fq;  // Fill queue
  struct xsk_ring_cons cq;  // Completion queue
  int ifindex;
  int queue_id;
};
```

### 2. UMEM Configuration

- Frame size: 2048 (fits Ethernet + IP + UDP + HD frame)
- Frame count: 65536 (128 MB total UMEM per port)
- Two ports = 256 MB total

### 3. Forwarding Loop (per worker)

```cpp
while (running) {
  // Receive batch from AF_XDP.
  unsigned int rcvd = 0;
  uint32_t idx_rx = 0;
  rcvd = xsk_ring_cons__peek(&port->rx, BATCH, &idx_rx);

  for (unsigned i = 0; i < rcvd; i++) {
    const struct xdp_desc *desc =
        xsk_ring_cons__rx_desc(&port->rx, idx_rx + i);
    uint8_t *pkt = xsk_umem__get_data(
        port->umem_area, desc->addr);

    // Parse Ethernet + IP + UDP headers.
    // Extract HD frame from UDP payload.
    // Lookup cached destination.
    // Rewrite Ethernet dst MAC, IP dst, UDP dst port.
    // Submit to TX ring of the destination port.
  }

  xsk_ring_cons__release(&port->rx, rcvd);

  // Submit TX batch.
  sendto(port->xsk_fd, NULL, 0, MSG_DONTWAIT, NULL, 0);
}
```

### 4. Dual-Port Topology

```
Client subnet A ──▶ Port 0 (10.50.0.1)
                         │
                    Relay (AF_XDP)
                         │
Client subnet B ──▶ Port 1 (10.50.1.1)
```

Or both ports on the same subnet for redundancy/load
balancing.

### 5. Connection Setup (stays on TCP)

1. Client connects via TCP (any port, kernel handles it)
2. HD enrollment + approval via existing path
3. Relay tells client: "send UDP to 10.50.0.1:4000"
4. Client switches to UDP for data frames
5. Relay receives via AF_XDP, forwards via AF_XDP

## Performance Target

AF_XDP on ConnectX-4 with mlx5:
- Single queue: ~10 Mpps (14.8 Gbps at 1400B)
- Multi-queue: ~25 Mpps (37 Gbps at 1400B)
- Two ports: ~50 Mpps (74 Gbps theoretical)

Realistic target on Haswell 6C/12T:
- CPU-limited before NIC-limited
- Target: 40+ Gbps combined (2x current 19.9 Gbps)

## Build Requirements

- libbpf 1.0+ (already have)
- libxdp 1.4+ (for AF_XDP helper functions)
- Kernel 5.19+ (already have 6.12)
- mlx5 driver (already loaded)
- CAP_NET_ADMIN + CAP_BPF

## Implementation Order

1. AF_XDP socket wrapper (xdp_socket.h/cc)
2. UDP HD frame parser
3. Single-port AF_XDP recv + forward loop
4. Dual-port with cross-port forwarding
5. Integration with existing TCP enrollment path
6. Benchmark: AF_XDP vs TCP relay
