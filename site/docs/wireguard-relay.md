---
title: WireGuard Relay
description: Run hyper-derp as a transparent UDP relay for stock WireGuard clients.
sidebar_position: 4
---

# WireGuard Relay — Homelab Quickstart


`hyper-derp` in `mode: wireguard` is a UDP relay for stock WireGuard. Your peers point `Endpoint =` at the relay box; the relay forwards their packets to each other. Useful when two peers are both behind NAT and can't see each other directly.

The clients run vanilla `wg` / `wg-quick`. They don't know the relay exists. The relay doesn't know anything about WireGuard either — it just forwards UDP based on a peer + link table you set up once.

Three boxes:

- **`relay`** — runs `hyper-derp`. Needs a public IP both clients can reach on UDP/51820.
- **`alice`** — first WG peer. Tunnel IP `10.99.0.1`.
- **`bob`** — second WG peer. Tunnel IP `10.99.0.2`.

Substitute the real IPs for `<RELAY-IP>`, `<ALICE-IP>`, `<BOB-IP>` as you go.

## 1. Install on the relay

Grab the deb and install:

```bash
sudo apt install -y /path/to/hyper-derp_<version>_amd64.deb
```

That's everything — the deb pulls in the `einheit` operator CLI, drops a default config at `/etc/hyper-derp/hyper-derp.yaml`, and enables the `hyper-derp` systemd unit. Open UDP/51820 inbound on the relay's firewall.

(From source: `git clone … && cmake --preset default && cmake --build build -j && sudo cmake --install build`. You'll need to run `sudo install -d -m 1777 /tmp/einheit && sudo modprobe wireguard && sudo systemctl daemon-reload` afterwards — those are what the deb's postinst would have done.)

## 2. Configure the relay

Overwrite the default config with your real one:

```bash
sudo tee /etc/hyper-derp/hyper-derp.yaml >/dev/null <<'EOF'
mode: wireguard
wg_relay:
  port: 51820
  roster_path: /var/lib/hyper-derp/wg-roster
log_level: info
einheit:
  ctl_endpoint: ipc:///tmp/einheit/hd-relay.ctl
  pub_endpoint: ipc:///tmp/einheit/hd-relay.pub
EOF
```

## 3. Start the daemon

```bash
sudo systemctl restart hyper-derp
sudo journalctl -u hyper-derp -n 10 --no-pager
```

You should see `wg-relay listening on UDP :51820 (0 peers, 0 links, xdp=off)`. Now drop into the operator CLI (no sudo — IPC is set up so any user can talk to the daemon):

```bash
hdcli
```

You'll get a `hyper-derp>` prompt with a small chafa banner. From here:

```
hyper-derp> wg show
```

Should print `port=51820`, `peer_count=0`, `link_count=0`. Stay in this REPL — the rest of the walkthrough runs commands here. `exit` or Ctrl-D leaves.

## 4. Set up alice

On `alice`:

```bash
wg genkey | tee alice.priv | wg pubkey > alice.pub
chmod 600 alice.priv
cat alice.pub          # note this; bob will need it

sudo tee /etc/wireguard/wg0.conf >/dev/null <<EOF
[Interface]
PrivateKey = $(cat alice.priv)
Address    = 10.99.0.1/24
ListenPort = 51820

[Peer]
PublicKey           = <BOB-PUBLIC-KEY>      # fill in after step 5
AllowedIPs          = 10.99.0.2/32
Endpoint            = <RELAY-IP>:51820
PersistentKeepalive = 25
EOF
```

`ListenPort = 51820` is mandatory — the relay matches peers by source port, so the port has to be stable. Don't bring `wg0` up yet.

## 5. Set up bob

Same on `bob`:

```bash
wg genkey | tee bob.priv | wg pubkey > bob.pub
chmod 600 bob.priv
cat bob.pub

sudo tee /etc/wireguard/wg0.conf >/dev/null <<EOF
[Interface]
PrivateKey = $(cat bob.priv)
Address    = 10.99.0.2/24
ListenPort = 51820

[Peer]
PublicKey           = <ALICE-PUBLIC-KEY>
AllowedIPs          = 10.99.0.1/32
Endpoint            = <RELAY-IP>:51820
PersistentKeepalive = 25
EOF
```

Now go back to alice's `wg0.conf` and paste bob's pubkey in the `<BOB-PUBLIC-KEY>` slot. Bring both up:

```bash
# alice
sudo wg-quick up wg0
# bob
sudo wg-quick up wg0
```

Ping won't work yet — the relay doesn't know about these peers.

## 6. Register peers + link on the relay

Back in the `hdcli` REPL on the relay:

```
hyper-derp> wg peer add alice <ALICE-IP>:51820 alice-laptop
hyper-derp> wg peer add bob   <BOB-IP>:51820   bob-laptop
hyper-derp> wg link add alice bob
```

Use the IP:port the relay actually sees alice and bob coming from. If they're behind NAT and you're not sure, on the relay run `sudo tcpdump -i any -nn udp port 51820` while a client is up and use whatever source IP:port shows up.

Verify:

```
hyper-derp> wg show
```

Should now read `peer_count=2`, `link_count=1`.

## 7. Ping

On alice:

```bash
ping -c 4 10.99.0.2
```

Should get four replies. Done.

In the relay REPL, `wg show` will now show `rx_packets` and `fwd_packets` non-zero and equal — every received packet was forwarded.

## 8. Adding more peers later

For each new peer, do step 4-style key generation + wg0.conf on the new box, then on the relay:

```
hyper-derp> wg peer add <name> <PUBLIC-IP>:51820 <description>
hyper-derp> wg peer pubkey <name> <THEIR-PUBKEY>
hyper-derp> wg link add <name> <existing-peer>
```

The pubkey isn't required for the relay to work, but if you stamp it the relay can render a ready-to-paste `[Peer]` block for new clients:

```
hyper-derp> wg show config <name> <RELAY-IP>:51820
```

(Output is a wg-quick `[Peer]` block with the right pubkey and endpoint pre-filled.)

## When it doesn't work

| symptom | usual cause | fix |
| --- | --- | --- |
| ping silent, `drop_unknown_src` ticks in `wg show` | client's source IP:port doesn't match what you registered | `tcpdump -i any -nn udp port 51820` on the relay; register what you see |
| ping silent, `drop_no_link` ticks | peer registered, link not added | `wg link add <a> <b>` |
| `hdcli`: `Connection refused` | daemon not running | `sudo systemctl status hyper-derp`, then `journalctl -u hyper-derp -n 30` |
| `hdcli`: `command not found` | deb not installed | reinstall — `hdcli` and `einheit` ship together |

If you change a peer's source endpoint (new public IP, NAT rebind), update it:

```
hyper-derp> wg peer update <name> <NEW-IP>:51820
```

To remove things:

```
hyper-derp> wg link remove alice bob
hyper-derp> wg peer remove alice    # also drops links involving alice
```

The peer + link table persists across `systemctl restart hyper-derp` — it's saved to `/var/lib/hyper-derp/wg-roster` after each change.

---

## How it works (one paragraph)

The relay maps **source 4-tuple → peer name → link partner → partner's endpoint**. When a packet arrives on UDP/51820, it looks up the source `(IP, port)` in the peer table; if it matches a registered peer with a link, the packet is rewritten to the partner's endpoint and sent. The relay never decrypts WG packets and doesn't track WG sessions. That's why it needs you to pin source endpoints (`ListenPort` on the client, fixed source IP:port at the relay) — there's nothing inside the encrypted payload it could use to identify peers.

If you want to chase throughput on real silicon (10/25 GbE NICs, cloud) there's an XDP fast-path mode — see the bench reports in the [`HD.Benchmark`](https://github.com/hyper-derp/HD.Benchmark/tree/master/docs) repo (`wireguard_relay_bench.md`, `wireguard_relay_bench_25g.md`, `wireguard_relay_bench_gcp.md`). For homelab speeds the userspace path is fine.
