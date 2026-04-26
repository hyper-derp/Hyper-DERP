# WG-relay hardening — design note

## Status

Proposed. Targeting **0.2.1** alongside the pubkey filter
([wg_relay_pubkey_filter.md](wg_relay_pubkey_filter.md)).

## Goal

Three small XDP-side checks that close obvious abuse paths against
the wg-relay without changing its mission ("relay-only, no WG
semantics in the data plane"):

1. **WG-shape filter** — drop packets that aren't WireGuard-shaped
   at the earliest possible point.
2. **Per-peer pps cap** — bound the rate any single registered
   peer can push through the relay to its link partner.
3. **Optional source-IP allowlist** — restrict the source-IP CIDR
   block that's allowed to *register as* a given peer (or send
   from one, post-registration).

None of these are new product features for the operator to learn
about. The first is always-on. The second has a sensible default
that operators can override per peer. The third is opt-in
metadata on `wg peer add`.

## What this is and is not

This **is** infrastructure hardening — closing easy mistakes and
spam vectors before they tax bob's downlink or the relay's NIC.

This is **not** authentication. WireGuard's own crypto is the
identity layer; we're not pretending otherwise.

---

## 1. WG-shape filter

### Problem

Today the XDP fast path matches by source 4-tuple. If a registered
peer's IP:port slot is held by someone else (NAT collision, DHCP
churn, a careless reuse), arbitrary UDP from that endpoint gets
forwarded to the link partner. WireGuard discards it on the
receive side, but the bytes still flow over the wire.

### Filter

In the BPF program, after the source 4-tuple match and link
lookup, peek at the first byte of the UDP payload:

```
type = pkt[ETH+IP+UDP].first_byte
allow if type ∈ { 0x01, 0x02, 0x03, 0x04 }
drop  otherwise
```

That covers all four WireGuard message types (init, response,
cookie, transport). One byte. ~3 BPF instructions. Free.

Length sanity check at the same time:

```
type 1 (init)     → expect 148 bytes
type 2 (response) → expect 92 bytes
type 3 (cookie)   → expect 64 bytes
type 4 (data)     → ≥ 32 bytes (header+counter+empty payload)
                  → ≤ 1500-IP-UDP, but allow up to MTU
```

Mismatched → drop. New counter `drop_not_wg_shaped`.

### What it costs

- 1 BPF instruction per packet on the hot path (load byte +
  range check via jump table).
- Zero false positives on real WG traffic.
- Verifier-friendly; no pointer arithmetic beyond what we already
  do for the partner-MAC lookup.

### What it doesn't help with

- A rogue peer (alice) sending real WG packets at high rate to
  bob — those pass the shape filter. That's covered by the next
  section.

---

## 2. Per-peer pps cap

### Problem

Once alice is registered and linked to bob, alice can send
arbitrary WG-shaped packets at line rate. The relay obediently
forwards every one to bob. Bob's `wg.ko` drops everything that
doesn't decrypt, but bob's downlink doesn't care — bytes are
bytes.

A real WG session generates a handshake every ~120 s and a
heartbeat every `PersistentKeepalive` interval (typically 25 s).
Transport-data depends on application traffic, but a sane upper
bound for a single tunnel on consumer/cloud links is ~200 kpps
(roughly 2 Gbit/s at 1280-byte MTU).

### Filter

A token-bucket rate-limit per peer, enforced in BPF:

```
struct peer_rl {
  __u64 tokens;       // current tokens
  __u64 last_ns;      // last refill time (CLOCK_MONOTONIC)
  __u32 rate_pps;     // bucket fill rate (packets per second)
  __u32 burst;        // bucket capacity
};

BPF_MAP_TYPE_PERCPU_HASH wg_peer_ratelimit
  key: peer_id (u32)
  value: peer_rl
```

On each forwarded packet:

```
tokens = min(tokens + (now - last_ns) * rate_pps / 1e9, burst)
if tokens < 1 → drop, increment drop_peer_rate_limit
else tokens -= 1, forward
last_ns = now
```

PERCPU_HASH avoids the cross-CPU contention; per-CPU drift on
the rate over 1 s windows is acceptable for this purpose.

### Defaults

- `rate_pps = 250000` (~250 kpps, comfortably above any single
  tunnel's working set; ~2.5 Gbit/s at 1280 B)
- `burst = 50000` (200 ms of allowance)

These are conservative — they only drop traffic from a peer
clearly trying to flood. Normal heavy use stays well under.

### Operator override

```
hyper-derp> wg peer ratelimit alice 100000 20000
                           ↑ peer  ↑ rate ↑ burst
```

Stored on the peer record alongside endpoint/pubkey/nic.
Persisted to the roster. `wg peer list` shows the configured
values. `wg peer ratelimit alice off` reverts to defaults.

To disable rate-limiting entirely (back to today's behaviour):

```
sudo systemctl edit hyper-derp
[Service]
Environment=HD_WG_RATELIMIT=off
```

### Side effects

- Adds one PERCPU_HASH lookup + token math per packet on the
  hot path. ~5 ns extra at line rate. Existing 25 GbE bench
  numbers should hold within noise.
- New counter: `drop_peer_rate_limit` (per-peer in
  `wg peer list`, aggregate in `wg show`).

---

## 3. Source-IP allowlist (optional, per peer)

### Problem

For peers behind static-IP infrastructure (datacentre, fixed
home connection), the operator knows what source IP space the
peer will ever come from. Restricting accepted source IPs to
that CIDR closes the spoofing case completely: even if an
attacker forges UDP from `<alice-ip>:51820`, packets from a
different CIDR get dropped.

### Surface

Optional argument to `wg peer add`:

```
hyper-derp> wg peer add alice 198.51.100.10:51820 alice-laptop
hyper-derp> wg peer src alice 198.51.100.0/24
```

Or `0.0.0.0/0` (default — equivalent to today, accepts any source
that matches the registered IP:port).

Multiple CIDRs allowed:

```
hyper-derp> wg peer src alice 198.51.100.0/24,2001:db8::/32
```

### Filter

After source 4-tuple match, before forward:

```
peer_src_cidr = wg_peer_src_cidr_lookup(peer_id)
if peer_src_cidr != 0/0 and src_ip ∉ peer_src_cidr →
  drop, increment drop_src_cidr_mismatch
```

CIDR storage: BPF_MAP_TYPE_LPM_TRIE keyed by IPv4 + peer_id;
separate for v6 if needed.

### When to use

- Datacentre / static-IP peer (highly recommended).
- Mobile / dynamic-IP peer: leave it `0.0.0.0/0` (the source IP
  changes too often to be useful). The other two filters still
  apply.

---

## Counter summary

After this lands, `wg show` exposes:

| field | meaning |
|---|---|
| `drop_unknown_src` | packet didn't match any registered peer (today) |
| `drop_no_link` | peer registered but no link (today) |
| `drop_not_wg_shaped` | first byte ∉ {0x01..0x04} or wrong length |
| `drop_handshake_mac1_mismatch` | MAC1 against partner pubkey failed (pubkey filter doc) |
| `drop_peer_rate_limit` | peer exceeded token-bucket budget |
| `drop_src_cidr_mismatch` | source IP outside peer's allowed CIDR |

Each is a strong operator signal — if any of them are growing
non-trivially, something is misconfigured or someone's poking
at the relay.

---

## Implementation order

The three are independent. Recommended landing order:

1. **WG-shape filter** — trivial (1 byte check), zero ops surface.
   Land first, free win.
2. **Per-peer pps cap** — needs the new map + new CLI verb. The
   token-bucket math is well-trodden BPF territory.
3. **Source-IP allowlist** — needs LPM_TRIE + new CLI verbs +
   v6 path. Largest of the three.

(1) and the pubkey filter could ship together in the same 0.2.1
patch since both are pure BPF additions on the existing
infrastructure. (2) and (3) are bigger and could slip to 0.2.2
without losing much.

---

## Out of scope

- **Cross-relay rate-limit / circuit breakers** — the operator
  can already remove a peer; that's the heavy hammer.
- **DPI / signature-based filtering** beyond the WG message
  type byte — explicit non-goal, mission is "no WG semantics in
  the relay".
- **Stateful flow tracking** — the relay is intentionally
  stateless beyond the peer + link table. Token-bucket counters
  are the only per-flow state we add.

## Estimated effort

Roughly:

| change | dev | tests |
|---|---:|---:|
| WG-shape filter | 1 h | 1 h |
| Per-peer pps cap | 4 h | 2 h |
| Source-IP allowlist | 6 h | 3 h |

Totals to ~2 dev-days for all three plus a bench re-run on the
25 GbE rig to confirm no regression in the XDP_TX/REDIRECT path.
