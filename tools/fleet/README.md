# Fleet control plane

Signed, versioned policy bundles pulled by relays. No
custom daemon; the server side is nginx + a ~100-line
Python WSGI script + an Ed25519 signing script run
out-of-band.

## Generate a signing keypair

```sh
python3 -c '
from nacl.signing import SigningKey
import base64
sk = SigningKey.generate()
open("signing.key", "wb").write(sk.encode())
print("pub:",
      base64.b64encode(sk.verify_key.encode()).decode())
'
```

The private key never leaves your signing machine. The
public key goes into every relay's config.

## Write a policy

`policy.yaml`:

```yaml
policy:
  routing:
    allow_direct: true
    allow_relayed: true
  federation:
    accept_from:
      - fleet_id: "partner-b"
        allowed_destinations: ["ck_monitoring*"]

revocations:
  relays: []
  peers: []
```

## Sign it

```sh
./sign.py --in policy.yaml --key signing.key \
          --fleet-id company-a --out bundle.json
```

`sign.py` reads the previous `bundle.json` (if present),
bumps the version, signs the bundle with Ed25519, and
writes atomically via temp + rename.

## Serve it

Copy `bundle.json` to the policy server. nginx handles
TLS termination + mTLS client-cert verification and
forwards to uwsgi running `serve.py`. See
`nginx.example.conf` for a starting point.

Relays pull `/api/v1/policy?since=<version>` every 60s
(±10s jitter). The 304 path keeps the server tiny:
steady state is a 304 every minute per relay.

## Revoke a relay or peer

Add to `revocations` in `policy.yaml`, re-sign, publish:

```yaml
revocations:
  relays:
    - relay_id: 47
      revoked_at: "2026-05-10T10:15:00Z"
      reason: "compromised"
  peers:
    - peer_fingerprint: "ck_abc..."
      revoked_at: "2026-05-10T11:30:00Z"
      reason: "key_rotation_skipped"
```

A relay that receives a bundle revoking its own
`relay_id` self-terminates: it drops all tunnels and
exits. The operator must investigate and re-provision
before the relay rejoins. Other relays refuse to accept
federation handshakes from the revoked relay on their
next bundle pull, so the compromised relay is isolated
even if it keeps running.

## Rollback

Relays refuse any bundle with `version <= local_version`.
To roll back content, re-sign the desired policy body
with a newer version number.

## Key rotation

Publish a new bundle (signed with the old key) whose
policy embeds the new public key. Relays accept either
signature for a grace period, then switch to the new
key for subsequent pulls.
