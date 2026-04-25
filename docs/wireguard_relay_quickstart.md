# WireGuard Relay Mode — Operator Quickstart

This walks through bringing up `hyper-derp` as a transparent UDP relay for stock WireGuard clients (`wg`, `wg-quick`, pfSense, the mobile WireGuard apps, …) and getting two clients talking through it.

The clients run vanilla WireGuard. They don't know `hyper-derp` exists; they just point their `Endpoint =` at the relay. The relay knows nothing about WireGuard semantics — no MAC1, no session tracking, no crypto. It forwards UDP based on a 4-tuple match against an operator-managed peer + link table.

> Not the same product as the HD-aware path (`mode: derp` and `hd-wg`). See [hd_wg.md](hd_wg.md) for that. They can coexist on different ports; this doc covers `mode: wireguard` only.

This doc has two halves:

- **[Quickstart from a fresh box](#quickstart-from-a-fresh-box)** — linear copy-paste walkthrough, ~15 minutes from clean Debian.
- **Reference** — [mental model](#mental-model), [XDP fast path](#xdp-fast-path), [performance](#performance-direct-vs-userspace-vs-xdp), [ops](#operations), [what the relay deliberately doesn't do](#what-the-relay-deliberately-does-not-do). Read after the quickstart, or as needed.

---

## Quickstart from a fresh box

Three Linux boxes:

- **`relay`** — runs `hyper-derp` in `mode: wireguard`. Public IP reachable from both clients.
- **`alice`** — first WG client. Stock `wg-quick`. Inner tunnel IP `10.99.0.1`.
- **`bob`** — second WG client. Stock `wg-quick`. Inner tunnel IP `10.99.0.2`.

The procedure below is copy-paste in order. Where placeholders appear (`<RELAY-IP>`, `<ALICE-IP>`, `<BOB-IP>`) substitute the real public-or-routable IPs of each host before pasting.

### 0. Prerequisites

On the relay and both clients:

- Linux kernel **6.6 or newer** (XDP support; older kernels fall back to userspace forwarding which still works but is slower — see [performance](#performance-direct-vs-userspace-vs-xdp)).
- Debian 12+/13+ or any equivalent distro. The daemon is C++23 + libbpf + libsodium; if you're on RHEL-family substitute the package manager and dependency names accordingly.
- `sudo` available.
- The relay needs to permit inbound UDP on port 51820 from the client IPs (firewall, security group, whatever).

Install the runtime dependencies on each box:

```bash
sudo apt update
sudo apt install -y wireguard-tools iperf3 ethtool
```

`wireguard-tools` is required on the clients (not the relay). `iperf3` and `ethtool` are useful for verification + debugging.

### 1. Install `hyper-derp` on the relay

Two paths: installing the deb from a release artifact, or building from source.

**(a) From a release deb**, if you have one:

```bash
sudo apt install -y /path/to/hyper-derp_<version>_amd64.deb
```

The deb installs:

- `/usr/bin/hyper-derp` — the daemon
- `/usr/bin/hd-cli` — operator CLI wrapper (calls `einheit`; see step 2)
- `/etc/hyper-derp/hyper-derp.yaml.example` — example config
- `/usr/lib/hyper-derp/wg_relay.bpf.o` — the XDP BPF program (optional, used only when `xdp_interface` is set)
- `/usr/lib/systemd/system/hyper-derp.service` — systemd unit (enabled by default; we'll add a drop-in for WG mode)
- `/usr/share/hyper-derp/hd_chafa` — branded chafa logo for the `hd-cli` interactive welcome banner. Used automatically; `hd-cli` exports `EINHEIT_LOGO_PATH` pointing at this file when launching `einheit`. To suppress, `unset EINHEIT_LOGO_PATH` before invoking `hd-cli`

**(b) From source** if you don't have a deb:

```bash
git clone https://github.com/hyper-derp/Hyper-DERP.git
cd Hyper-DERP
sudo apt install -y cmake clang libbpf-dev libsodium-dev libzmq3-dev libssl-dev
cmake --preset default && cmake --build build -j
sudo cmake --install build
```

After install, sanity-check:

```bash
hyper-derp --version
# expected: hyper-derp 0.1.x
```

### 2. Install `einheit` (runtime dependency of `hd-cli`)

The operator drives the daemon over an IPC socket using `einheit`, a separate CLI tool that ships from its own project. The `hd-cli` wrapper is installed by the `hyper-derp` deb (see step 1) but it requires the `einheit` binary at runtime.

Install `einheit` from the einheit project's release artefacts (currently distributed alongside `hyper-derp` on internal deployments — ask your deployment lead). Place the binary somewhere on `PATH`, e.g. `/usr/local/bin/einheit` or `/usr/local/sbin/einheit`, and make sure it's executable.

Verify:

```bash
which einheit
# expected: a path; if it prints nothing, einheit isn't on PATH

einheit --version
# expected: a version string
```

If `einheit` isn't on PATH, the `hd-cli` wrapper falls back to the `$EINHEIT_BIN` env var. Set that in `/etc/profile.d/hyper-derp.sh` or systemd drop-in if you keep einheit somewhere unusual:

```bash
export EINHEIT_BIN=/opt/einheit/bin/einheit
```

Don't try to use `hd-cli` yet — the daemon's not running; the IPC socket doesn't exist. We come back to it in step 4.

### 3. Configure the relay

Three config artefacts. All on the relay box.

**3a. The yaml** at `/etc/hyper-derp/hyper-derp.yaml`:

```bash
sudo tee /etc/hyper-derp/hyper-derp.yaml >/dev/null <<'EOF'
mode: wireguard
wg_relay:
  port: 51820
  roster_path: /var/lib/hyper-derp/wg-roster
  # Uncomment to attach the XDP fast path; needs an
  # XDP-capable NIC + the systemd capability bits in
  # the drop-in below.
  # xdp_interface: eth0
  # xdp_bpf_obj_path: /usr/lib/hyper-derp/wg_relay.bpf.o
log_level: info
einheit:
  ctl_endpoint: ipc:///tmp/einheit/hd-relay.ctl
  pub_endpoint: ipc:///tmp/einheit/hd-relay.pub
EOF
```

**3b. The systemd drop-in.** The shipped `hyper-derp.service` is hardened for the DERP path: `DynamicUser=yes`, `ProtectSystem=strict`, `PrivateTmp=yes`. With those defaults the daemon cannot write the roster, the einheit IPC isn't visible to `hd-cli`, and (if you enable XDP) `bpf()` is blocked by the seccomp filter. The drop-in fixes all three:

```bash
sudo mkdir -p /etc/systemd/system/hyper-derp.service.d
sudo tee /etc/systemd/system/hyper-derp.service.d/wg-mode.conf >/dev/null <<'EOF'
[Service]
# Writable state dir for the wg roster. systemd creates
# /var/lib/hyper-derp owned by the per-instance
# DynamicUser; the daemon writes through to that.
StateDirectory=hyper-derp

# Bind-mount the host /tmp/einheit dir into the daemon's
# PrivateTmp namespace so hd-cli can reach the IPC
# socket. Keeps the rest of /tmp private.
BindPaths=/tmp/einheit

# UMask=000 so ZeroMQ creates the IPC sockets mode 0775
# rather than 0755. hd-cli runs as your operator user;
# without this it can't write to a socket owned by the
# daemon's DynamicUser. /tmp/einheit is a host-local
# control plane, not a security boundary.
UMask=000

# Unlink stale IPC socket files from a previous run; if
# they're still present ZeroMQ fails with EADDRINUSE.
ExecStartPre=/bin/rm -f /tmp/einheit/hd-relay.ctl /tmp/einheit/hd-relay.pub

# Below: only needed if you enable the XDP fast path
# (xdp_interface set in the yaml above). Safe to leave in
# even when XDP is off.
AmbientCapabilities=CAP_BPF CAP_NET_ADMIN CAP_SYS_NICE
SystemCallFilter=bpf
RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX AF_NETLINK
EOF
```

**3c. The IPC dir on the host side**, mode 1777 because both the daemon's DynamicUser and your operator user need to drop sockets there:

```bash
sudo install -d -m 1777 /tmp/einheit
```

### 4. Start the relay daemon

```bash
sudo systemctl daemon-reload
sudo systemctl restart hyper-derp
sudo journalctl -u hyper-derp -n 20 --no-pager
```

You want to see lines like:

```
hyper-derp starting in wireguard relay mode on UDP :51820
wg-relay listening on UDP :51820 (0 peers, 0 links, xdp=off)
einheit channel ready on ipc:///tmp/einheit/hd-relay.ctl
einheit channel listening on ipc:///tmp/einheit/hd-relay.ctl
```

If `xdp_interface` was set, the line just before "listening" should also read `wg-relay xdp: attached on <iface> (mode=native)` (or `mode=generic` on drivers without native XDP support).

Test that the operator CLI can reach the daemon:

```bash
sudo hd-cli wg show
```

Should print a small key/value table with `port=51820`, `peer_count=0`, `link_count=0`, `xdp_attached=false` (or `true` if you enabled XDP). If you get `oneshot: no matching command` or `Connection refused`, see [common errors](#common-errors-and-what-to-check-first).

### 5. Set up the first WG client (`alice`)

On `alice`:

```bash
# Generate a key pair
wg genkey | tee alice.priv | wg pubkey > alice.pub
chmod 600 alice.priv

# Note alice's public key — you'll paste it into bob's config later
cat alice.pub
```

Write `/etc/wireguard/wg0.conf`:

```bash
sudo tee /etc/wireguard/wg0.conf >/dev/null <<EOF
[Interface]
PrivateKey = $(cat alice.priv)
Address    = 10.99.0.1/24
ListenPort = 51820

[Peer]
PublicKey           = <BOB-PUBLIC-KEY>          # filled in step 6
AllowedIPs          = 10.99.0.2/32
Endpoint            = <RELAY-IP>:51820
PersistentKeepalive = 25
EOF
```

`<RELAY-IP>` is the relay box's public IP. Don't bring `wg0` up yet — bob's pubkey isn't known.

### 6. Set up the second WG client (`bob`)

On `bob`:

```bash
wg genkey | tee bob.priv | wg pubkey > bob.pub
chmod 600 bob.priv
cat bob.pub
```

Write `/etc/wireguard/wg0.conf`:

```bash
sudo tee /etc/wireguard/wg0.conf >/dev/null <<EOF
[Interface]
PrivateKey = $(cat bob.priv)
Address    = 10.99.0.2/24
ListenPort = 51820

[Peer]
PublicKey           = $(cat /path/to/alice.pub)   # paste alice's public key here
AllowedIPs          = 10.99.0.1/32
Endpoint            = <RELAY-IP>:51820
PersistentKeepalive = 25
EOF
```

Now go back to `alice` and fill in `<BOB-PUBLIC-KEY>` in her wg0.conf with the contents of `bob.pub`.

Bring up the WG interface on both:

```bash
# on alice
sudo wg-quick up wg0

# on bob
sudo wg-quick up wg0
```

`wg-quick` automatically picks an `MTU` based on the route to the peer's endpoint, so on cloud NICs (gVNIC, etc.) it'll land on 1380 instead of the default 1420. That avoids the WG-fragmenting-on-1460-MTU footgun. Don't set `MTU =` manually unless you know your underlay needs it.

Don't expect ping to work yet — the relay doesn't know about these peers.

### 7. Register peers + link on the relay

On the relay box:

```bash
# Use whatever IPs the relay actually sees alice and bob coming from.
# In a typical NAT-free setup these are alice's and bob's public IPs.
# If clients are behind NAT, use the post-NAT addresses (you may need
# to inspect tcpdump on the relay to figure them out).
sudo hd-cli wg peer add alice <ALICE-IP>:51820 alice-laptop
sudo hd-cli wg peer add bob   <BOB-IP>:51820   bob-laptop
sudo hd-cli wg link add alice bob
```

**Optional but recommended:** also paste each peer's WG public key into the relay. The relay never inspects it, but it lets `wg show config` render a complete `[Peer]` block on demand for clients you onboard later:

```bash
sudo hd-cli wg peer pubkey alice "$(cat /path/to/alice.pub)"
sudo hd-cli wg peer pubkey bob   "$(cat /path/to/bob.pub)"
```

Verify the relay's view:

```bash
sudo hd-cli wg peer list
sudo hd-cli wg link list
sudo hd-cli wg show
```

`wg show` should now read `peer_count=2`, `link_count=1`. The packet counters are still zero — we haven't sent anything.

### 8. Test with ping

On `alice`:

```bash
ping -c 4 10.99.0.2
```

Should report `4 received, 0% packet loss`. Latency depends on path; same-region cloud is typically <1 ms.

Verify the relay actually saw the traffic:

```bash
sudo hd-cli wg show
```

`rx_packets`/`fwd_packets` should be non-zero and equal (every received packet was forwarded). If XDP is attached, `xdp_fwd_packets` carries the bulk and the userspace `rx_packets`/`fwd_packets` only show the cold-start (first 1-2 packets per direction before MAC learning completes).

That's the relay working end-to-end. From here you can run real workloads over `10.99.0.0/24`, scale to more peers (each gets their own `wg peer add`), or move on to the reference sections below.

### 9. Render a wg-quick `[Peer]` block for new clients

Once you've stamped a peer's pubkey via `wg peer pubkey`, the relay can render a ready-to-paste `[Peer]` block for any client who needs to talk to that peer:

```bash
sudo hd-cli wg show config alice <RELAY-IP>:51820
```

The second argument is the relay's advertised host:port — i.e. what should appear in the rendered `Endpoint =` line. Output looks like:

```
[Interface]
# PrivateKey = (paste your `wg genkey` output)
# Address    = (your tunnel IP, e.g. 10.99.0.1/24)
ListenPort   = 51820

# bob (bob-laptop)
[Peer]
PublicKey           = <bob's-pubkey>
AllowedIPs          = (your call)
Endpoint            = <RELAY-IP>:51820
PersistentKeepalive = 25
```

The `[Interface]` block has placeholders because the relay has no business assigning tunnel IPs or holding private keys. The `[Peer]` block is concrete and copy-pasteable.

---

## Common errors and what to check first

| symptom | cause | fix |
| --- | --- | --- |
| `hd-cli: command not found` | step 2 not done | install `einheit` + drop in the wrapper script |
| `oneshot: no matching command` from `hd-cli` | daemon not running, or IPC socket dir not shared | check `journalctl -u hyper-derp`; ensure `/tmp/einheit/` is mode 1777 and the `BindPaths` drop-in is in place |
| daemon exits immediately, `journalctl` shows `code=killed, status=31/SYS` | seccomp killed `bpf()` | the systemd drop-in's `SystemCallFilter=bpf` line is missing, or `daemon-reload` not run |
| daemon logs `wg-relay xdp: BPF load failed: Permission denied` | `CAP_BPF`/`CAP_NET_ADMIN` not in `AmbientCapabilities` | add to drop-in, `daemon-reload`, restart |
| daemon logs `wg-relay xdp: attach to <iface> failed: Address family not supported by protocol` | `RestrictAddressFamilies` blocks `AF_NETLINK` (libbpf attaches via netlink) | add `AF_NETLINK` to that line in the drop-in |
| daemon logs `wg-relay xdp: 'XXX' has no IPv4 — cross-NIC redirect to it will fall back to userspace` | NIC has no v4 address (yet) | bring the NIC up with `ip addr add ...` before starting the daemon, or restart after IP comes up |
| daemon logs `XDP load failed: ... should be less than or equal to half the maximum number of RX/TX queues` (gVNIC) | gve XDP requires reserving half the channels for XDP_TX | `sudo ethtool -L <iface> rx 1 tx 1` (or `rx 2 tx 2` on bigger machines), then restart |
| `xdp: Operation not supported` (gVNIC on c3/c3d/c4/n4/h3/h4) | DQO_RDA queue format doesn't have XDP support in upstream gve yet | leave `xdp_interface` empty, run on the userspace path |
| ping works but TCP throughput collapses | underlying NIC has MTU < 1500 (e.g. gVNIC's 1460); WG packets fragment | use `wg-quick` (auto-detects path MTU), or `ip link set wg0 mtu 1380` manually |
| ping fails, relay's `xdp_pass_no_peer` or `drop_unknown_src` increments | the source 4-tuple of incoming WG packets doesn't match what you registered (NAT?) | run `tcpdump -i <relay-iface> udp port 51820` on the relay; the source ip:port you see is what to register |
| ping fails, relay's `drop_no_link` increments | peers registered, link not added | `hd-cli wg link add <a> <b>` |
| `wg peer key` returns `oneshot: no matching command` | the einheit CLI's oneshot parser conflates `key` with its arg slot | use the longer form `wg peer pubkey` (we registered both names; this is documented) |

If you're stuck somewhere not on this list, capture `journalctl -u hyper-derp -n 50 --no-pager` and `sudo hd-cli wg show` and `sudo tcpdump -i <relay-iface> -nn -c 20 udp port 51820` and check for whichever is the obvious mismatch.

---

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
            │     alice  <ALICE-IP>:51820        │
            │     bob    <BOB-IP>:51820          │
            │   link table:                      │
            │     alice ↔ bob                    │
            └────────────────────────────────────┘
```

Forwarding rule: a packet whose source 4-tuple matches a registered peer's pinned endpoint is sent to that peer's link partner's endpoint. Iteration-1 invariant — each peer is in **at most one link** so the destination is unambiguous from the source alone. Multi-link mesh routing is future work.

## Hard requirement: pinned source endpoints

The relay identifies a peer **by its source 4-tuple**, not by anything inside the WG packet. That means:

- **The peer's outbound source port must be stable.** WireGuard sends from its `ListenPort`, so set `ListenPort = <something fixed>` on every client. Without it, ephemeral source ports will arrive on the relay and `drop_unknown_src` will tick.
- **The peer's source IP must match what you registered.** Behind a NAT, register the public IP:port the relay actually sees. (TODO: a future iteration may let operators bind to `0.0.0.0` and learn endpoints from a one-shot enrollment exchange.)

If forwarding silently fails, the first thing to check is the relay's `drop_unknown_src` counter. If it's growing, the peer's source 4-tuple does not match the registered endpoint. To see exactly what arrives:

```bash
sudo tcpdump -i any -nn udp port 51820
```

Each accepted packet should produce two lines — one inbound from a client, one outbound to its link partner. If you only see the inbound, either the source doesn't match a registered peer (`drop_unknown_src`) or the peer has no link (`drop_no_link`).

## XDP fast path

When `wg_relay.xdp_interface` is set, the daemon attaches a BPF program at the NIC ingress hook. The fast-path handles forwarding entirely in the kernel: source-endpoint lookup → MAC learning → header rewrite → `XDP_TX` back out the same NIC. The userspace recv loop stays running and is the cold-start fallback for any packet whose partner MAC has not yet been observed (typically the first one or two packets of a new session).

Single-NIC setup (uses `XDP_TX` to bounce out the ingress NIC):

```yaml
mode: wireguard
wg_relay:
  port: 51820
  roster_path: /var/lib/hyper-derp/wg-roster
  xdp_interface: eth0
  # Optional. Defaults to the deb-installed location.
  # xdp_bpf_obj_path: /usr/lib/hyper-derp/wg_relay.bpf.o
```

Dual-NIC setup (uses `XDP_REDIRECT` between two NICs, one per peer):

```yaml
mode: wireguard
wg_relay:
  port: 51820
  roster_path: /var/lib/hyper-derp/wg-roster
  xdp_interface: ens4f0,ens4f1     # comma-separated
```

After registering each peer, pin it to one of the NICs:

```bash
hd-cli wg peer nic alice ens4f0
hd-cli wg peer nic bob   ens4f1
```

If `xdp_interface` is absent or the program fails to load, the daemon logs the failure and falls through to the userspace-only path — correctness is unchanged.

### Counters under XDP

`wg show` adds four counters when XDP is attached:

| field | meaning |
| --- | --- |
| `xdp_attached` | true if the BPF program is live |
| `xdp_rx_packets` | total UDP packets the program saw on `port` |
| `xdp_fwd_packets` | packets forwarded via `XDP_TX` / `XDP_REDIRECT` |
| `xdp_pass_no_peer` | source endpoint not registered (userspace counts this as `drop_unknown_src` instead) |
| `xdp_pass_no_mac` | source registered but partner MAC not yet learned; userspace `fwd_packets` should pick these up |

A healthy active tunnel has `xdp_pass_no_mac` at 1 or 2 (the cold-start packets) and stays there; everything afterwards goes via `XDP_TX` / `XDP_REDIRECT`.

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
| `rx_packets` | UDP datagrams received on the relay's port (userspace path) |
| `fwd_packets` | datagrams forwarded by the userspace path |
| `drop_unknown_src` | userspace saw a packet whose source 4-tuple matches no peer |
| `drop_no_link` | userspace saw a packet from a registered peer that has no link |

When XDP is attached, the userspace counters only count cold-start fallbacks; the bulk of traffic moves via the `xdp_*` counters described above.

`drop_unknown_src` (or `xdp_pass_no_peer`) is the most common signal of a misconfigured client — usually a missing `ListenPort` or NAT-rewritten source.

### Running outside systemd (dev / test rigs)

For test rigs you can skip systemd entirely and run the binary directly under any user that owns the roster path and `/tmp/einheit/`:

```bash
sudo bash -c 'umask 000; nohup /usr/bin/hyper-derp --config /etc/hyper-derp/hyper-derp.yaml > /tmp/hyper-derp.log 2>&1 < /dev/null & disown'
```

This is what the in-tree fleet test (`tests/integration/wg_relay_fleet.sh`) uses; it sidesteps every hardening interaction at the cost of the protections the systemd unit gives you.

## Performance: direct vs userspace vs XDP

Numbers from a libvirt fleet (3× Intel Xeon SierraForest VMs, 2 vCPU / 1 GiB each, virtio bridge), `wg-quick` on both clients, MTU 1420, payload 1400 B per CLAUDE.md. Single iperf3 stream. Native-mode XDP attaches on `virtio_net`. See [`wireguard_relay_bench.md`](wireguard_relay_bench.md) and [`wireguard_relay_bench_25g.md`](wireguard_relay_bench_25g.md) for the full bench reports including 25 GbE silicon and GCP cloud comparisons.

### Tune the receiver before measuring

The first iteration of these numbers was misleading at high rates because XDP_TX delivers packets in tighter softirq bursts than direct WG decryption does, and that burstiness overflowed iperf3's UDP receive socket on the receiver (`/proc/net/snmp` `Udp.RcvbufErrors` accounted for ~88 % of the apparent XDP "loss" at 500 Mbit/s and 1 Gbit/s). Always raise the receiver-side socket buffer for clean numbers:

```bash
# on the iperf3 server side
sudo sysctl -w net.core.rmem_max=33554432

# then on the client
iperf3 -c <peer> -p 5201 -u -l 1400 -b 2G -t 10 -w 8M
```

Without this, you are measuring iperf3's single-threaded read loop, not the relay.

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

XDP doubles the userspace ceiling and recovers ~65 % of direct on a single stream.

### UDP @ 1400 B — sustained loss vs offered rate (with `-w 8M`)

| offered rate | direct | userspace | XDP |
| --- | --- | --- | --- |
| 100 Mbit/s | 0 % | 0 % | 0 % |
| 500 Mbit/s | 0 % | 0.61 % | **0 %** |
| 1 Gbit/s | 0 % | 3 % | **0 %** |
| 2 Gbit/s | 0.061 % | saturates | 0.27 % |

XDP is **indistinguishable from direct** through 1 Gbit/s: every packet that left the sender reached the iperf3 server. Userspace by contrast was already losing 3 % at 1 Gbit/s.

The remaining gap at 2 Gbit/s decomposes (verified via `/proc/net/snmp` + `ethtool -S enp1s0`):

- BPF program drops: **0** (`rx_xdp_drops` flat across the run).
- Receiver socket buffer drops: **0** (`Udp.RcvbufErrors` flat with `-w 8M`).
- Relay's virtio_net TX ring drops: ~400 per blast (`tx_xdp_tx_drops`). The TX ring is 256 entries — virtio's max via ethtool — and at 178 kpps the ring fills during softirq bursts before the host vhost-net thread drains it.

In other words the iteration-2 fast path is doing its job; the residual loss at 2 Gbit/s is the libvirt VM's TX queue depth, fixed by setting `tx_queue_size` higher on the guest's libvirt definition (a host change, not a hyper-derp change).

### Reproduce

```bash
# server side (one of c1/c2)
sudo sysctl -w net.core.rmem_max=33554432
iperf3 -s -p 5201

# client side
iperf3 -c <peer-tunnel-ip> -p 5201 -t 10 -w 8M                  # TCP
iperf3 -c <peer-tunnel-ip> -p 5201 -u -l 1400 -b 2G -t 10 -w 8M # UDP @ 1400B
ping -c 100 -i 0.05 -q <peer-tunnel-ip>                          # latency
```

To compare direct vs relayed, swap the `Endpoint =` line in each client's `wg0.conf` and `wg-quick down/up wg0`. To compare userspace vs XDP without rebuilding, comment out `xdp_interface:` in the relay yaml and `systemctl restart hyper-derp`.

When chasing residual loss, snapshot before/after:

```bash
# relay
sudo /usr/sbin/ethtool -S <iface> | \
    grep -E 'rx_xdp_packets|rx_xdp_drops|tx_xdp_tx|tx_xdp_tx_drops'

# receiver
grep '^Udp:' /proc/net/snmp
```

If `tx_xdp_tx_drops` grows, the relay's virtio TX ring is the bottleneck. If `Udp.RcvbufErrors` grows, the iperf3 server overflowed; raise `rmem_max` and `-w`. `rx_xdp_drops` should always be zero — if it's not, the BPF program was rejected at runtime, which is a real bug.

## What the relay deliberately does **not** do

- It does not parse WireGuard packets. No MAC1, no session index, no handshake state.
- It does not learn endpoints. The operator pins them.
- It does not assign tunnel IPs. The clients agree among themselves.
- It does not hold WG private keys. Pubkeys are stored only as metadata for `wg show config`.
- It does not attempt mesh routing. Each peer is in at most one link.

These are by design — the goal is to be a transparent UDP middlebox for stock WG clients, not another tunnel manager. Anything richer belongs in `mode: derp` and the HD-aware client path.
