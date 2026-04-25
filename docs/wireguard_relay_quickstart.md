# WireGuard relay quickstart

Set up a hyper-derp daemon running in **WireGuard relay mode**, register two peers from inside the einheit CLI, and connect stock `wg-quick` clients through it. No `hd-wg`, no client-side daemon — the clients run vanilla WireGuard and don't know they're being relayed.

The relay is transparent: it observes only the public WireGuard framing (MAC1 on handshake init/response to identify the destination, `receiver_index` on data packets) and forwards UDP. No crypto, no peer private keys, no plaintext access.

```
┌─ hd-c1 (10.99.0.1) ─┐                                  ┌─ hd-c2 (10.99.0.2) ─┐
│ wg-quick (stock)    │ ──UDP──► hd-r2 (relay) ──UDP──► │ wg-quick (stock)    │
└─────────────────────┘  :51820                           └─────────────────────┘
```

For the design rationale and the alternative client-side `hd-wg` approach see [wireguard_mode.md](wireguard_mode.md) and [hd_wg.md](hd_wg.md). This doc covers the now-implemented relay-side path.

## Prerequisites

**Relay host (hd-r2):**

- The hyper-derp binary built from this tree (`cmake --build build -j --target hyper-derp`).
- Port 51820/UDP open between the relay and each client.
- Operator can drive the einheit CLI against the daemon (typically `EINHEIT_ROLE=admin` set in the shell).

**Each client (hd-c1, hd-c2):**

- Linux kernel with the `wireguard` module (`sudo modprobe wireguard`).
- `wireguard-tools` for `wg` / `wg-quick` (`sudo apt-get install wireguard-tools`).
- Root or `CAP_NET_ADMIN` to bring up `wg0`.

There is nothing else for the clients to install. They are entirely stock WireGuard.

## Step 1 — Configure the daemon for WG mode

`~/hd/hyper-derp.yaml` on the relay:

```yaml
mode: wireguard
wg_relay:
  port: 51820
  roster_path: /home/worker/hd/wg-roster
log_level: info
einheit:
  ctl_endpoint: ipc:///tmp/einheit/hd-relay.ctl
```

`mode: wireguard` makes the daemon run a UDP forwarder instead of the DERP/HD path. `roster_path` is where peer add/remove will be persisted; without it the roster is in-memory only and disappears on restart. Mode is selected at startup — switching back to DERP requires a yaml edit + restart.

Start the daemon however you normally do (the user-mode systemd unit at `dist/hyper-derp.user.service` works as-is — it doesn't care which mode the yaml selects).

## Step 2 — Log into the einheit CLI

```bash
ssh hd-r2-cli              # or however you've aliased it
```

Confirm the daemon is up and in the right mode:

```
worker@hd-relay > wg show
```

```
field             value
peers             0
sessions          0
rx_packets        0
fwd_packets       0
drop_no_dst       0
drop_bad_form     0
drop_no_session   0
```

If `wg show` returns `not_wg_mode`, the daemon is running DERP mode — fix `mode:` in the yaml and restart.

## Step 3 — Generate WireGuard keys on each client

Standard `wg` workflow. From any shell on the client:

```bash
wg genkey | tee privatekey | wg pubkey > publickey
cat publickey
```

Repeat on the second client. Save both pubkeys; you'll need each peer's public key in the *other* client's `[Peer]` block.

## Step 4 — Write `/etc/wireguard/wg0.conf` on each client

These are completely standard WireGuard config files. The clients peer with each other (not with the relay); the relay's address is just the `Endpoint`.

`hd-c1` (sets up 10.99.0.1, peers with `<C2_PUB>`):

```ini
[Interface]
PrivateKey = <C1_PRIV>
Address = 10.99.0.1/24
ListenPort = 51820

[Peer]
PublicKey = <C2_PUB>
AllowedIPs = 10.99.0.2/32
Endpoint = <relay-ip>:51820
PersistentKeepalive = 25
```

`hd-c2` (sets up 10.99.0.2, peers with `<C1_PUB>`):

```ini
[Interface]
PrivateKey = <C2_PRIV>
Address = 10.99.0.2/24
ListenPort = 51820

[Peer]
PublicKey = <C1_PUB>
AllowedIPs = 10.99.0.1/32
Endpoint = <relay-ip>:51820
PersistentKeepalive = 25
```

`ListenPort = 51820` is required if you want to register the client with a pinned source port (Step 5). Drop it for ephemeral source ports — see *Notes* below.

## Step 5 — Register the peers via the einheit CLI

The relay needs each peer's pubkey-as-hex (not the base64 form `wg pubkey` emits) and the source `host:port` it'll dial in from. Convert one-shot:

```bash
echo "<base64-pub>" | base64 -d | xxd -p -c64
```

Then in the CLI on the relay:

```
worker@hd-relay > wg peer add 3361221a0ba4c0ba0b5dc49e718355bb9a0084bf8c2ed5870ddc793226e6e165 192.168.122.189:51820 alice
worker@hd-relay > wg peer add 0aee01dd9724b5e44d0acd8a771a3eb7b7982d304453ae52ce48b165fd25b57f 192.168.122.239:51820 bob
worker@hd-relay > wg peer list
```

```
field              value
peer.0.pubkey      3361221a0ba4c0ba0b5dc49e718355bb9a0084bf8c2ed5870ddc793226e6e165
peer.0.endpoint    192.168.122.189:51820
peer.0.label       alice
peer.1.pubkey      0aee01dd9724b5e44d0acd8a771a3eb7b7982d304453ae52ce48b165fd25b57f
peer.1.endpoint    192.168.122.239:51820
peer.1.label       bob
peer.count         2
```

The roster is persisted to the file you set as `roster_path` and replayed on every restart. You can sanity-check with `cat ~/hd/wg-roster`.

To remove later: `wg peer remove <pubkey-hex>`.

## Step 6 — Bring up `wg0` on each client

```bash
sudo wg-quick up wg0
```

Run this on both clients. From either side:

```bash
ping -c 4 10.99.0.2          # from hd-c1
```

You should see four replies, ~1.5 ms RTT in a libvirt-fleet test, ~5–30 ms over a real link.

`wg show wg0` on either side reports a recent handshake and counters incrementing. The `Endpoint` field shows the relay's IP — that's the only thing the client knows; it has no idea who actually delivered the packet.

## Step 7 — Watch it from the CLI

```
worker@hd-relay > wg show
```

```
field              value
peers              2
sessions           4
rx_packets         16
fwd_packets        16
drop_no_dst        0
drop_bad_form      0
drop_no_session    0
```

`sessions` grows during handshake observation (each handshake establishes one session id per direction). `rx_packets` is everything that hit the relay's UDP socket; `fwd_packets` is what got forwarded. The drop counters tell you whether you're seeing malformed traffic, packets for unknown destinations, or data packets that arrived before the relay observed their handshake (typically transient at startup).

## Notes

**Pinned vs. learned endpoints.** The current implementation requires a configured endpoint per peer (`wg peer add` with `host:port`). The relay routes destination traffic to that exact endpoint. If a client roams or uses an ephemeral source port, the operator has to update the endpoint with another `wg peer add` (it replaces in place). Endpoint learning — the relay deducing peer endpoints from observed handshakes — is a follow-up.

**Auth modes.** This v0 has no source-side auth check beyond MAC1 matching the destination pubkey. Spoofing requires knowing the destination peer's pubkey (which is public, so anyone can probe), but the spoofer has to match an existing receiver_index for data packets. The `pin` and `allowlist` modes from the design doc that bind source IP/CIDR to a pubkey aren't built yet; in production-facing deployments the relay should sit behind a firewall that already restricts who can reach UDP/51820.

**Crypto.** The relay never decrypts. `wireguard.ko` on each client does the Noise handshake end-to-end with the other client. The relay's MAC1 computation is publicly recomputable — it's a keyed BLAKE2s where the key is derived from the destination's *public* key.

**Multi-worker / kTLS.** Doesn't apply here. WG mode is a separate UDP forwarder on its own port, single thread, no DERP/kTLS plumbing.

**Coexisting with DERP.** A single hyper-derp instance picks one mode at startup. To run both DERP and WG on the same host, run two daemons on different ports — they don't interfere.

## Troubleshooting

**`wg show` returns `not_wg_mode`** — daemon is in DERP mode. Edit yaml `mode: wireguard`, restart.

**Ping replies but `wg show` reports `peers=0`** — daemon was restarted between `wg peer add` and the ping; either you didn't set `roster_path` (so nothing was persisted) or systemd restarted under you. Add `roster_path:` to the yaml and re-add the peers.

**`drop_no_dst` increments** — handshake came in for a pubkey not in your roster. Either a stranger's WG client is probing you, or you forgot to register a peer.

**`drop_no_session` increments** — data packets arrived for a `receiver_index` the relay hasn't seen a handshake for. Usually the peer rebooted and the relay still has the old session table; clears up when WG rekeys (~2 min) or as soon as a new handshake completes.

**MAC1 scan cost** — `O(N)` per handshake against the roster. Negligible at fleet scale (sub-microsecond per pubkey on modern hardware); becomes worth caching past ~1000 peers, but that's outside the v0 envelope.
