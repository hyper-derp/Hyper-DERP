# hdwg — WireGuard tunnels via HD Protocol

> Not to be confused with [WireGuard relay mode](wireguard_relay_quickstart.md), a
> separate *relay-side* design where the Hyper-DERP server speaks WG
> directly. `hdwg` is a *client-side* daemon that uses the existing HD
> relay as a signaling channel (and optional fallback transport) for
> otherwise-vanilla `wireguard.ko` tunnels.

## What it does

`hdwg` sits next to `wireguard.ko` and:

1. Connects to an HD relay (TLS, HMAC-authenticated).
2. Discovers other enrolled peers via `PeerInfo`.
3. Exchanges WG public keys with each peer via a small
   MeshData message (`WGEX` magic).
4. Tries to set up **direct** peer-to-peer UDP — wg.ko
   talks straight to the peer's public endpoint.
5. Falls back to **relayed** — WG packets wrap in HD
   MeshData and flow through the HD relay — when direct
   fails or isn't possible.

The WG protocol itself is untouched: the kernel module
does all crypto. `hdwg` only controls *who the peer is*
(netlink `SET_PEER`) and *where its UDP goes*.

## Quick start

```sh
sudo hdwg \
  --relay-host relay.example.com \
  --relay-port 3341 \
  --relay-key $(cat relay.hmac.hex) \
  --wg-key $(wg genkey | tee wg.priv) \
  --tunnel 10.99.0.1/24
```

Or with a YAML config:

```yaml
# /etc/hdwg.yaml
relay:
  host: relay.example.com
  port: 3341
  key: "aabbcc..."          # HMAC shared secret
wireguard:
  private_key: "eeff00..."  # 32-byte hex
  interface: wg0
  listen_port: 51820
tunnel:
  cidr: 10.99.0.1/24
proxy:
  port: 51821               # local UDP bridge
keepalive: 25
force_relay: false          # skip ICE, always use relay
```

```sh
sudo hdwg --config /etc/hdwg.yaml
```

## Data flow

```
Direct path (happy case):
  wg0 (kernel) -- UDP --> peer_ip:51820

Relay path (force_relay, sym-NAT, or direct dies):
  wg0 -- UDP --> 127.0.0.1:51821 (hdwg proxy)
      -- MeshData over TLS --> HD relay
      -- MeshData over TLS --> peer's hdwg proxy
      -- UDP --> peer's wg0
```

The proxy binds `127.0.0.1:<proxy-port>` and is added as
wg.ko's `endpoint` only for peers in the relayed state.
Direct peers have wg.ko's `endpoint` set to the peer's
real `ip:port`.

## State machine

```
           PeerInfo         WGEX          candidates        handshake on direct
kNew  --------------->  kWgexSent  --> kWgexDone  -----> kIceChecking  -----------> kDirect
                                       |                  |                           |
                                       | 500ms timeout    | 5s timeout (no fresh      | rx-stall 15s
                                       |   (no candidates | handshake) or iptables    | (WG sees no
                                       |   — peer in      | block                     | reply)
                                       v   force-relay)   v                           v
                                    kRelayed <---------- kRelayed <---- FALL signal from peer
```

Transitions:

- `kNew → kWgexSent`: received `PeerInfo` for this peer,
  sent our `WGEX` + `CAND`.
- `kWgexSent → kWgexDone`: received the peer's `WGEX`.
  Wait for their candidates.
- `kWgexDone → kIceChecking`: received `CAND`; configure
  wg.ko with the direct endpoint, capture handshake time
  as a baseline.
- `kIceChecking → kDirect`: wg.ko reports a fresh
  handshake after the endpoint switch — direct works.
- `kIceChecking → kRelayed`: 5s elapsed without a fresh
  handshake.
- `kWgexDone → kRelayed`: 500ms without any candidates
  (peer is in `force_relay` mode, or candidates were lost).
- `kDirect → kRelayed`: health check every 2s observes
  `tx_bytes` growing while `rx_bytes` has been static for
  15s. Removes + re-adds the WG peer (wipes the stale
  session) and sends a `FALL` MeshData to the remote so
  it resets too.

## Force-relay mode

`--force-relay` or `force_relay: true` skips ICE entirely:

- We never send CAND.
- We ignore any CAND we receive.
- On receiving WGEX, the `kWgexDone → kRelayed` 500ms
  timer fires, wg.ko gets the proxy endpoint, done.

Use this when direct paths are known-unavailable (CGNAT
on both ends, corporate firewall, etc.) or when HD is
explicitly the intended transport.

## Signaling messages

All over HD `MeshData`. First 4 bytes identify the kind.

| Magic | Size | Meaning |
|-------|-----:|---------|
| `WGEX` | 40 B | 32-byte WG pubkey + 4-byte tunnel IP |
| `CAND` | 6–12 B | 1–2 `[ip:4][port:2]` candidate tuples |
| `FALL` | 4 B | "I fell back to relay, please reset your session" |

All other MeshData payloads are opaque WG UDP bytes —
forwarded between the proxy socket and wg.ko.

## CLI

```
hdwg [options]
  --config PATH        YAML config file
  --relay-host HOST    Relay IP
  --relay-port PORT    Relay port (3341)
  --relay-key HEX      HMAC shared secret
  --wg-key HEX         WireGuard private key
  --wg-interface NAME  WG interface (wg0)
  --wg-port PORT       WG listen port (51820)
  --tunnel CIDR        Tunnel address (e.g. 10.99.0.1/24)
  --proxy-port PORT    UDP proxy port (51821)
  --stun SERVER        STUN server (host:port)
  --keepalive SECS     WG persistent keepalive (25)
  --force-relay        Always tunnel through HD (no ICE)
  --help
```

Needs `CAP_NET_ADMIN` (netlink WG config + interface up).

## Measured performance (VM fleet)

| Path | RTT |
|------|----:|
| Direct | 0.6 ms |
| Relayed (force-relay) | 1.5 ms |
| Relayed (ICE fell back) | 1.5 ms |
| Direct → runtime fallback | ~15 s detection window, then 1.5 ms |

Relay path RTT is tuned — the naive `SO_RCVTIMEO` was
adding ~100ms per hop; current loop only blocks on poll,
and only re-enters `HdClientRecvFrame` when the internal
buffer has more bytes.

## Current limitations

- Direct-path promotion can race with the 5s ICE window
  when both peers start simultaneously. WG handshake
  requires outbound traffic on at least one side, so the
  happy path is most reliable when something is pinging.
- Runtime direct → relay fallback has a ~15s detection
  window (intentional hysteresis) and drops packets
  during the transition (~20% loss over that window in
  the VM test).
- The proxy listens on `127.0.0.1` only; both wg.ko and
  the proxy must run in the same network namespace.
