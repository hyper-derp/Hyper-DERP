# WG-relay pubkey filter + automatic roaming — design note

## Status

Proposed. Targeting **0.2.1** if implemented.

## Goal

Use the operator-stamped partner pubkey for two things at once:

1. **Automatic roaming.** When a peer's source IP:port changes
   (mobile, ISP rebind, VPN reconnect), the relay can recognise
   the new handshake as belonging to that peer and update its
   roster entry without operator intervention. This is the big
   feature.
2. **Handshake validation.** A side-effect of (1): handshakes
   that don't match any registered partner's pubkey get dropped.
   That gives an opt-in misconfiguration catch and a small,
   bounded security improvement.

Both engage automatically the moment both ends of a link have a
pubkey stamped via `wg peer pubkey`. No new operator commands
required.

## How MAC1 carries identity

Every WireGuard handshake init/response packet ends in 16 bytes
of MAC1, computed as:

```
MAC1 = Blake2s(LABEL_MAC1 ++ Spub_r, msg[0 .. -32])
```

where `Spub_r` is the **responder's** static public key. The
HMAC key is public, so MAC1 isn't an authenticator of the
sender — but it *is* a signed claim of "this packet is destined
for the peer whose static pubkey is X".

Combine that with our roster: when alice and bob are linked, and
the relay has bob's pubkey on file, an arriving handshake whose
MAC1 verifies against bob's pubkey can only be *from* bob's
link partner — alice — by definition. The relay knows who sent
it, even if the source IP doesn't match the registered endpoint.

## Use 1: Automatic roaming

### The pain today

Alice's IP changes (CGNAT rebind, leaving the office Wi-Fi for
LTE, ISP DHCP renewal). Her wg-quick keeps trying to reach the
relay from the new IP:port. The relay sees:

```
src = <alice-new-ip>:51820
not in peer table → drop_unknown_src++
```

The tunnel stays broken until the operator manually runs
`hdcli wg peer update alice <new-ip>:51820`.

### With the pubkey-driven relearn

When the relay receives an arriving packet that:

- doesn't match any registered peer's source 4-tuple, AND
- is a handshake init (type 0x01) or response (type 0x02), AND
- has a valid MAC1 against some registered peer's link
  partner's pubkey,

then the only peer this packet can be from is that partner's
link partner. The relay:

1. Updates that peer's `endpoint` field to the new source
   `IP:port`.
2. Increments `endpoint_relearn` counter for that peer.
3. Forwards the handshake to the partner as normal.
4. Persists the new endpoint to the roster file (atomic
   rewrite, same path that `wg peer add` uses today).

Bob's wg.ko receives the handshake, completes it normally, and
the tunnel is back. No operator intervention.

### Flow

```
Alice's IP changed: 198.51.100.10 → 203.0.113.42
Roster still says alice = 198.51.100.10:51820

T+0:    alice's wg-quick keepalive fires, sends handshake init
        from 203.0.113.42:51820 to relay:51820
T+0:    relay sees src=203.0.113.42:51820 — no peer match
        — packet is type 0x01 handshake init
        — compute MAC1 against every linked partner's pubkey
        — match found: bob's pubkey
        — therefore packet is from alice (bob's link partner)
T+0:    relay updates alice.endpoint = 203.0.113.42:51820
        forwards handshake to bob
        endpoint_relearn[alice]++
T+1:    bob responds; relay forwards back to 203.0.113.42:51820
T+2:    alice's wg.ko completes handshake. Tunnel up.
```

Worst case: the gap between alice's IP change and her next
handshake (default 120 s for active sessions, immediate for
new ones with `PersistentKeepalive`).

### Tentative-then-confirm: the safety gate

A handshake init with valid MAC1 is necessary but not sufficient
to commit the relearn. Anyone with knowledge of bob's pubkey
can produce one. What an attacker **can't** produce is a
follow-up packet that's actually part of a live WG session — the
session keys are derived during the handshake from a Curve25519
DH that requires alice's private key.

So the relay does the relearn in two phases:

1. **Tentative** — on a valid-MAC1 handshake init from an
   unknown source, mark the new source as a `candidate` endpoint
   for the peer. The committed endpoint stays unchanged. The
   handshake response from the partner is mirrored to **both**
   the candidate and the committed endpoints, so a real-alice
   roam still completes (her wg.ko gets the response), and a
   stale-alice (still at the old IP) keeps her existing session.
2. **Confirm** — only commit the new endpoint once transport-data
   (type 0x04) actually flows from the candidate. That packet
   carries a receiver-index that wg.ko on the responder side
   accepted; the relay doesn't need to verify it cryptographically,
   it just needs to observe that the candidate is sending the
   shape of traffic that follows a *successful* handshake. An
   attacker without alice's private key never gets here — they
   can't produce valid post-handshake messages.

```
T+0:    candidate handshake init (valid MAC1 against bob.pub)
        from src=NEW_IP — committed endpoint untouched, candidate
        recorded with 30 s timeout.
T+0:    relay forwards init to bob.
T+0:    bob's response → relay → forwarded to BOTH committed and
        candidate endpoints.
T+1:    real alice (at NEW_IP) decrypts response, sends transport
        data from NEW_IP. Relay sees type 0x04 from NEW_IP →
        commit: alice.endpoint = NEW_IP, candidate cleared.
        endpoint_relearn[alice]++.
T+30:   no transport-data ever arrived from NEW_IP →
        candidate expires, alice.endpoint stays at OLD_IP.
        drop_relearn_unconfirmed++.
```

Cost during the candidate window: one extra forward of the
handshake response (16 + 92 = 108 bytes one time per relearn
attempt). No effect on transport-data forwarding rate.

### What this closes off

The "attacker who knows bob's pubkey hijacks alice's endpoint
slot" attack from the previous draft: now closed. The attacker
can trigger a candidate state but can never confirm it, so the
committed endpoint never moves. The only observable effect of
the attack is `drop_relearn_unconfirmed` ticking up — a clean
operator signal.

### Cooldown

To prevent attackers from churning the candidate slot (each
candidate keeps state for 30 s), the relay enforces a minimum
interval between *new* candidate registrations per peer:

```
DEFAULT: 5 s minimum between relearn attempts for the same peer
```

Configurable via `hdcli wg peer relearn-cooldown alice <secs>`.
Set to `0` to disable the cooldown, or `off` to disable
relearning entirely for that peer (back to today's static
behaviour).

A second per-source-IP cap prevents one attacker from chewing
through every peer's candidate slot in a scan:

```
DEFAULT: 100 candidate registrations / minute / source-IP
```

Beyond that: `drop_relearn_source_rate` ticks, candidates from
that source are refused for 60 s.

### Dynamic blocklist on repeated failed confirms

A `drop_relearn_unconfirmed` is unambiguous: the source produced
a valid-MAC1 handshake init but couldn't progress the protocol.
That's not a flaky network — it's someone who has the public key
but not the private key. The relay tracks failed confirms per
source IP and escalates:

```
DEFAULT POLICY (configurable):
  2 failed confirms within 60 s    → block source-IP for 60 s
  5 failed confirms within 1 h     → block source-IP for 1 h
  10 failed confirms within 24 h   → block source-IP for 24 h
```

While a source is blocklisted, the BPF program drops **all**
its packets at the very top of the XDP path — not just
handshakes. The block is per-IP, not per-(IP,port), so a port
shuffle doesn't escape it. New counter:
`drop_blocklisted` (every dropped packet), `blocklist_added`
(every escalation event).

```
struct blocklist_entry {
  __u64 expiry_ns;
  __u32 strikes;       // failed-confirm count
  __u32 first_strike_ns;
};

BPF_MAP_TYPE_HASH wg_blocklist
  key: u32 (src IPv4)  // separate map for v6
  value: blocklist_entry
```

Map is sized for ~10k active blocks (640 KB). Stale entries
sweep on access.

### Why blocklisting matters: the relay-as-amplifier problem

Without it, an attacker who knows bob's pubkey can blast the
relay with arbitrarily many forged handshake inits, and the
relay will dutifully forward every one to bob — because
"forward to partner" is what the candidate-registration path
does. The candidate-confirm gate stops the *endpoint hijack*,
but it doesn't stop the relay from acting as a forwarder of
attack traffic. Concretely:

- **Bob pays the cost.** Each forwarded init costs bob's CPU
  (MAC1 verify, ChaPoly decrypt attempt) and his ingress
  bandwidth. At 200 byte init * 10k pps = 16 Mbit/s pinned on
  bob's NIC, plus ~5 % of one CPU on his side.
- **The attacker is hidden behind the relay's IP.** From bob's
  `wg.ko` logs and from any defensive tooling on bob's side,
  the source of the flood is the relay. Bob can't blocklist
  the real attacker — he can't see them.
- **The relay becomes the laundering layer.** Anyone investigating
  bob's attack traffic finds the relay, not the source. That's
  bad operationally (the relay's reputation), bad legally (the
  relay operator looks like the abuser), and bad ergonomically
  (bob's only fix is to take down the link to alice, which is
  exactly the disruption we were trying to avoid).

The blocklist is the actual fix for this. After 2 failed
confirms, the source's *every* packet drops at the top of XDP
— never reaches the forward path, never lands on bob's NIC.
Bob sees nothing. The relay isn't laundering anything anymore.

### What blocklisting still can't do

A distributed attacker rotating source IPs faster than the strike
threshold can still get the first ≤ 2 forgeries per IP through.
That's a fundamental tradeoff between false-positive rate and
attack-surface reduction; lower the strikes/window threshold to
trade off in either direction. The candidate-confirm gate
remains the protection for alice's endpoint regardless — neither
the rotating-IP attacker nor the single-IP attacker can hijack
the slot, only briefly waste forwarding cycles.

False-positive risk:

- A real peer behind a flaky network that completes handshake
  but loses transport-data could in theory trip a strike. The
  threshold (2 failures within 60 s) and the fact that strikes
  only count for *unknown-source* candidates (a peer
  registered with a stable endpoint never goes through this
  path) make this rare.
- If it happens, an operator override clears it: `wg blocklist
  remove <ip>`.

### Operator surface

```
hyper-derp> wg blocklist list             # current blocks + expiry
hyper-derp> wg blocklist remove <ip>      # manual unblock
hyper-derp> wg blocklist add <ip> 1h      # manual block, ad-hoc
hyper-derp> wg blocklist policy 2 60 60   # tighten threshold
                              ↑   ↑   ↑
                        strikes  win  block (all in seconds)
```

Defaults are conservative; most operators won't need to touch
the policy.

## Use 2: Handshake validation

The same MAC1 computation gives us a misconfiguration filter for
free: arriving handshakes whose MAC1 doesn't match *any*
registered link partner's pubkey are dropped:

- A client pointed at the wrong relay (typo, stale config) gets
  rejected at the handshake.
- A stranger landing on a registered peer's source 4-tuple
  (NAT collision, DHCP churn) gets dropped instead of having
  their packets forwarded.
- New counter: `drop_handshake_no_pubkey_match`.

This filter is gated on the operator having stamped at least
one pubkey on the link. Operators who don't bother retain
today's "any source matching the registered IP:port forwards"
behaviour exactly. Engaging it is just `wg peer pubkey alice
<pub>` and `wg peer pubkey bob <pub>`.

## Security tradeoff

With the tentative-then-confirm gate above, the attack surface
collapses to:

| | |
|---|---|
| Attacker can establish a WG tunnel to bob? | **No.** They don't have alice's static private key. |
| Attacker can read alice's traffic? | **No.** Same reason. |
| Attacker can hijack alice's endpoint slot? | **No.** Only a candidate slot — the committed endpoint never moves until transport-data confirms, and the attacker can't produce post-handshake transport-data. |
| Attacker can disrupt alice's tunnel? | **No.** Old endpoint keeps forwarding throughout the candidate window. Real alice sees no interruption. |
| Attacker can chew CPU / memory at the relay? | **Marginally.** Each candidate registration is one map entry + one extra response forward, gated by per-peer cooldown (5 s) and per-source rate cap (100 / min). |
| Can the operator detect attempts? | Yes. `drop_relearn_unconfirmed` ticking is the canonical signal that someone tried and failed. `drop_relearn_source_rate` flags scanners. |
| Is this worse than today? | No, on every axis above. The relay gains automatic roaming without losing any of today's properties. |

The earlier draft's hijack window is closed: an attacker who
forges a handshake init drives `drop_relearn_unconfirmed`,
nothing else. Real alice's existing flow continues uninterrupted
from her old endpoint until her own next handshake — at which
point the candidate-confirm path commits the new endpoint.

The mitigation if you want to keep automatic roaming off for a
specific peer (datacentre / static-IP peers especially):

```
hdcli wg peer relearn-cooldown alice off
```

Combined with the source-IP allowlist from
[wg_relay_hardening.md](wg_relay_hardening.md), an operator can
fully lock down a peer to a specific CIDR while leaving roaming
on for mobile peers.

## What this is and is not

| | |
|---|---|
| Automatic roaming for mobile / dynamic-IP peers | ✅ |
| Misconfiguration catch | ✅ (free side-effect) |
| "Strangers can't accidentally route through this relay" | ✅ |
| Cryptographic peer authentication | ❌ — MAC1 is forgeable by anyone with the pubkey |
| Defence against handshake-spoof DoS | ⚠️ partial — attacker needs bob's pubkey, can disrupt for ≤ keepalive interval |
| Per-packet authentication on transport-data | ❌ — type 0x04 packets carry no pubkey, only receiver-index. wg.ko on the responder side is what authenticates them. |

## WG protocol surface (recap)

The wg-relay only ever sees four packet types on its UDP port:

| Type | First byte | MAC1 carries pubkey claim? | Length |
|------|-----------|---|---|
| 1 — handshake init | `0x01` | yes (responder's static) | 148 bytes |
| 2 — handshake resp | `0x02` | yes (responder's static) | 92 bytes |
| 3 — cookie reply | `0x03` | no (cookie-encrypted) | 64 bytes |
| 4 — transport data | `0x04` | no (only receiver-index) | ≥ 32 bytes, ≤ MTU |

So the pubkey-keyed filter and roaming are necessarily
handshake-only. Types 3 and 4 fall through to the source-4-tuple
match, exactly as today.

## Filter rule

For an arriving packet at the relay's wg-relay UDP port:

1. **If source 4-tuple matches a registered peer**:
   - Continue with today's link-partner forward.
   - For type 1/2 packets, optionally verify MAC1 against the
     partner's pubkey. Drop on mismatch with
     `drop_handshake_pubkey_mismatch++`. (Catches the case
     where alice's endpoint slot got reused by an unrelated
     client — they pass the source-IP check but their
     handshake isn't for bob.)
2. **If source 4-tuple does not match any registered peer** AND
   packet is type 0x01 or 0x02:
   - Compute `expected_mac1 = Blake2s(LABEL_MAC1 ++ partner_pub,
     pkt[0 .. -32])` for each linked partner's pubkey.
   - On match against `partner_x.pubkey`:
     - Identify the peer this packet is from as
       `partner_x.linked_to` (its link partner).
     - If the relearn cooldown for that peer hasn't elapsed,
       drop with `drop_relearn_cooldown++`.
     - Otherwise update that peer's endpoint to the new source,
       persist roster, increment `endpoint_relearn[peer]++`,
       forward as normal.
   - On no match, drop with `drop_handshake_no_pubkey_match++`.
3. **All other unrecognised packets** (type 3, 4, or no
   handshake type at all) — drop with `drop_unknown_src++`,
   today's behaviour unchanged.

## Counter summary

After this lands, `wg show` exposes:

| field | meaning |
|---|---|
| `drop_unknown_src` | unrecognised packet that wasn't a relearnable handshake |
| `drop_no_link` | source matched, but peer has no link |
| `drop_handshake_pubkey_mismatch` | type 1/2 from a registered source, but MAC1 didn't match the link partner's pubkey |
| `drop_handshake_no_pubkey_match` | type 1/2 from an unknown source, MAC1 didn't match any partner's pubkey |
| `drop_relearn_cooldown` | type 1/2 from unknown source, MAC1 matched, but per-peer cooldown is in effect |
| `drop_relearn_source_rate` | per-source-IP rate cap exceeded (≥100 / min) |
| `drop_relearn_unconfirmed` | candidate endpoint expired without transport-data confirming the relearn — strong signal of a forged-handshake attempt |
| `drop_blocklisted` | packet from a source IP currently on the dynamic blocklist |
| `blocklist_added` | source IP escalated onto the blocklist (event) |
| `endpoint_relearn[peer]` (per-peer counter) | times this peer's endpoint was successfully relearned (after confirm) |
| `endpoint_candidate[peer]` (per-peer counter) | times a candidate endpoint was registered (whether or not it confirmed) |

Per-peer view in `wg peer list`:

```
peer.0.name             alice
peer.0.endpoint         203.0.113.42:51820
peer.0.endpoint_relearn 3        ← three roams since last reset
peer.0.last_relearn_ns  ...
```

## Where it runs

Two places, mirroring the existing fast-path / userspace split:

- **Userspace path** — straightforward Blake2s in
  `src/wg_relay/forward.cc`, before the existing forward call.
  Adds one keyed Blake2s per handshake packet, which is rare
  (handshake every keepalive interval per session).
- **XDP fast path** — the BPF program already inspects the
  source endpoint and the partner MAC. We add a third map,
  `wg_partner_pubkey_state`, keyed by peer-id, value is 32 bytes
  of precomputed `Blake2s(LABEL_MAC1 ++ pub)` partial state
  (since the key is fixed per peer). Inline keyed Blake2s
  finishes the per-packet tail and compares the last 16 bytes.

When the BPF Blake2s is uncertain to fit the verifier limit,
the BPF program can `XDP_PASS` handshake packets to userspace
and let the userspace path enforce. Transport-data (the 99 %+
of bytes) stays on the XDP fast path.

For the **relearn** step, BPF can't write to the persistent
roster file; it has two options:

1. Update the BPF `wg_peer_endpoint` map directly + write a
   small ring-buffer record that userspace consumes and
   persists to the roster.
2. `XDP_PASS` the packet to userspace, which does the relearn,
   the roster write, then re-injects via the existing tap.

(2) is simpler and the rate is negligible (relearns happen at
human-roaming cadence, not per-packet), so likely the right
first cut.

## Operator surface

No new commands for the common case. The existing
`wg peer pubkey <name> <pub>` becomes load-bearing.

```
hyper-derp> wg peer pubkey alice <alice-pub>
hyper-derp> wg peer pubkey bob   <bob-pub>
```

Once both ends of a link have stamped pubkeys, both filtering
and automatic roaming engage.

Optional knobs:

```
hyper-derp> wg peer relearn-cooldown alice 30      # default 5 s
hyper-derp> wg peer relearn-cooldown alice off     # disable roaming for alice
```

`wg show config` is unchanged; `wg peer list` gains
`endpoint_relearn` and `last_relearn_ns` columns.

## Roster format change

The on-disk roster at `/var/lib/hyper-derp/wg-roster` already
has a `pubkey` field per peer. Add two optional fields:

```yaml
peers:
  - name: alice
    endpoint: 203.0.113.42:51820
    pubkey: xg4iUA5dIqy0o3qsYy6YKeZVMc/IEe4tVIahB7Onoz0=
    relearn_cooldown_ms: 5000   # new, default 5000
    endpoint_relearn: 3         # new, persisted across restart
```

Backwards compatible: existing rosters without the two new
fields default to `relearn_cooldown_ms: 5000` and
`endpoint_relearn: 0`.

## Failure modes

| | |
|---|---|
| Operator stamps wrong pubkey for one end | Every legitimate handshake from the other end fails MAC1; counters tick. Endpoint relearn never fires for that peer. Fix: re-stamp. |
| Operator stamps only one end | Filter inactive (gated on partner having a pubkey). Reverts to source-endpoint match only. No regression vs today. |
| Two attackers race a roam | Cooldown guarantees one update per peer per `relearn_cooldown_ms`. Real alice's next handshake re-stamps within ~ keepalive interval. |
| Real alice and an attacker race during roaming | Alice's keepalive cadence (typically 25 s) ensures the bad endpoint gets overwritten quickly. The disruption window scales with `PersistentKeepalive`. |
| Mobile peer behind double NAT, source port unstable | Each port change triggers a relearn (within cooldown). Acceptable for mobile peers; not great for high-churn NAT. The `relearn-cooldown off` knob disables roaming for peers where this is a problem. |

## Out of scope

- Cryptographic authentication of transport-data packets
  (impossible without terminating the tunnel).
- Per-session rate-limiting of handshakes (covered separately
  in [wg_relay_hardening.md](wg_relay_hardening.md)).
- Roaming for cookie-reply (type 3) — relearn is tied to
  handshake init/response only.
- Multi-link peers (today's iteration-1 invariant: one link per
  peer; multi-link mesh routing is its own future doc).

## Implementation sketch

1. **`bpf/wg_relay.bpf.c`** — add `wg_partner_pubkey_state`
   BPF_MAP_TYPE_HASH (key: peer_id, value: 32 B precomputed
   Blake2s state). For type 1/2 packets without a source-4-tuple
   match, iterate registered partners (small, bounded; ≤ peer
   count) and verify MAC1. On match, `XDP_PASS` to userspace
   for the actual relearn + forward.
2. **`src/wg_relay/forward.cc`** — userspace mirror for cold
   start and the relearn write path. Atomic roster rewrite is
   already there from `wg peer add`.
3. **`src/wg_relay/cli.cc`** — surface new counters in
   `wg show` / `wg peer list`. Add `wg peer relearn-cooldown`
   verb.
4. **Tests** —
   - libvirt fleet: register alice with one IP, bring her up
     from a different IP, assert tunnel comes up via relearn.
   - assert relearn-cooldown rate-limits.
   - assert handshake to a peer with no stamped partner pubkey
     still works (regression-safety of the gate).

## Estimated effort

~2 dev-days:

- 4 h: BPF Blake2s + map + XDP_PASS plumbing
- 4 h: userspace relearn + roster write + cooldown
- 3 h: CLI + counters
- 3 h: integration tests (libvirt fleet, mobile-IP simulation)
- 2 h: docs (quickstart roaming section, cli_handbook)
- 1 h: bench re-run on 25 GbE — verify XDP_TX/REDIRECT numbers
  are unchanged on transport-data path
