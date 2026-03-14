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

## Hypotheses

### H1: Cross-shard overhead scales as N²

SPSC rings are per source-destination pair: N workers = N×N ring
buffers + up to N eventfd signals per CQE batch. At 8 workers
that's 64 ring pairs vs 16 at 4 workers. The signaling and polling
cost may outweigh the parallelism benefit.

### H2: Cache thrashing

Each worker owns an io_uring instance, provided buffer ring, slab
allocator, and peer hash table. At 8 workers on 16 vCPU, aggregate
working set may exceed L2 and spill into shared L3, causing
cross-core cache line bouncing on SPSC ring head/tail pointers.

### H3: Kernel resource contention

More io_uring instances = more kernel-side bookkeeping. TCP stack
is shared; more workers means more concurrent socket operations
competing for netfilter/routing/socket lock paths.

### H4: Hash distribution imbalance

FNV-1a on 32-byte keys with 20 peers across 8 workers — some
workers may get 0-1 active peers while others get 3-4. At 4 workers
the distribution is more even by pigeonhole.

### H5: 20G collapse is backpressure pathology

The throughput drop from 15G to 20G (11.8G → 8.4G) suggests the
backpressure mechanism is not just limiting throughput but actively
degrading it. Possible recv_paused oscillation amplified by
cross-shard signaling delays with 8 workers.

## Test 1: Worker Count Sweep

### Configuration
- **VM**: c4-highcpu-16 (same as full sweep)
- **Worker counts**: 2, 4, 6, 8
- **Rates**: 5G, 7.5G, 10G, 15G, 20G (the range where differences appear)
- **Runs**: 10 per rate per worker count
- **Both HD and TS** (TS is the control — should be identical across
  HD worker count changes)

### What we expect
- If H1/H2/H3: gradual degradation as workers increase.
  4 workers ≈ 8 vCPU performance. 6 workers in between.
- If H4: performance depends on specific peer-to-worker mapping.
  Changing peer count (e.g., 40 instead of 20) would change the
  result. Check /debug/workers for skew.
- If H5: the 20G collapse happens only at 8 workers, not at 4.

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

### What we're looking for
- Does the collapse happen with 4 workers? If not, it's a
  cross-shard issue, not a fundamental throughput limit.
- At what rate does throughput stop climbing and start falling?
- Is the collapse sudden (cliff) or gradual (plateau then decline)?

### Additional instrumentation
- Poll HD /debug/workers at 1s intervals during the run to
  capture recv_paused transitions. If recv_paused is toggling
  rapidly (multiple times per second), that's the oscillation.
- Check for send queue depth spikes correlating with throughput
  drops.

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
- Check /debug/workers output: messages_forwarded per worker
- If skew > 2:1 between busiest and quietest worker, hash
  distribution is the problem
- Test with 40 peers (more even distribution across 8 workers)
- Test with 80 peers (should further equalize)

## Order of Operations

1. Run Test 1 (worker sweep). ~3 hours.
2. Analyze: identify whether 4 workers fixes 16 vCPU.
3. Run Test 2 (collapse investigation) with best + worst worker
   count. ~1.5 hours.
4. Based on results, run relevant Test 3 diagnostics. ~30 min.

**Total: ~5 hours.**

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
- We can explain why more workers hurts.
- We know if the 20G collapse is worker-count-dependent.
- We have deployment guidance: "use N workers for M vCPU."
