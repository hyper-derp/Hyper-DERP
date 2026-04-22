# Fleet Control Plane

Signed, versioned policy bundles pulled by every relay
in a fleet. The control-plane server is nginx + a
~100-line Python WSGI script; the signing key stays
offline. Relays speak a tiny HTTPS client embedded in
the relay binary and apply the policy atomically on each
successful pull.

```
         operator machine                 policy server                 relays
  +-------------------------+        +----------------------+      +---------------+
  |  policy.yaml            |  scp   |  /var/fleet/         |  mTLS|  relay #1     |
  |  signing.key (offline)  | -----> |    bundle.json       | ---> |  (pulls every |
  |  sign.py                |  rsync |  nginx + uwsgi +     |      |   60s +/-10s) |
  +-------------------------+        |  serve.py            |      +---------------+
                                     +----------------------+      +---------------+
                                                                   |  relay #N ... |
                                                                   +---------------+
```

## Components

| Piece | Lives in | Purpose |
|-------|----------|---------|
| `tools/fleet/sign.py` | operator machine | canonical-JSON + Ed25519 signer; bumps version monotonically |
| `tools/fleet/serve.py` | policy server | WSGI app; `/api/v1/policy[?since=<v>]` and `/api/v1/revocations`; no auth (nginx handles it) |
| `tools/fleet/nginx.example.conf` | policy server | TLS 1.3 + mTLS client-cert verification; forwards to uwsgi |
| `include/hyper_derp/fleet_controller.h` | relay | background puller; Ed25519 verify; atomic apply |

## Bundle format

```json
{
  "version": 42,
  "fleet_id": "company-a",
  "signed_body_b64": "<base64 of canonical JSON>",
  "signature_b64":   "<base64 Ed25519 sig over signed_body bytes>"
}
```

The signed body decodes to:

```json
{
  "version":     42,
  "issued_at":   "2026-04-22T14:22:03Z",
  "fleet_id":    "company-a",
  "policy":      { ... },
  "revocations": { "relays": [...], "peers": [...] }
}
```

The on-disk envelope carries the version + fleet_id
twice: once in the outer JSON for cheap filtering, once
inside the signed body. The relay verifies they match
before applying.

## Operator workflow

### 1. Generate a signing keypair (one time)

Keep the private key on an offline machine — USB stick,
HSM, or a dedicated signing box that never touches the
production network.

```sh
python3 -c '
from nacl.signing import SigningKey
import base64
sk = SigningKey.generate()
open("signing.key", "wb").write(sk.encode())
print("pub:", base64.b64encode(sk.verify_key.encode())
                 .decode())
'
```

Record the printed public key; every relay needs it in
its config.

### 2. Author `policy.yaml`

```yaml
policy:
  routing:
    allow_direct: true
    allow_relayed: true
    require_relay_for_cross_region: false
  federation:
    accept_from:
      - fleet_id: "partner-b"
        allowed_destinations: ["ck_monitoring*"]
revocations:
  relays: []
  peers: []
```

Keep this file in a git repo — git history is your
audit log.

### 3. Sign it

```sh
./tools/fleet/sign.py \
    --in policy.yaml \
    --key signing.key \
    --fleet-id company-a \
    --out bundle.json
```

Version is read from the previous `bundle.json` and
bumped by one. Override with `--version <N>` when
rolling back (see below).

### 4. Publish

Copy `bundle.json` to the policy server:

```sh
scp bundle.json fleet.example.com:/var/fleet/bundle.json
```

nginx serves it from disk; no restart needed.

## Server deployment

`serve.py` runs behind nginx via uwsgi. Minimal
nginx.conf excerpt (full example in
`tools/fleet/nginx.example.conf`):

```nginx
server {
  listen 443 ssl;
  ssl_certificate      /etc/fleet/server.crt;
  ssl_certificate_key  /etc/fleet/server.key;
  ssl_client_certificate /etc/fleet/relay-ca.crt;
  ssl_verify_client on;

  location /api/v1/ {
    include uwsgi_params;
    uwsgi_param HD_FLEET_BUNDLE_PATH
                /var/fleet/bundle.json;
    uwsgi_pass unix:/run/fleet-serve.sock;
  }
}
```

Relays authenticate with mTLS client certificates
issued by your fleet CA; serve.py does no auth of its
own.

## Relay configuration

```yaml
hd:
  relay_key: "..."
  relay_id:  7
  federation:
    fleet_id: "company-a"
  fleet_controller:
    url: "https://fleet.example.com"
    signing_pubkey_b64: "abc...="   # from step 1
    client_cert: /etc/hd/relay.crt
    client_key:  /etc/hd/relay.key
    ca_bundle:   /etc/fleet/server-ca.crt
    poll_interval_secs: 60
    bundle_cache_path: /var/lib/hyper-derp/bundle.json
```

On startup the relay loads `bundle_cache_path` (if
present) so the last-known-good policy is active even
before the first poll succeeds. After that, it polls
every `poll_interval_secs` ± 10s and applies whatever
the server returns, provided:

1. HTTPS succeeds and status is 200 or 304
2. Ed25519 signature verifies against `signing_pubkey_b64`
3. Outer `fleet_id` == configured `fleet_id`
4. Inner `fleet_id` == outer `fleet_id`
5. `version` strictly greater than the currently-applied
   version

Any failure leaves the previous policy in place; the
server exposes the last-apply status via
`/api/v1/relay` as `hd_fleet_last_status` (future
endpoint).

## Revocation

Add to `revocations` in `policy.yaml`, re-sign, publish:

```yaml
revocations:
  relays:
    - relay_id: 47
      revoked_at: "2026-05-10T10:15:00Z"
      reason: "compromised"
  peers:
    - peer_fingerprint: "ck_abcdef..."
      revoked_at: "2026-05-10T11:30:00Z"
      reason: "key_rotation_skipped"
```

When a relay receives a bundle that revokes its own
`relay_id`, it sets an internal flag that the server's
stop-poller consumes: it initiates a graceful drain +
shutdown within 100ms. The operator must investigate
and re-provision (new cert, new relay_id) before the
relay rejoins.

Peer revocations are drained by the server on the same
100ms cadence: each fingerprint is revoked via
`HdPeersRevoke` (adds to the persistent denylist),
its active tunnel is closed, and future enrollment
attempts from the same key are rejected.

## Rollback

Relays refuse any bundle with `version <=
local_version`. To publish older content, sign it with
a newer version number:

```sh
./sign.py --in older_policy.yaml --key signing.key \
          --fleet-id company-a --out bundle.json \
          --version 1000
```

## Key rotation

1. Sign a new bundle (with the current key) whose policy
   includes the new public key alongside the old.
2. Deploy to relays. For a configurable grace period,
   relays accept either signature.
3. After the grace period expires, sign subsequent
   bundles with the new key only.
4. Decommission the old key.

(Grace-period signature acceptance lands with a future
bundle-schema revision; today the pubkey is pinned in
relay config and rotation requires a config push.)

## What this isn't

- **No custom daemon on the server.** nginx + 100 lines
  of Python is the entire server.
- **No UI.** Operators edit YAML. If you want
  review/approval, use `git` on the policy repo.
- **No database.** `bundle.json` on disk is the state.
- **No HA on the control plane.** Relays run on
  last-known-good for as long as the server is down;
  alert if the server is unreachable for more than an
  hour.
- **No multi-tenant RBAC.** One fleet per controller,
  one operator. Federate with other fleets instead of
  subdividing inside one.
