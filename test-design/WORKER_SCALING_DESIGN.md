# Worker Count Scaling — Test Design

## Goal

Determine the optimal worker count on 16 vCPU and identify the
source of the diminishing return observed in the full sweep.

## Motivation

The full sweep revealed a paradox:

- **8 vCPU / 4 workers**: 14.8 Gbps peak, 3.0x ratio over TS
- **16 vCPU / 8 workers**: 11.8 Gbps peak, 1.3x ratio over TS

More cores made HD **worse**. The 16 vCPU result is the only config
where HD doesn't clearly dominate, and it's the only one where the
worker count exceeds 4.

Additionally, at 20G offered on 16 vCPU, HD drops to 8.4 Gbps with
51% loss — **worse than at 15G** (11.8 Gbps, 9% loss). This is not
normal saturation behavior; something is actively breaking at high
load with 8 workers.

## Evidence from Full Sweep Worker Stats

The /debug/workers data collected during the full sweep already
narrows the hypothesis space:

### 16 vCPU (8 workers) at 15G:
- Recv imbalance: 1.39x (179GB to 250GB across workers)
- **Total xfer_drops: 32.6M** — massive cross-shard frame loss
- xfer_drops unevenly distributed (132K to 6.7M per worker)
- send_drops present on 5 of 8 workers (~2K each)

### 8 vCPU (4 workers) at 15G:
- Recv imbalance: 1.05x (411GB to 433GB — nearly perfect)
- **Total xfer_drops: 3.2M** — 10x fewer than 16 vCPU
- Only 2 of 4 workers have xfer_drops

### Key observations:
1. **xfer_drops are the smoking gun.** 32.6M dropped frames at
   16 vCPU vs 3.2M at 8 vCPU, at the same offered rate. The
   cross-shard SPSC rings are overflowing.
2. **Hash imbalance is real but moderate.** 1.39x at 8 workers
   vs 1.05x at 4 workers. Not catastrophic, but it means some
   workers are hotter than others, accelerating ring overflow.
3. **The bimodality at 20G+ is likely cascade failure.** Once
   xfer rings start dropping, the destination worker's send queue
   drains, reducing backpressure, causing recv to accelerate,
   which produces more cross-shard traffic, which overflows more
   rings. Some runs enter this death spiral, others don't.

## The TLS Factor

**Critical context from earlier testing:** enabling kTLS pushed loss
rates to zero in configurations that showed significant loss over
plain TCP. TLS encryption on the send path adds per-record overhead
that naturally rate-limits output — acting as implicit backpressure.

This reframes the entire 16 vCPU problem:

1. **Plain TCP lets the relay send faster than SPSC rings can
   drain.** Without TLS, workers blast packets at wire speed. The
   cross-shard rings overflow because destination workers can't
   consume fast enough. With kTLS, the send-side crypto throttles
   output to what the rings can sustain.

2. **The xfer_drop cascade may not exist with kTLS.** If TLS
   backpressure keeps send queues bounded, SPSC rings never fill,
   and the bimodal collapse at 20G doesn't trigger.

3. **Production always uses TLS.** Nobody runs DERP without TLS.
   The plain TCP benchmark is a stress test of relay internals,
   not a deployment scenario. The TLS benchmark is the one that
   matters for the "replace your derper" story.

4. **Both should be reported.** Plain TCP shows the raw
   packet-shuffling ceiling. TLS shows production-relevant
   performance. The comparison tells you how much headroom TLS
   encryption consumes.

**Implication for this test:** the worker scaling sweep must include
TLS runs. If 8 workers on 16 vCPU works fine with kTLS, the
"regression" is a plain-TCP-only artifact and the deployment
guidance is simply "use TLS" (which you'd do anyway).

## Hypotheses (updated)

### H1: SPSC xfer ring overflow (STRONG — evidence supports)

The SPSC rings have a fixed capacity. With 8 workers, each worker
has 7 inboxes to poll. If a worker falls behind processing one
inbox (because it's busy with local traffic or other inboxes), the
source worker's writes to that ring overflow and frames are dropped
as xfer_drops. At 4 workers, each worker has only 3 inboxes — less
opportunity for one to fall behind.

**Fix options:**
- Larger SPSC ring capacity (currently unknown — check code)
- Adaptive ring sizing based on worker count
- Backpressure from destination to source when ring is near full

### H2: Cross-shard overhead scales as N²

SPSC rings are per source-destination pair: N workers = N×N ring
buffers + up to N eventfd signals per CQE batch. At 8 workers
that's 64 ring pairs vs 16 at 4 workers. ProcessXfer must poll
all N-1 inboxes — more workers = more polling overhead even when
rings are empty.

### H3: Cache thrashing

Each worker owns an io_uring instance, provided buffer ring, slab
allocator, and peer hash table. At 8 workers on 16 vCPU, aggregate
working set may exceed L2 and spill into shared L3, causing
cross-core cache line bouncing on SPSC ring head/tail pointers.

### H4: Hash distribution imbalance (CONFIRMED — moderate)

Recv imbalance of 1.39x at 8 workers confirms uneven distribution.
FNV-1a on 32-byte keys with only 20 peers across 8 workers leaves
some workers with 1-2 peers and others with 3-4. This isn't the
root cause but it amplifies H1 — the hot workers overflow first.

### H5: 20G collapse is cascade failure (LIKELY)

The per-run data at 20G is starkly bimodal:
- 14 runs cluster at ~4,061 Mbps (death spiral)
- 11 runs cluster at ~14,006 Mbps (healthy)

This is not random variance. Something triggers a catastrophic
mode switch. The xfer_drop cascade described above is the most
likely mechanism.

## Test 1: Worker Count Sweep

### Configuration
- **VM**: c4-highcpu-16 (same as full sweep)
- **Worker counts**: 2, 4, 6, 8
- **Rates**: 5G, 7.5G, 10G, 15G, 20G (the range where differences appear)
- **Runs**: 10 per rate per worker count
- **Transport**: plain TCP first, then repeat 4w and 8w with kTLS
- **Both HD and TS** (TS is the control — should be identical across
  HD worker count changes)

### What we expect
- **4 workers should match 8 vCPU performance** (~14.8 Gbps peak).
  If it does, the issue is confirmed as worker-count-dependent.
- **6 workers**: expect intermediate — some xfer_drops but less
  than 8 workers. If 6 is nearly as good as 4, the cliff is
  between 6 and 8.
- **2 workers**: should eliminate almost all cross-shard overhead
  but may underperform due to insufficient parallelism.
- **xfer_drops per worker count** is the key diagnostic. Plot
  total xfer_drops vs worker count at matched offered rates.

### Data to collect per run
- Throughput, loss (existing bench harness)
- Per-run CPU via pidstat
- HD /debug/workers snapshot at end of run — traffic per worker,
  queue depths, recv_paused state

### Per worker count (before+after runs)
- `perf stat -e cache-misses,cache-references,instructions,cycles`
  for one 15s run at 15G. Gives IPC and cache miss rate.
  Compare across worker counts.

## Test 2: 20G Collapse Investigation

### Configuration
- **VM**: c4-highcpu-16
- **Workers**: 4 (expected best) and 8 (broken case)
- **Rates**: 15G, 17.5G, 20G, 22.5G, 25G (fine-grained around
  the collapse point)
- **Runs**: 10 per rate
- **Transport**: plain TCP, then repeat 8w with kTLS

### What we're looking for
- Does the collapse happen with 4 workers? If not, it's a
  cross-shard issue, not a fundamental throughput limit.
- **Does the collapse happen with kTLS on 8 workers?** If not,
  TLS backpressure prevents the cascade and the problem is
  academic.
- At what rate does throughput stop climbing and start falling?
- Is the collapse sudden (cliff) or gradual (plateau then decline)?

### Additional instrumentation
- Poll HD /debug/workers at 1s intervals during the run to
  capture recv_paused transitions. If recv_paused is toggling
  rapidly (multiple times per second), that's the oscillation.
- Check for send queue depth spikes correlating with throughput
  drops.
- **Compare xfer_drops between plain TCP and kTLS at matched
  offered rates.** This directly tests whether TLS backpressure
  prevents ring overflow.

## Test 3: Diagnose the Mechanism

Run after Tests 1-2 to confirm which hypothesis holds.

### If H1 suspected (cross-shard overhead)
```bash
# Profile eventfd syscall frequency at 4 vs 8 workers, 15G
perf trace -e write,read -p <pid> -- sleep 5 2>&1 | \
  grep eventfd | wc -l
```
If eventfd calls scale much faster than N, signaling overhead is
the issue.

### If H2 suspected (cache thrashing)
```bash
# Compare cache behavior at 4 vs 8 workers, same rate
perf stat -e cache-misses,cache-references,L1-dcache-load-misses,\
LLC-load-misses,LLC-store-misses -p <pid> -- sleep 10
```
If LLC miss rate jumps significantly at 8 workers, working sets
are colliding.

### If H4 suspected (hash imbalance)
Already confirmed at 1.39x for 8 workers. But is it a contributor
or just noise?
- Test 8 workers with 40 peers — should improve distribution
- If 40 peers + 8 workers significantly reduces xfer_drops and
  improves throughput, hash imbalance is load-bearing
- If performance doesn't change, imbalance is cosmetic

### Ring capacity check (do first — costs nothing)
```bash
# Find SPSC ring size in the source code
grep -r "xfer.*capacity\|kXferRing\|SPSC.*size" ~/dev/Hyper-DERP/src/
```
If ring capacity is small (e.g., 1024 entries), doubling it might
fix the xfer_drops entirely without changing worker count. This
would be the simplest fix if the ring is undersized for 8 workers.

## Order of Operations

1. Check SPSC ring capacity in source code (5 min, free).
2. Run Test 1 plain TCP worker sweep (2, 4, 6, 8). ~3 hours.
3. Analyze: identify whether 4 workers fixes 16 vCPU.
4. Run Test 1 kTLS for 4w and 8w only. ~1.5 hours.
5. **Decision point:** If kTLS eliminates the 8w regression,
   the collapse is a plain-TCP artifact. Skip Test 2 for plain
   TCP and focus on TLS-mode characterization. If kTLS doesn't
   help, proceed to Test 2.
6. Run Test 2 (collapse investigation). ~1.5 hours.
7. Based on results, run relevant Test 3 diagnostics. ~30 min.

**Total: ~6.5-8 hours** depending on whether kTLS resolves it.

## What This Tells Us for the Future

The worker scaling question matters beyond current benchmarks:

- On bare metal with 50GbE, we'll have 32+ cores available.
  If 4 workers is optimal regardless of core count, the remaining
  cores go to kernel networking / IRQ processing — which may be
  exactly right at 4.5M pps.
- If the optimal worker count is f(bandwidth) not f(cores), the
  deployment guidance is: "set workers to 4, use remaining cores
  for the OS." Simple and predictable.
- If the issue is N² cross-shard scaling, the architecture may
  need a hierarchical forwarding model for high worker counts:
  workers grouped into pods, cross-pod forwarding through a
  designated gateway worker. But that's a big change — only
  pursue if bare metal profiling confirms the need.

## Exit Criteria

- We know the optimal worker count for 16 vCPU (likely 4).
- We can explain why more workers hurts (xfer_drops confirmed
  as mechanism, root cause identified).
- We know if the 20G collapse is worker-count-dependent.
- **We know if kTLS eliminates the regression.** This determines
  whether it's a real production issue or a stress-test artifact.
- We have deployment guidance: "use N workers for M vCPU."
- We know whether the fix is "use fewer workers" (config change)
  or "increase ring capacity / add ring backpressure" (code change).
  The latter would unblock higher worker counts for bare metal.

## Implications for the Full Sweep

If kTLS resolves the 16 vCPU issue, the full sweep should be
rerun with TLS for all configs. The report would then present:

- **Plain TCP** as the raw relay ceiling (stress test)
- **kTLS** as production-relevant performance
- Both HD and TS with TLS for fair comparison (TS also benefits
  from the backpressure, so the ratio may shift)

This also affects the full sweep design doc — add a TLS column
to the test matrix for the next round (GCP rerun or AWS).
