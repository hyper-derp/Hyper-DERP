# HD Protocol Routing Policy

## Status: Design (not implemented)

## Summary

Routing mode (direct vs relayed) is a policy choice, not an
automatic optimization. The client expresses intent, the relay
evaluates it against operator and pair policies, and both sides
know which mode they're operating in.

## Client Intent

```
enum RoutingIntent {
  PreferDirect,   // Try ICE, fall back to relay
  RequireDirect,  // Try ICE, fail if ICE fails
  PreferRelay,    // Don't try ICE, use relay
  RequireRelay,   // Use relay, refuse if unavailable
}
```

`prefer_*` = preference. `require_*` = policy. Don't collapse.

## Relay Policy Layers

Three-way AND: `client_intent AND operator_policy AND pair_policy`.
All must permit the mode.

1. **Operator policy**: fleet-wide rules.
   "All traffic relayed for audit" or "no relay, egress costs."
   Overrides client preference.

2. **Pair policy**: per-peer-pair rules.
   "A↔B must be direct" or "A→C only through relay R2."
   For compliance, geography, metadata control.

3. **Capability matching**: if client requires direct but peer
   is behind known symmetric NAT, short-circuit and fail/deny
   instead of waiting for ICE timeout.

## Wire Format

```
OpenConnection {
  target_peer_id: uint16
  target_relay_id: uint16
  routing: RoutingIntent
  allow_upgrade: bool    // relayed → direct
  allow_downgrade: bool  // direct → relayed
}

OpenConnectionResult {
  mode: Direct | Relayed | Denied
  deny_reason: PolicyForbids | PairForbids | PeerUnreachable
  peer_endpoint_hint: optional<endpoint>
  relay_path: optional<[relay_id]>
}
```

## Upgrade/Downgrade

Explicit opt-in, not automatic:

- `allow_upgrade: true` — try to promote relayed → direct
  in background. Default for most users.
- `allow_upgrade: false` — stay relayed. Don't expose endpoint.
  For privacy/compliance.
- `allow_downgrade: true` — fall back to relayed if direct breaks.
  Default for most users.
- `allow_downgrade: false` — drop connection if direct breaks.
  For require_direct users.

## Symmetric Consent

If peer A says `require_relay` and peer B says `require_direct`,
the connection fails. Neither side is silently forced into a mode
they didn't want.

If A says `prefer_relay` and B says `require_direct`, the relay
notifies A: "B requires direct, proceed or abort." Decision stays
with the user.

## State Machine

```
intent: immutable per-connection
current_mode: state machine position

kNew → kWgexDone → (policy decision)
  ├── PreferDirect|RequireDirect → kIceGathering → kIceChecking
  │     → kDirect (success)
  │     → kRelayed (PreferDirect fallback)
  │     → kFailed (RequireDirect, no fallback)
  └── PreferRelay|RequireRelay → kRelayed (no ICE)

Transitions:
  kRelayed + allow_upgrade → background ICE → kDirect on success
  kDirect + allow_downgrade + path broken → kRelayed
  kDirect + !allow_downgrade + path broken → kFailed
  kRelayed + !allow_upgrade → stays relayed
```

## Observability

```
$ hdctl status
Peer: camera-floor-3
  Mode: Direct (promoted from Relayed at 14:22:03)
  Endpoint: 198.51.100.14:51820
  Latency: 0.8ms
  Policy: prefer_direct, allow_upgrade, allow_downgrade

Peer: monitoring-station
  Mode: Relayed (via relay-eu)
  Latency: 47ms
  Policy: require_relay (compliance)
```

The mode is visible, not hidden. Industrial/enterprise buyers
want to see and control the routing fabric.

## Implementation Scope

**Phase 1 (current)**: automatic direct/relay with upgrade.
No policy layer. `prefer_direct` + `allow_upgrade` + `allow_downgrade`
hardcoded. Ship the stack, prove it works.

**Phase 2**: routing intent in OpenConnection. Relay-side policy
evaluation. Pair policies via admin API. Symmetric consent.
~400 LOC in wg_peer.cc, ~300-400 LOC relay-side policy engine.

## Conceptual Shift

The original HD Protocol spec treated levels as an optimization
cascade: "try the best, degrade if needed." This design reframes
them as a policy space:

- Level 0: DERP compatibility (always relayed, Tailscale compat)
- Level 1: HD relayed (HD protocol, relay carries traffic)
- Level 2: HD direct (HD signaling, WireGuard peer-to-peer)

Each is a valid target, not a fallback tier. The relay is a
security and routing decision, not just a performance fallback.
