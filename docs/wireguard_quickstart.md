# WireGuard relay quickstart

Walkthrough for a minimal hd-wg setup: one hyper-derp relay plus two clients that get a kernel WireGuard tunnel between them, with all relay-side admin done through the einheit CLI.

This is the *client-side* hd-wg model. The hyper-derp relay is unmodified — it just forwards HD MeshData. WG crypto is performed by `wireguard.ko` on each client. The relay never sees plaintext.

```
hd-c1 (10.99.0.1) ── wg.ko ── 127.0.0.1:51821 (hd-wg proxy)
                      │
                      └─── HD MeshData (TLS) ───► hd-r1 ───► hd-c2 (10.99.0.2)
```

For background on what hd-wg is doing under the hood (state machine, fall-back, candidates, force-relay) see [hd_wg.md](hd_wg.md).

## Prerequisites

**On the relay host (hd-r1):**
- `hyper-derp` running, HD enabled, kTLS up. The CLI handbook covers this — see [cli_handbook.md](cli_handbook.md).
- The relay's `hd.relay_key` (HMAC shared secret) — clients need it.
- A reachable IP and port (defaults: 23340 in the test fleet, 3341 in `dist/hd-wg.yaml`).

**On each client (hd-c1, hd-c2):**
- Linux kernel with `wireguard` and `tls` modules (`sudo modprobe wireguard tls`; persist with `/etc/modules-load.d/`).
- `wireguard-tools` (for `wg genkey` / `wg show`): `sudo apt-get install -y wireguard-tools`.
- The `hd-wg` binary copied into `~/bin/`. Build from this tree (`cmake --build build -j --target hd-wg`) and `scp build/client/hd-wg <client>:~/bin/hd-wg && ssh <client> chmod +x ~/bin/hd-wg`. There is no Debian package for it yet.
- Root or `CAP_NET_ADMIN` (hd-wg drives netlink to bring up `wg0`).

## Step 1 — Confirm the relay is healthy

From your workstation, drop into the einheit shell on the relay:

```bash
ssh hd-r1-cli       # (the *-cli alias; see cli_handbook.md)
```

Or one-shot:

```bash
ssh hd-r1 hd-cli show status
```

You're looking for `status=ok`, `hd_enabled=true`, and a sane `hd_relay_id`. If the relay isn't running, `daemon start` (local) or fix the systemd unit before continuing — there's nothing more to do here until the daemon answers.

## Step 2 — Set the relay to auto-approve

For a minimal demo, let the relay enroll new HD peers without operator approval. From the einheit shell on hd-r1:

```
configure
set hd.enroll_mode auto
commit
```

Or as a one-liner from outside the shell:

```bash
ssh hd-r1 'printf "configure\nset hd.enroll_mode auto\ncommit\nexit\n" | hd-cli'
```

This persists into the commit log (`~/hd/commits.log` if `einheit.commit_log_path` is set), so the setting survives daemon restarts. To switch back to operator-gated enrollment later: `set hd.enroll_mode manual`.

## Step 3 — Generate WireGuard keys on each client

`hd-wg` expects WG private keys as 64-character hex (not `wg genkey`'s base64). Convert in one shot:

```bash
mkdir -p ~/hdwg
wg genkey | base64 -d | xxd -p -c64 > ~/hdwg/wg.priv.hex
xxd -r -p ~/hdwg/wg.priv.hex | base64 | wg pubkey > ~/hdwg/wg.pub
cat ~/hdwg/wg.pub
```

The public key is for your records — peers discover each other through HD `WGEX` messages, so you don't have to wire up a separate WireGuard `[Peer]` config.

## Step 4 — Write the hd-wg config on each client

`/home/worker/hdwg/hd-wg.yaml` on **hd-c1**:

```yaml
relay:
  host: 192.168.122.13       # hd-r1
  port: 23340
  key: "1a411c2e74b05bb985af2e53606344ec7b0783d41ec91f920bf70393759b2e27"
wireguard:
  private_key: "<contents of ~/hdwg/wg.priv.hex>"
  interface: wg0
  listen_port: 51820
tunnel:
  cidr: 10.99.0.1/24         # this client's tunnel address
proxy:
  port: 51821
keepalive: 25
force_relay: 1                # 1 = always relay (skip ICE)
```

Same on **hd-c2** with `tunnel.cidr: 10.99.0.2/24` and its own private key.

`force_relay: 1` is the simplest path for a first run — no STUN, no candidate exchange. Drop it later to let hd-wg promote to direct UDP when peers can reach each other.

`force_relay` is **int**, not bool. ryml-cxx rejects `true` / `false` here.

## Step 5 — Start hd-wg on both clients

```bash
sudo modprobe wireguard
sudo setsid nohup hd-wg --config ~/hdwg/hd-wg.yaml \
  > ~/hdwg/hd-wg.log 2>&1 < /dev/null &
```

A successful startup log:

```
[info] loaded config from /home/worker/hdwg/hd-wg.yaml
[info] hd-wg starting (force-relay)
[info] wireguard interface wg0 created
[info] wireguard wg0 addr 10.99.0.1/24 up
[info] wg0 mtu set to 1380
[info] enrolled with relay (peer_id=0)
[info] wg proxy listening on 127.0.0.1:51821
[info] hd-wg running (tunnel 10.99.0.1/24, proxy 127.0.0.1:51821, mtu 1380)
```

When the second client comes up, both logs gain peer-discovery lines:

```
[info] peer 9 appeared
[info] peer 9 wg tunnel 10.99.0.2 (wgex done, waiting for candidates)
[info] peer 9 no candidates, using relay
```

(`no candidates` is expected with `force_relay: 1` — both sides skip ICE on purpose.)

## Step 6 — Watch enrollment from the relay

Back on hd-r1's CLI, the two clients are now enrolled HD peers:

```bash
ssh hd-r1 hd-cli show peers
```

```
key                                                              state
ck_<hex…>                                                        approved
ck_<hex…>                                                        approved
```

`show counters` confirms enrollments and zero auth failures:

```bash
ssh hd-r1 hd-cli show counters
```

```
hd.enrollments       2
hd.auth_failures     0
```

Once traffic flows, `hd_mesh_forwards_total` (Prometheus, on `:29100/metrics`) increments — that's WG UDP being relayed through HD MeshData.

## Step 7 — Test the tunnel

From hd-c1:

```bash
ping -c 4 10.99.0.2
```

```
64 bytes from 10.99.0.2: icmp_seq=1 ttl=64 time=1.7 ms
64 bytes from 10.99.0.2: icmp_seq=2 ttl=64 time=2.0 ms
…
```

`sudo wg show wg0` on each side should show a recent handshake:

```
peer: izgNrXXwmscLcpv883pThUt8iv8IjD2djMQqoDLck2U=
  endpoint: 127.0.0.1:51821
  allowed ips: 10.99.0.2/32
  latest handshake: 16 seconds ago
  transfer: 636 B received, 692 B sent
```

`endpoint: 127.0.0.1:51821` is the local hd-wg proxy — wg.ko sends UDP to the proxy, the proxy wraps it as HD MeshData, the relay forwards to the other client's proxy, which unwraps and hands it to that side's wg.ko. A typical relayed RTT in the test fleet is ~1.5–2ms.

## Tearing it down

```bash
sudo pkill -x hd-wg
sudo ip link del wg0
```

To revoke a peer from the relay side:

```
ssh hd-r1 hd-cli --replay <(printf 'peer revoke ck_<hex>\nexit\n')
```

Or interactively in the shell, `peer revoke <key>`.

## Troubleshooting

**`error: invalid WG private key`** — `wireguard.private_key` is base64, not hex. Re-encode (Step 3).

**`could not deserialize value`** — `force_relay` is int, not bool. Use `1` or `0`.

**Clients enrolled but `show peers` is empty** — relay is in `enroll_mode: manual` and the peers haven't been approved. Either flip to `auto` (Step 2) or `peer approve <key>` for each.

**Tunnel up but ping hangs** — check `hd_mesh_forwards_total` on the relay's `/metrics`. Static at 0 means hd-wg's proxy isn't pushing frames; non-zero means the relay is forwarding but the receiving side isn't decoding (mismatched WG keys, MTU issue).

**`hd connect: Connection refused`** in hd-wg log — relay isn't listening on the configured port, or the firewall is in the way. Confirm with `ss -ltn | grep <port>` on the relay.

**`hd upgrade: ...`** — the relay's TLS material is missing or kTLS isn't usable on the client side. Check the relay log for `kTLS enabled` at startup, and the client kernel for `lsmod | grep ^tls`.

## What this isn't

- Not Internet-grade. The fleet here uses a self-signed kTLS cert and unauthenticated auto-approve. For production you want operator-gated enrollment, signed fleet bundles (see `docs/fleet_control_plane.md`), and a real CA.
- Not direct-by-default. `force_relay: 1` was set so the demo doesn't depend on UDP reachability between clients. Drop it (or set to `0`) and configure a STUN server for a real ICE candidate exchange. The state machine in [hd_wg.md](hd_wg.md) covers the direct path.
- Not a relay-side WG implementation. The hyper-derp relay does *not* understand WireGuard packets; it forwards opaque HD MeshData. A separate relay-side WG mode is sketched in `docs/wireguard_mode.md` but not built.
