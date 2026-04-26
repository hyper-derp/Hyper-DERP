# WG-relay pubkey filter — design note

## Status

Proposed. Targeting **0.2.1** if implemented.

## Goal

Let operators opt the wg-relay (`mode: wireguard`) into a stricter
mode where it forwards a WireGuard handshake only when the handshake
is *destined for* a peer whose static pubkey we have on record. This
turns the operator-stamped `wg peer pubkey <name> <pub>` from
metadata-only into an enforced check during handshake establishment.

## What this is and is not

This is **not** a security boundary against a determined attacker.
WireGuard's MAC1 field is `Blake2s(LABEL_MAC1 ++ Spub_r, msg[..-32])`
— the HMAC key is the *public* responder pubkey, so anyone who knows
the pubkey can forge MAC1. Treat this filter as:

- a **misconfiguration catch** — a client pointed at the wrong relay,
  or a stale endpoint that ended up in someone else's IP:port slot,
  is rejected at the relay rather than silently forwarded;
- an **operational story** — "this relay only carries traffic between
  peers whose pubkeys I've authorized" is true for the handshake
  path, even if the bytes that follow are not authenticated by the
  relay;
- **fail-closed for unknown clients** — if a stranger lands on a
  registered peer's source 4-tuple (e.g. a NAT collision), their
  handshake will get dropped instead of relayed.

It is **not** a defence against:

- an attacker who knows the responder's public key (they can forge
  MAC1);
- transport-data forgery once a session is up — type-4 packets carry
  only a 32-bit receiver-index and no pubkey, so we can never gate
  them by identity;
- replay or reordering — same caveat as today.

If you need cryptographic peer authentication at the relay, that's a
different product (likely terminating WG and re-encrypting outbound).
This note is explicitly *not* that.

## WG protocol surface

The wg-relay only ever sees four packet types on its UDP port:

| Type | Bytes 0..3 | Carries pubkey? |
|------|-----------|-----------------|
| 1 — handshake init | `01 00 00 00` | sender static is encrypted with responder's static; MAC1 is over responder's static |
| 2 — handshake resp | `02 00 00 00` | MAC1 is over responder's static |
| 3 — cookie reply  | `03 00 00 00` | no pubkey-bearing field; cookie reply is rare and only after a `MAC2` failure |
| 4 — transport data | `04 00 00 00` | no pubkey, only receiver-index |

Concrete consequence: only types 1 and 2 are filterable by pubkey.
Types 3 and 4 must pass-through based on source-endpoint match
(today's behaviour).

## Filter rule

For an arriving packet at the relay's wg-relay UDP port:

1. **Source 4-tuple** must match a registered peer. (Existing rule;
   unchanged. `drop_unknown_src` increments otherwise.)
2. The peer must have a registered link partner. (Existing rule;
   `drop_no_link` otherwise.)
3. **If the packet's first byte is `0x01` or `0x02`** (handshake)
   AND the registered link partner has a pubkey stamped via
   `wg peer pubkey`:
   - Compute `expected_mac1 = Blake2s(LABEL_MAC1 ++ partner_pub,
     pkt[0 .. -32])` (where `LABEL_MAC1 = "mac1----"`, ASCII).
   - Compare `expected_mac1` against the MAC1 field at offset
     `len - 32` of the packet.
   - If they don't match, drop. New counter:
     `drop_handshake_mac1_mismatch`.
4. Otherwise (types 3, 4, or partner has no stamped pubkey),
   forward as today.

The filter is **gated on the partner having a stamped pubkey** so
that operators who don't bother with `wg peer pubkey` retain
today's behaviour exactly. This keeps the feature opt-in at the
roster level — it engages automatically the moment an operator
stamps both ends of a link.

## Where it runs

Two places, mirroring the existing fast-path / userspace split:

- **Userspace path** — straightforward Blake2s computation in
  `src/wg_relay/forward.cc` before the existing forward call.
  Adds one keyed Blake2s per handshake packet, which is rare
  (handshake every 2 minutes per session). Not on the hot path.
- **XDP fast path** — the BPF program (`bpf/wg_relay.bpf.c`)
  already inspects the source endpoint and the partner MAC.
  We add a third map, `wg_partner_pubkey`, keyed by peer-id,
  and run a Blake2s check inline for handshake bytes only.

  Blake2s in BPF is feasible — the existing libbpf community has
  Blake2s implementations as a single function under the verifier's
  instruction limit. Tested up to ~250 instructions for the keyed
  variant. Worst case we degrade to "userspace handles all
  handshakes, XDP handles transport-data only" which is fine
  performance-wise (handshake rate is negligible).

When the BPF Blake2s lookup is uncertain to fit, the BPF program
should `bpf_redirect` handshake packets to userspace via
`AF_PACKET` / `XDP_PASS` and let the userspace path enforce. The
99 %+ of bytes (transport-data) stay on the XDP fast path.

## Operator surface

No new commands. The existing `wg peer pubkey <name> <pub>` becomes
load-bearing instead of cosmetic. To enable:

```
hyper-derp> wg peer pubkey alice <alice-pub>
hyper-derp> wg peer pubkey bob   <bob-pub>
```

Once both ends of a link have stamped pubkeys, the filter engages
automatically.

To check the filter is doing something, `wg show` gains a counter:

```
drop_handshake_mac1_mismatch  <count>
```

Non-zero means we caught at least one misconfigured client.

## Roster format change

The existing on-disk roster at `/var/lib/hyper-derp/wg-roster`
already has a `pubkey` field per peer (added in 0.1.x for
`wg show config` rendering). No schema change needed — the filter
just reads what the roster already stores.

## Failure modes

- **Operator stamped wrong pubkey for one end**: every handshake from
  the other end gets `drop_handshake_mac1_mismatch`. Visible in
  counters. Fix: re-stamp.
- **Operator stamped only one end**: filter inactive (gated on
  partner having a pubkey). Reverts to source-endpoint match only.
  No regression vs today.
- **Client roams to a new endpoint and doesn't update the operator
  roster**: handshake still has the right MAC1, so the filter passes
  it. The existing source-endpoint check (`drop_unknown_src`) is
  what catches that case, unchanged.

## Out of scope

- Cryptographic authentication of transport-data packets (impossible
  at the relay without terminating the tunnel).
- Per-session policy / rate-limiting of handshakes (separate
  mechanism, doesn't need pubkey awareness).
- WG cookie-reply (type 3) handling beyond pass-through.

## Implementation sketch

Three changes:

1. `bpf/wg_relay.bpf.c` — add `wg_partner_pubkey` BPF_MAP_TYPE_HASH
   keyed by peer-id (32-bit), value is 32 bytes of Blake2s state
   (precomputed `Blake2s(LABEL_MAC1 ++ pub)` partial, since the
   key is fixed per peer). Inline keyed Blake2s for the per-packet
   tail, compare last 32 bytes. New stats counter.
2. `src/wg_relay/forward.cc` — same check in userspace for the
   cold-start path and for builds without XDP. Increment
   `drop_handshake_mac1_mismatch`.
3. `src/wg_relay/cli.cc` — surface the new counter in `wg show`.
   No new CLI verbs.

## Estimated effort

~1 day:

- 3 h: write the BPF Blake2s + add the map + verifier passes
- 2 h: userspace mirror + counter wiring
- 1 h: integration test (libvirt fleet — register one alice with
  the wrong pubkey, assert handshakes drop)
- 1 h: docs (quickstart + cli_handbook updates)
- 1 h: bench (verify XDP_TX still hits 10 Gbit/s — handshake-only
  cost shouldn't move it; transport-data path is unchanged)
