#!/usr/bin/env python3
"""Sign a fleet policy bundle.

Reads a YAML policy file and an Ed25519 signing key, emits
a versioned, signed bundle.json that the relay's
FleetController verifies on pull.

Bundle format:

  {
    "version":         <int64>,        # duplicated for
                                       # quick filtering
    "fleet_id":        "<string>",     # ditto
    "signed_body_b64": "<base64>",     # canonical JSON
                                       # (sort_keys,
                                       # no whitespace)
                                       # of the signed
                                       # payload
    "signature_b64":   "<base64 Ed25519 sig of the
                        raw bytes that signed_body_b64
                        decodes to>"
  }

The signed payload decodes to:

  {
    "version":     <int64>,
    "issued_at":   "<RFC3339 UTC>",
    "fleet_id":    "<string>",
    "policy":      { ... },
    "revocations": { ... }
  }

Keeping the signed payload opaque-to-the-relay as raw
bytes avoids having to re-canonicalize JSON in C++ for
verification.

The signing key stays offline (USB stick, HSM, or
dedicated signing machine). Online compromise of the
policy server does not compromise signing.
"""

import argparse
import base64
import datetime
import json
import os
import sys

try:
  import yaml
except ImportError:
  sys.stderr.write(
      "PyYAML required: pip install pyyaml\n")
  sys.exit(2)

try:
  from nacl.signing import SigningKey
except ImportError:
  sys.stderr.write(
      "PyNaCl required: pip install pynacl\n")
  sys.exit(2)


def canonical(obj):
  """Deterministic JSON for signing."""
  return json.dumps(obj, sort_keys=True,
                    separators=(",", ":")).encode("utf-8")


def load_key(path):
  with open(path, "rb") as f:
    data = f.read()
  # Raw 32-byte seed wins — strip() would corrupt it.
  if len(data) == 32:
    return SigningKey(data)
  return SigningKey(base64.b64decode(data.strip()))


def next_version(bundle_path):
  if not os.path.exists(bundle_path):
    return 1
  try:
    with open(bundle_path, "r") as f:
      prev = json.load(f)
    return int(prev.get("version", 0)) + 1
  except (OSError, ValueError):
    return 1


def main():
  p = argparse.ArgumentParser(description=__doc__)
  p.add_argument("--in", dest="in_path", required=True,
                 help="policy.yaml")
  p.add_argument("--key", required=True,
                 help="Ed25519 signing key file "
                      "(raw 32 bytes or base64)")
  p.add_argument("--out", default="bundle.json",
                 help="output bundle.json path")
  p.add_argument("--fleet-id", required=True,
                 help="fleet identifier (e.g. "
                      "company-a)")
  p.add_argument("--version", type=int, default=0,
                 help="explicit version (default: "
                      "previous + 1)")
  args = p.parse_args()

  with open(args.in_path, "r") as f:
    raw = yaml.safe_load(f) or {}
  policy = raw.get("policy", raw)
  revocations = raw.get("revocations", {})

  signing_key = load_key(args.key)
  version = (args.version if args.version > 0
             else next_version(args.out))

  signed_payload = {
      "version": version,
      "issued_at":
          datetime.datetime.now(
              datetime.timezone.utc)
              .isoformat(timespec="seconds")
              .replace("+00:00", "Z"),
      "fleet_id": args.fleet_id,
      "policy": policy,
      "revocations": revocations,
  }
  signed_bytes = canonical(signed_payload)
  sig = signing_key.sign(signed_bytes).signature
  bundle = {
      "version": version,
      "fleet_id": args.fleet_id,
      "signed_body_b64": base64.b64encode(
          signed_bytes).decode("ascii"),
      "signature_b64": base64.b64encode(sig).decode(
          "ascii"),
  }

  tmp = args.out + ".tmp"
  with open(tmp, "w") as f:
    json.dump(bundle, f, indent=2, sort_keys=True)
    f.write("\n")
  os.rename(tmp, args.out)

  sys.stderr.write(
      f"wrote {args.out} version={version} "
      f"fleet={args.fleet_id}\n")


if __name__ == "__main__":
  main()
