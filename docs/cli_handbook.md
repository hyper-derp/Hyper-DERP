# Hyper-DERP CLI Handbook

This is the operator handbook for driving a hyper-derp relay through the einheit CLI. The hyper-derp deb bundles `einheit` at `/usr/bin/einheit` and the `hdcli` wrapper at `/usr/bin/hdcli` — typical operators just run `hdcli`. This handbook covers the underlying einheit invocation for cases where you need finer control (different role, custom endpoints, scripting).

## What you're talking to

```
einheit (DEALER) ────► hyper-derp (ROUTER on ipc:///tmp/einheit/hd-relay.ctl)
        ◄─── catalog (describe handshake)
        ◄─── command responses
        ◄──── events (PUB on ipc:///tmp/einheit/hd-relay.pub)
```

The CLI knows almost nothing about the daemon at compile time. On startup it issues a `describe` call and merges the daemon's command catalog into its tree, which is why a hyper-derp release that adds a new wire verb shows up in the CLI immediately — no client rebuild.

## Prerequisites

- Daemon listening on its einheit endpoints (`einheit.ctl_endpoint` / `einheit.pub_endpoint` in `dist/hyper-derp.yaml`).
- `einheit` binary, with `--adapter hd-relay` selected.
- `EINHEIT_ROLE` set when you need anything beyond `show status` / `show peers` / `show peer`. The daemon enforces an advisory role gate — see *Roles* below.

## Connecting

### From the relay host (local IPC)

```bash
EINHEIT_ROLE=admin einheit \
  --adapter hd-relay \
  --endpoint ipc:///tmp/einheit/hd-relay.ctl \
  --event-endpoint ipc:///tmp/einheit/hd-relay.pub
```

The hyper-derp deb ships a wrapper at `/usr/bin/hdcli` that
bakes those flags in (and exports the chafa-branded banner):

```bash
hdcli                 # interactive REPL, admin role
hdcli show status     # one-shot
hdcli show fleet
hdcli wg show         # wg-relay-mode counters (mode: wireguard only)
```

### Over SSH

`~/.ssh/config` entries on the operator workstation:

```
Host hd-r1-cli
  HostName 192.168.122.13
  User worker
  IdentityFile ~/.ssh/id_ed25519_targets
  RequestTTY yes
  RemoteCommand /usr/bin/hdcli
```

Then:

```bash
ssh hd-r1-cli                    # drops into the einheit shell
ssh hd-r1 hdcli show status     # oneshot via plain ssh
```

### Roles

`EINHEIT_ROLE` selects what the daemon will let you do:

| Value      | Permits                                                |
|------------|--------------------------------------------------------|
| (unset)    | `show status`, `show peers`, `show peer`              |
| `operator` | + `show audit/counters/config/fleet/commits/commit`,   |
|            |   all `peer ...` verbs                                 |
| `admin`    | + the candidate-config lifecycle, `relay init`,        |
|            |   `daemon restart` / `daemon stop`                     |

`--role admin` on the command line is equivalent to setting the env. The gate is advisory — the daemon trusts the caller's stated role; a SO_PEERCRED-based check is a future iteration.

## Read-only commands

| Path                 | Role     | What                                               |
|----------------------|----------|----------------------------------------------------|
| `show status`        | any      | Worker count, byte counters, hd peer count         |
| `show peers`         | any      | All HD peers with state / fd / peer_id             |
| `show peer <key>`    | any      | One peer's detail + policy + forwarding rules      |
| `show fleet`         | operator | Self relay_id + remote relay-table entries + hops  |
| `show config`        | operator | Redacted runtime config snapshot                   |
| `show config <pre>`  | operator | Same, filtered by path prefix                      |
| `show counters`      | operator | Per-worker recv / send / drops + HD enroll counters|
| `show audit`         | operator | Last 32 routing-policy decisions                   |
| `show commits`       | operator | Persisted commit log (id, ts, set/delete counts)   |
| `show commit <id>`   | operator | One commit with diff vs. cumulative prior state    |
| `show schema`        | (local)  | Every configurable path with type + help text      |
| `show env`           | (local)  | Terminal caps, theme, aliases, target              |

`show schema` and `show env` are framework-local — no wire round-trip.

Keys are accepted as `ck_<hex>...` (Curve25519 client key), `rk_<hex>...` (relay key), or raw 64-hex.

## Configuration lifecycle

Live-mutable settings go through a Junos-style candidate workflow:

```
configure              # opens a session, returns session_id
set <path> <value>     # validates + stages
delete <path>          # stages a default-restore
commit                 # applies + persists, ends the session
rollback candidate     # discards staged changes
rollback previous      # synthesizes an inverse of the last commit
```

Both dotted and space-separated paths are accepted equivalently:

```
set hd.relay_policy.max_direct_peers 64
set hd relay_policy max_direct_peers 64
```

Validation runs at `set` time; the daemon refuses values that don't match the path's type. Apply runs at `commit` time, under the relevant locks (e.g. `hd_peers.mutex` for HD-policy fields).

### Live-mutable paths

| Path                                       | Type                             | Default          |
|--------------------------------------------|----------------------------------|------------------|
| `log_level`                                | enum: trace/debug/info/warn/error/critical/off | `info` |
| `hd.enroll_mode`                           | enum: auto / manual              | `manual`         |
| `hd.relay_policy.default_mode`             | enum: prefer_direct/require_direct/prefer_relay/require_relay | `prefer_direct` |
| `hd.relay_policy.forbid_direct`            | bool                             | `false`          |
| `hd.relay_policy.forbid_relayed`           | bool                             | `false`          |
| `hd.relay_policy.max_direct_peers`         | int ≥ 0                          | `0` (unlimited)  |
| `hd.relay_policy.audit_relayed_traffic`    | bool                             | `true`           |
| `hd.federation.fleet_id`                   | string                           | `""`             |
| `peer_rate_limit`                          | uint64                           | `0` (unlimited)  |

Anything not in the table needs a yaml edit + restart — `set` will refuse it with `not_live_mutable` and a hint. Identity fields (`port`, `workers`, `tls_cert/key`, `hd.relay_key`, `hd.relay_id`) are deliberately excluded.

### Persistence

Set `einheit.commit_log_path: <file>` in the daemon config to keep a tab-separated record of every commit:

```
1   2026-04-23T18:22:41.340Z   S   hd.relay_policy.forbid_relayed   true   ...
2   2026-04-23T18:23:09.124Z   S   hd.relay_policy.max_direct_peers 256    ...
```

Replayed at startup (last-write-wins per key), so committed changes survive restarts. Partial last lines from a power cut are dropped silently.

### Rollback

`rollback candidate` discards an in-flight session — no wire effect.

`rollback previous` builds a new commit whose ops invert the last record: keys it set get their prior value back (or a delete to default if there was no prior); keys it deleted get restored. The inverse is itself a commit, so `rollback previous` is idempotent and shows up in `show commits`.

`rollback previous` requires a non-empty commit log (it has nothing to invert without history).

## Peer lifecycle (operator role)

| Path                        | What                                                |
|-----------------------------|-----------------------------------------------------|
| `peer approve <key>`        | Move a pending peer to approved                     |
| `peer deny <key>`           | Reject a pending peer                               |
| `peer revoke <key>`         | Denylist + disconnect                               |
| `peer redirect <key> <dst>` | Send a Redirect frame to the peer                   |
| `peer policy set <key> <intent>` | Pin a routing intent on a peer                 |
| `peer policy clear <key>`   | Remove the pin                                      |
| `peer rule add <src> <dst>` | Add a forwarding rule                               |

`<intent>` is one of `prefer_direct`, `require_direct`, `prefer_relay`, `require_relay`.

## Service control

The CLI has both wire and local verbs for service management. Wire verbs work over the einheit channel and are dispatched by the daemon; local verbs shell out to `systemctl --user hyper-derp`.

| Path              | Where       | When to use                                    |
|-------------------|-------------|-----------------------------------------------|
| `daemon restart`  | wire        | Daemon is up; you want to restart it. Reply flushes before exit; the CLI's DEALER auto-reconnects to the new process. |
| `daemon stop`     | wire        | Daemon is up; you want it down.               |
| `daemon start`    | local       | Daemon is down; can't be a wire verb.         |
| `daemon status`   | local       | Cheaper than a round-trip; works when daemon is unreachable. |

Both wire verbs require admin role and prompt for confirmation. The CLI's DEALER socket is restart-tolerant by design — `daemon restart` does not end your session.

## Events

The daemon's PUB socket emits hierarchical topics. Subscribe with `watch <command>` from the shell or with any external ZMQ SUB consumer.

| Topic prefix                          | Body                                         |
|---------------------------------------|----------------------------------------------|
| `state.metrics.{recv_bytes,...}`      | Cumulative counters (10× per second)         |
| `state.peers.<ck_hex>`                | `key=...\nstate=approved|denied|revoked\n`   |
| `state.config.committed`              | `commit_id=N\n`                              |
| `state.config.rolled_back`            | `scope=candidate|previous\n[commit_id=...]\n`|

## Troubleshooting

### `oneshot: request timed out` immediately after boot

`/tmp` is tmpfs on most distros; the daemon's IPC socket directory got wiped and never recreated. Use the user-mode unit at `dist/hyper-derp.user.service` (it has `ExecStartPre=mkdir -p /tmp/einheit`) or create the directory before starting the daemon.

### `error: HD mode (--hd-relay-key) requires TLS`

Set `tls_cert` / `tls_key` in the yaml. The daemon will not accept HD peers over plaintext.

### `kTLS not available: TCP_ULP setsockopt: No such file or directory`

`tls` kernel module isn't loaded. `sudo modprobe tls`, then make it persistent with `echo tls | sudo tee /etc/modules-load.d/tls.conf`.

### `error: forbidden — role 'X' not allowed; need admin`

Daemon-side role gate. Set `EINHEIT_ROLE=admin` (or `--role admin`).

### `error: not_live_mutable`

The path you tried to `set` requires a yaml edit + daemon restart. The path table above shows what is and isn't live-mutable.

### Stuck in `deactivating (stop-sigterm)` for 90s

Pre-existing daemon shutdown bug — SIGTERM handler is installed but something in the cleanup path hangs. Workaround: the user-mode unit sets `TimeoutStopSec=5s` so systemd SIGKILLs sooner. Filed as a follow-up.

### `set hd <tab>` shows nothing

You're on an old einheit-cli (pre-`a0dcf0b`). Update the binary; recent versions fall back to schema path-completion when the partial doesn't name a leaf.

## Files of interest

| Path                                  | What                                    |
|---------------------------------------|-----------------------------------------|
| `~/hd/hyper-derp.yaml`                | Daemon config                           |
| `~/hd/commits.log`                    | Persisted candidate-config commit log   |
| `~/hd/audit.log`                      | Routing-policy decision audit           |
| `~/hd/cert.pem` / `~/hd/key.pem`      | kTLS material                           |
| `~/.config/systemd/user/hyper-derp.service` | User-mode unit                    |
| `/tmp/einheit/hd-relay.ctl`           | Control IPC socket (ROUTER ↔ DEALER)    |
| `/tmp/einheit/hd-relay.pub`           | Event PUB socket                        |
