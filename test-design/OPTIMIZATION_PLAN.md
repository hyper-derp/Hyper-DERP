# Optimization Plan — Post Worker Scaling Analysis

## Context

The worker scaling test confirmed:
- 4w is optimal for plain TCP on 16 vCPU
- kTLS eliminates the 8w collapse (~10.8 Gbps stable)
- But even 4w shows death spiral at 20G (min 595 Mbps)
- 6w has xfer_drops at 5G while 4w has zero — sharp cliff
- Hash distribution is poor at low peer counts (6x imbalance)
- kTLS worker stats are missing (empty files — endpoint issue)

## Worker Scaling Findings (updated)

### TLS overhead is smaller than expected

- **TS TLS cost: ~10%** at saturation (8,760 → 7,845 Mbps).
  Go's crypto/tls with AES-NI is efficient on 16 vCPU.
- **HD kTLS cost: ~7%** at rates where TCP is stable (≤10G).
  Above 10G the comparison is meaningless because TCP is bimodal.
- **8w kTLS outperforms 4w kTLS at 10G+** (10,782 vs 9,657 at
  15G). More workers helps under kTLS — parallelism pays off
  when the send path is throttled. This reverses the plain TCP
  finding where 4w was strictly better.

### Production ratios (HD kTLS vs TS TLS)

| Rate  | HD 4w | HD 8w | TS    | 4w ratio | 8w ratio |
|------:|------:|------:|------:|---------:|---------:|
| 10G   | 8,109 | 8,438 | 7,455 | 1.09x   | 1.13x    |
| 15G   | 9,657 |10,782 | 7,845 | 1.23x   | 1.37x    |
| 20G   |10,623 |10,991 | 7,789 | 1.36x   | 1.41x    |

The 1.4x advantage at saturation is solid but not as dramatic as
the 3-6x seen in plain TCP. TLS levels the playing field because
both sides are now CPU-bound on crypto, not on relay architecture.

**The larger vCPU advantage ratios (4 vCPU: 6x) will likely
compress less under TLS because at lower core counts, TS hits
goroutine scheduling limits before crypto limits.** Phase B on
2/4 vCPU will confirm.

## Priority 1: Backpressure Hysteresis

### Problem

The recv_paused mechanism oscillates: pause → send queue drains
→ resume → flood → pause. At high offered rates this amplifies
into a death spiral where some runs collapse to <1 Gbps. Affects
all worker counts, but more workers = more oscillation.

Evidence:
- 4w TCP at 20G: min 595 Mbps, max 16824 Mbps (same config)
- 8w TCP at 20G: min 403 Mbps, max 15491 Mbps
- kTLS doesn't oscillate because crypto throttles the send path

### Fix

Widen the gap between pause and resume thresholds in
`data_plane.cc`. Current behavior (suspected): pause when send
queue exceeds high watermark, resume immediately when it drops
below the same threshold.

Change to:
- Pause recv when send queue > `kSendPressureHigh` (e.g., 80%)
- Resume recv only when send queue < `kSendPressureLow` (e.g., 30%)
- The 50% gap prevents rapid toggling

Look for `recv_paused`, `SendPressure`, or backpressure-related
constants in `data_plane.cc`.

### Validation

10 runs at 20G with 4w, plain TCP. Compare:
- Before: min 595, mean 10513, bimodal
- After: should be stable, no runs below 10G
- Also check 8w at 15G — should improve too

### Impact

If this works, it fixes the plain TCP variance at ALL worker
counts. The 16 vCPU story gets clean even without kTLS.

**Reduced priority for production:** kTLS naturally prevents
the oscillation. Hysteresis still matters for plain TCP
benchmarks and for bare metal where you might push past the
kTLS ceiling. But it's no longer blocking Phase B.

## Priority 2: Fix /debug/workers Under kTLS

### Problem

All kTLS worker stat files are 0 bytes. The collection script
either:
- Tried HTTP on the debug port (needs HTTPS with kTLS active)
- The debug endpoint isn't bound when TLS is enabled
- Curl didn't have the right cert/insecure flag

### Fix

Check how the debug HTTP server is configured when TLS is
enabled. Options:
- Bind debug endpoint on a separate plain HTTP port (simplest)
- Use `curl -k` (insecure) to skip cert validation for debug
- Add a `--debug-port` flag that's always plain HTTP

### Why It Matters

Phase B (kTLS full sweep) needs xfer_drop data to confirm the
"TLS prevents ring overflow" hypothesis. Without it we're
assuming, not proving.

### Validation

Start HD with kTLS, connect a client, curl the debug endpoint.
Verify JSON response is non-empty.

## Priority 3: ProcessXfer Bitmask — VALIDATED

### Implementation

- `types.h:310`: `alignas(64) uint32_t pending_sources` on Worker
  struct. Separate cache line to avoid false sharing.
- `data_plane.cc ForwardMsg`: atomic OR of sender bit on push.
- `data_plane.cc ProcessXfer`: `__atomic_exchange_n` + `__builtin_ctz`
  bitmask loop. Only drains inboxes with pending data.

### Results

| Config | Baseline | P3 | Delta |
|--------|--------:|---------:|------:|
| 8w kTLS @15G | 10,782 Mbps | 11,306 Mbps | **+4.9%** |
| 8w kTLS @20G | 10,991 Mbps | 12,470 Mbps | **+13.5%** |
| 8w TCP xfer_drops @15G | 13,759,054 | 4,414,067 | **-68%** |

kTLS loss at 20G dropped from 9.0% to 1.1%. Production HD/TS
ratio at 20G improved from 1.41x to 1.60x.

Plain TCP bimodality unchanged (P1 territory).

### Status: MERGED — include in all future builds and sweeps.

## Priority 4: Hash Distribution

### Problem

FNV-1a on 32-byte keys distributes 20 peers across 8 workers
with 1.7x imbalance. 40 peers shows up to 6x imbalance on
some runs. Hot workers overflow their SPSC inboxes first.

### Options (pick one)

**A. Better hash function**
Switch to wyhash or xxHash3. Unlikely to make a big difference
— the problem is small N (20 peers / 8 workers), not hash
quality.

**B. Power-of-two-choices assignment**
On connect, hash to two candidate workers. Assign to the one
with fewer current peers. Simple, good balance, maintains some
locality.

**C. Round-robin assignment**
Assign peers to workers in connect order. Perfect balance.
Loses hash-based locality but for a relay this doesn't matter
— there's no affinity benefit.

**D. Document the limitation**
"Use workers ≤ peers/4 for optimal distribution." This is
honest and avoids code complexity. For most deployments (50+
peers) the hash distribution is fine.

### Recommendation

Option D for now. The real-world peer count will be much higher
than 20. Revisit if bare metal profiling shows hash imbalance
as a bottleneck with realistic peer counts.

## Priority 5: Adaptive SPSC Ring Sizing

### Problem

Fixed 16384 ring capacity. With 4 workers (3 inboxes each),
rings never overflow. With 8 workers (7 inboxes each), they
overflow at 5G+. The same capacity is either wasteful or
insufficient depending on worker count.

### Fix

Scale ring capacity with worker count:

```
size_t ring_capacity = kBaseRingSize * (num_workers - 1);
```

Or simpler: `ring_capacity = kBaseRingSize * num_workers / 2`.

More workers = more cross-shard traffic per ring = larger rings.

### Why It's Low Priority

With backpressure hysteresis (P1) and ProcessXfer bitmask (P3),
the rings may never overflow even at current size. Only revisit
if xfer_drops persist after those fixes.

## Execution Order

```
P1 (hysteresis)  ──→  validate 10 runs  ──→  good?
                                               │
P2 (debug port)  ──→  quick fix + verify       │
                                               ▼
P3 (bitmask)     ──→  validate 10 runs  ──→  both good?
                                               │
                                               ▼
                                         Phase B (kTLS sweep)
                                               │
                                               ▼
                                         Bare metal setup
                                               │
                                               ▼
                                         P4/P5 only if profiling
                                         shows they matter
```

P2 is the only blocker for Phase B. P1 and P3 are optimizations
that improve plain TCP numbers but don't affect the production
(kTLS) story. They become more important for bare metal where
higher throughput pushes past the kTLS ceiling.

**Revised priority for Phase B:**
- P2 (debug port): must fix before Phase B
- P1 (hysteresis): nice to have, not blocking
- ~~P3 (bitmask): bare metal optimization, defer~~ **DONE**

**8w kTLS outperforming 4w kTLS** means Phase B should test
auto worker count (vCPU/2) rather than capping at 4. The
plain TCP finding of "4 is optimal" does not apply under TLS.

P3 results reinforce this: 8w kTLS now does 12,470 Mbps at
20G (+13.5% over pre-bitmask). More workers + bitmask + kTLS
is the optimal production configuration.

## Time Estimate

| Item | Code | Test | Total |
|------|-----:|-----:|------:|
| P1: Hysteresis | 30 min | 1 hr | 1.5 hr |
| P2: Debug port | 15 min | 15 min | 30 min |
| P3: Bitmask | 1 hr | 1 hr | 2 hr |
| P4: Hash | — | — | deferred |
| P5: Ring sizing | — | — | deferred |
| **Total** | | | **~4 hr** |

After P1-P3, proceed to Phase B (kTLS full sweep) with the
fixes in place. This gives the cleanest possible dataset for
publication.
