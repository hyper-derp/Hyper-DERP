# WireGuard Relay Mode — Operator Quickstart

This walks through bringing up `hyper-derp` as a transparent UDP relay for stock WireGuard clients (`wg`, `wg-quick`, pfSense, the mobile WireGuard apps, …) and getting two clients talking through it.

The clients run vanilla WireGuard. They don't know `hyper-derp` exists; they just point their `Endpoint =` at the relay. The relay knows nothing about WireGuard semantics — no MAC1, no session tracking, no crypto. It forwards UDP based on a 4-tuple match against an operator-managed peer + link table.

> Not the same product as the HD-aware path (`mode: derp` and `hd-wg`). See [hd_wg.md](hd_wg.md) for that. They can coexist on different ports; this doc covers `mode: wireguard` only.

## Mental model

```
       wg-quick on alice                wg-quick on bob
   (10.99.0.1/24, port 51820)       (10.99.0.2/24, port 51820)
              │                                │
              │  Endpoint = relay:51820        │  Endpoint = relay:51820
              ▼                                ▼
            ┌────────────────────────────────────┐
            │           hyper-derp              │
            │     mode: wireguard, :51820       │
            │                                    │
            │   peer table (operator-set):       │
            │     alice  192.168.122.189:51820   │
            │     bob    192.168.122.239:51820   │
            │   link table:                      │
            │     alice ↔ bob                    │
            └────────────────────────────────────┘
```

Forwarding rule: a packet whose source 4-tuple matches a registered peer's pinned endpoint is sent to that peer's link partner's endpoint. Iteration-1 invariant — each peer is in **at most one link** so the destination is unambiguous from the source alone. Multi-link mesh routing is future work.

## Relay setup

### Config

Drop a yaml at `/etc/hyper-derp/hyper-derp.yaml` (or wherever the systemd unit points):

```yaml
mode: wireguard
wg_relay:
  port: 51820
  roster_path: /var/lib/hyper-derp/wg-roster
log_level: info
einheit:
  ctl_endpoint: ipc:///tmp/einheit/hd-relay.ctl
  pub_endpoint: ipc:///tmp/einheit/hd-relay.pub
```

`mode: wireguard` skips the entire DERP/HD plumbing — no TLS, no kTLS, no HD peers, no metrics server. The daemon binds the single UDP port in `wg_relay.port` and runs the einheit channel for operator commands. `roster_path` is rewritten atomically on every successful `wg peer add` / `wg link add` and reloaded on startup.

You can pre-seed peers + links from yaml; entries here win on name collisions during reload:

```yaml
wg_relay:
  port: 51820
  roster_path: /var/lib/hyper-derp/wg-roster
  peers:
    - { name: alice, endpoint: 192.168.122.189:51820, label: alice-laptop }
    - { name: bob,   endpoint: 192.168.122.239:51820, label: bob-laptop }
  links:
    - { a: alice, b: bob }
```

### Systemd drop-in

The shipped `hyper-derp.service` is hardened for the DERP path: `DynamicUser=yes`, `ProtectSystem=strict`, `PrivateTmp=yes`. With those defaults the daemon **cannot** write `/var/lib/hyper-derp/wg-roster` (no state dir) and the einheit IPC under `/tmp/einheit/` is invisible to anything else on the host (private namespace). You need a small drop-in to make WG mode actually usable:

```bash
sudo systemctl edit hyper-derp
```

```ini
[Service]
# Writable state dir for the wg roster.
# Becomes /var/lib/hyper-derp owned by the DynamicUser
# (and bind-mounted to /var/lib/private/hyper-derp on
# disk; both paths are stable across DynamicUser
# rotations).
StateDirectory=hyper-derp

# Share /tmp/einheit between the daemon's PrivateTmp
# namespace and the host so hd-cli can reach the IPC
# socket. Keeps the rest of /tmp private.
BindPaths=/tmp/einheit

# UMask=000 so ZeroMQ creates the IPC sockets mode 0775
# rather than 0755. Without it, hd-cli (running as your
# operator user) cannot connect to a socket owned by the
# daemon's per-instance DynamicUser. /tmp/einheit is a
# host-local control plane, not a security boundary.
UMask=000

# Unlink stale IPC socket files from a previous instance
# before binding; otherwise ZeroMQ fails with
# EADDRINUSE on the .pub socket.
ExecStartPre=/bin/rm -f /tmp/einheit/hd-relay.ctl /tmp/einheit/hd-relay.pub
```

Then create the shared IPC dir once (host side, before first start) — mode 1777 because the daemon's DynamicUser and your operator user both need to drop sockets there:

```bash
sudo install -d -m 1777 /tmp/einheit
```

### Start it

```bash
sudo systemctl daemon-reload
sudo systemctl restart hyper-derp
journalctl -u hyper-derp -n 20 --no-pager
```

You want lines like:

```
hyper-derp starting in wireguard relay mode on UDP :51820
wg-relay listening on UDP :51820 (0 peers, 0 links)
einheit channel listening on ipc:///tmp/einheit/hd-relay.ctl
```

### Running outside systemd (dev/fleet testing)

For test rigs you can skip systemd entirely and run the binary directly under any user that owns the roster path and `/tmp/einheit/`:

```bash
nohup hyper-derp --config ./hyper-derp.yaml > hyper-derp.log 2>&1 &
```

This is what the in-tree fleet test (`hd-r2`) uses; it sidesteps every hardening interaction at the cost of the protections the systemd unit gives you.

## Operator workflow (hyper-derp CLI)

All mutations happen through the hyper-derp CLI. From the relay host:

```bash
hd-cli wg peer add alice 192.168.122.189:51820 alice-laptop
hd-cli wg peer add bob   192.168.122.239:51820 bob-laptop
hd-cli wg link add alice bob
```

That's enough for forwarding to start. The clients can begin sending traffic the moment those three commands return — the relay matches the source endpoint, looks up the link, and forwards.

To see what's there:

```bash
hd-cli wg peer list      # name, endpoint, pubkey, link, counters
hd-cli wg link list      # configured A↔B pairs
hd-cli wg show           # aggregate counters + roster size
```

### Optional: pubkey for `show config`

You can also paste each peer's WireGuard public key (base64, as `wg genkey | wg pubkey` emits) into the relay. **The relay never inspects this** — it's purely metadata that lets `wg show config` render a complete `[Peer]` block for the operator to copy onto a client.

```bash
hd-cli wg peer pubkey alice 2R7TcAy1njtLPGr0Eu/RWvVcYoDADXZERFfPA7nIJhs=
hd-cli wg peer pubkey bob   +9/gqzMIAGlsHZ1Y6c5ji02v4ymdwnGOoVEonN3uWiY=
```

> The path is `wg peer pubkey`, not `wg peer key` — the einheit CLI's oneshot parser conflates the bare token `key` with its standard `key=...` arg slot. Interactive mode handles either, but oneshot needs the longer name.

### Render a wg-quick `[Peer]` block

```bash
hd-cli wg show config alice 192.168.122.83:51820
```

The second argument is the relay's advertised host:port — what you want to appear in the rendered `Endpoint =` line.

Before you've set the partner's pubkey, the output reminds you to do it:

```
[Interface]
# PrivateKey = (paste your `wg genkey` output)
# Address    = (your tunnel IP, e.g. 10.99.0.1/24)
ListenPort   = 51820

# bob (bob-laptop)
[Peer]
# PublicKey           = (set with `wg peer pubkey bob <pubkey>` on the relay)
AllowedIPs          = (your call)
Endpoint            = 192.168.122.83:51820
PersistentKeepalive = 25
```

After `wg peer pubkey bob <pubkey>` the `[Peer]` block is complete:

```
[Peer]
PublicKey           = +9/gqzMIAGlsHZ1Y6c5ji02v4ymdwnGOoVEonN3uWiY=
AllowedIPs          = (your call)
Endpoint            = 192.168.122.83:51820
PersistentKeepalive = 25
```

The `[Interface]` block has placeholders because the relay has no business assigning tunnel IPs or holding private keys — that's between the WG peers and the operator running them.

## Client setup (stock wg-quick)

On each client, write `/etc/wireguard/wg0.conf`:

```ini
# alice — /etc/wireguard/wg0.conf
[Interface]
PrivateKey = <alice-private-key>
Address    = 10.99.0.1/24
ListenPort = 51820

[Peer]
PublicKey           = <bob-public-key>
AllowedIPs          = 10.99.0.2/32
Endpoint            = <relay-public-ip>:51820
PersistentKeepalive = 25
```

```ini
# bob — /etc/wireguard/wg0.conf
[Interface]
PrivateKey = <bob-private-key>
Address    = 10.99.0.2/24
ListenPort = 51820

[Peer]
PublicKey           = <alice-public-key>
AllowedIPs          = 10.99.0.1/32
Endpoint            = <relay-public-ip>:51820
PersistentKeepalive = 25
```

The two `Endpoint =` lines both point at the relay. Each peer's `[Peer] PublicKey` is the **other client's** key, not the relay's — the relay does not terminate the tunnel.

Bring it up:

```bash
sudo wg-quick up wg0
```

Verify:

```bash
ping 10.99.0.2     # from alice
sudo wg show       # latest handshake should be recent, transfer should grow
```

On the relay:

```bash
hd-cli wg show
# rx_packets and fwd_packets climb in lockstep; drops stay at 0.
```

## Hard requirement: pinned source endpoints

The relay identifies a peer **by its source 4-tuple**, not by anything inside the WG packet. That means:

- **The peer's outbound source port must be stable.** WireGuard sends from its `ListenPort`, so set `ListenPort = <something fixed>` on every client. Without it, ephemeral source ports will arrive on the relay and `drop_unknown_src` will tick.
- **The peer's source IP must match what you registered.** Behind a NAT, register the public IP:port the relay actually sees. (TODO: a future iteration may let operators bind to `0.0.0.0` and learn endpoints from a one-shot enrollment exchange.)

If forwarding silently fails, the first thing to check is the relay's `drop_unknown_src` counter. If it's growing, the peer's source 4-tuple does not match the registered endpoint. To see exactly what arrives:

```bash
sudo tcpdump -i any -nn udp port 51820
```

Each accepted packet should produce two lines — one inbound from a client, one outbound to its link partner. If you only see the inbound, either the source doesn't match a registered peer (`drop_unknown_src`) or the peer has no link (`drop_no_link`).

## Operations

### Removing peers and links

```bash
hd-cli wg link remove alice bob
hd-cli wg peer remove alice           # also removes any links involving alice
```

Both are persisted to disk atomically before the call returns.

### Restarts and roster persistence

The roster file (default `/var/lib/hyper-derp/wg-roster`) is rewritten on every successful mutation and replayed on startup. After a `systemctl restart hyper-derp` you should see:

```
wg-relay roster loaded from /var/lib/hyper-derp/wg-roster: 2 peers, 1 links, 0 bad
wg-relay listening on UDP :51820 (2 peers, 1 links)
```

The log line is informational; counters and last-seen timestamps reset on restart, but the peer + link tables, links, and pubkeys don't.

Under the systemd unit, `/var/lib/hyper-derp` is created and owned by the DynamicUser; on disk the data lives at `/var/lib/private/hyper-derp/` (systemd's standard StateDirectory mapping). Either path works for `cat`'ing the roster.

### Counters

`wg show` exposes:

| field | meaning |
| --- | --- |
| `rx_packets` | UDP datagrams received on the relay's port |
| `fwd_packets` | datagrams forwarded to a registered partner |
| `drop_unknown_src` | source 4-tuple did not match any peer |
| `drop_no_link` | source matched a peer but it has no link |

`drop_unknown_src` is the most common signal of a misconfigured client — usually a missing `ListenPort` or NAT-rewritten source.

## XDP fast path

When `wg_relay.xdp_interface` is set, the daemon attaches a BPF program at the NIC ingress hook. The fast-path handles forwarding entirely in the kernel: source-endpoint lookup → MAC learning → header rewrite → `XDP_TX` back out the same NIC. The userspace recv loop stays running and is the cold-start fallback for any packet whose partner MAC has not yet been observed (typically the first one or two packets of a new session).

### Yaml

```yaml
mode: wireguard
wg_relay:
  port: 51820
  roster_path: /var/lib/hyper-derp/wg-roster
  xdp_interface: enp1s0
  # Optional. Defaults to the deb-installed location.
  # xdp_bpf_obj_path: /usr/lib/hyper-derp/wg_relay.bpf.o
```

If `xdp_interface` is absent or the program fails to load, the daemon logs the failure and falls through to the userspace-only path — correctness is unchanged.

### Systemd capabilities

XDP needs three additions to the wg-mode drop-in:

```ini
[Service]
# CAP_BPF: load + manage BPF maps and programs.
# CAP_NET_ADMIN: attach the program to a NIC.
# CAP_SYS_NICE: pre-existing, kept for SQPOLL.
AmbientCapabilities=CAP_BPF CAP_NET_ADMIN CAP_SYS_NICE

# bpf() is not in @system-service.
SystemCallFilter=bpf

# libbpf attaches XDP via netlink.
RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX AF_NETLINK
```

After `systemctl daemon-reload && systemctl restart hyper-derp` the log should read:

```
wg-relay xdp: attached on enp1s0 (ifindex=2, mode=native)
wg-relay listening on UDP :51820 (... xdp=on)
```

`mode=native` means the driver supports XDP directly. `mode=generic` is the SKB-mode fallback — works on any driver but pays the SKB allocation cost; verify the driver if performance matters.

### Counters under XDP

`wg show` adds four counters when XDP is attached:

| field | meaning |
| --- | --- |
| `xdp_attached` | true if the BPF program is live |
| `xdp_rx_packets` | total UDP packets the program saw on `port` |
| `xdp_fwd_packets` | packets forwarded via `XDP_TX` |
| `xdp_pass_no_peer` | source endpoint not registered (userspace counts this as `drop_unknown_src` instead) |
| `xdp_pass_no_mac` | source registered but partner MAC not yet learned; userspace `fwd_packets` should pick these up |

A healthy active tunnel has `xdp_pass_no_mac` at 1 or 2 (the cold-start packets) and stays there; everything afterwards goes via `XDP_TX`.

## Performance: direct vs userspace vs XDP

Numbers from a libvirt fleet (3× Intel Xeon SierraForest VMs, 2 vCPU / 1 GiB each, virtio bridge), `wg-quick` on both clients, MTU 1420, payload 1400 B per CLAUDE.md. Single iperf3 stream, no SO_RCVBUF tuning. Native-mode XDP attaches on `virtio_net`.

### Latency (ICMP, 100 packets at 50 ms)

| | min | avg | max | mdev |
| --- | --- | --- | --- | --- |
| direct | 0.230 ms | **0.646 ms** | 1.128 ms | 0.220 ms |
| relayed (userspace) | 0.307 ms | **1.171 ms** | 2.081 ms | 0.421 ms |
| relayed (XDP) | 0.300 ms | **1.099 ms** | 2.000 ms | 0.403 ms |

For a 5-packet hot path (steady-state, no cold-start mixed in) the XDP relay matches direct: avg 0.66 ms. The 100-packet number is dragged up by occasional userspace fall-throughs and VM scheduling noise, but the variance shape is the same as direct.

### TCP throughput (10 s)

| | bitrate | retransmits |
| --- | --- | --- |
| direct | **6.31 Gbit/s** | 360 |
| userspace | 1.99 Gbit/s | 17 380 |
| **XDP** | **4.08 Gbit/s** | 4 685 |

XDP doubles the userspace ceiling and recovers ~65 % of direct on a single stream. The remaining gap is the cost of one extra NIC bounce — the packet still has to traverse the bridge twice.

### UDP @ 1400 B — sustained loss vs offered rate

| offered rate | direct | userspace | XDP |
| --- | --- | --- | --- |
| 100 Mbit/s | 0 % | 0 % | 0 % |
| 500 Mbit/s | 0.025 % | 0.61 % | 0.027 % |
| 1 Gbit/s | 0.038 % | 3 % | 0.16 % |
| 2 Gbit/s | 0.08 % | saturates | 0.29 % |
| 3 Gbit/s | — | — | 7.1 % |

Userspace had a clean ceiling at ~100 Mbit/s; XDP holds clean to ~2 Gbit/s and only starts losing at 3 Gbit/s. At iteration-1's pain-points (500 Mbit/s, 1 Gbit/s) loss drops by **20–30×**. At 2 Gbit/s XDP is within 4× of direct's loss rate (0.29 % vs 0.08 %), whereas userspace had already collapsed.

### Reproduce

```bash
# server side (one of c1/c2)
iperf3 -s -1 -p 5201

# client side
iperf3 -c <peer-tunnel-ip> -p 5201 -t 10                # TCP
iperf3 -c <peer-tunnel-ip> -p 5201 -u -l 1400 -b 1G -t 10   # UDP @ 1400B
ping -c 100 -i 0.05 -q <peer-tunnel-ip>                 # latency
```

To compare direct vs relayed, swap the `Endpoint =` line in each client's `wg0.conf` and `wg-quick down/up wg0`. To compare userspace vs XDP without rebuilding, comment out `xdp_interface:` in the relay yaml and `systemctl restart hyper-derp`.

## What the relay deliberately does **not** do

- It does not parse WireGuard packets. No MAC1, no session index, no handshake state.
- It does not learn endpoints. The operator pins them.
- It does not assign tunnel IPs. The clients agree among themselves.
- It does not hold WG private keys. Pubkeys are stored only as metadata for `wg show config`.
- It does not attempt mesh routing. Each peer is in at most one link.

These are by design — the goal is to be a transparent UDP middlebox for stock WG clients, not another tunnel manager. Anything richer belongs in `mode: derp` and the HD-aware client path.
