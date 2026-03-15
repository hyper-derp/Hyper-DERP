# Phase B — kTLS Full Sweep Plan

## Goal

Production-relevant benchmark dataset: HD with kTLS vs TS with
TLS across all vCPU configs. This is the primary dataset for
publication. Phase A (plain TCP) becomes the engineering appendix.

## Blocker

**P2: Fix /debug/workers under kTLS.** Worker stat files were
empty during the worker scaling kTLS runs. Without this, we
can't collect xfer_drops, send_drops, or worker imbalance data
under kTLS.

Options:
- Separate debug HTTP port (always plain, e.g., --debug-port 9090)
- `curl -k https://...` (skip cert validation for debug)
- Check if the issue was the collection script, not the endpoint

Fix and verify before starting the sweep. ~15 min.

## Pre-flight

- [ ] P2 fixed and verified (curl debug endpoint with kTLS active)
- [ ] `modprobe tls` on relay VM, verify with `lsmod | grep tls`
- [ ] HD binary: P3 bitmask build (ring size 4096 — default)
- [ ] Go derper: release build with TLS (same as Phase A)
- [ ] Confirm kTLS active: HD startup log shows `BIO_get_ktls_send`
- [ ] Self-signed certs deployed for HD and TS
- [ ] Bench client supports TLS connections to relay
- [ ] Dry run: 1 rate, 1 run, both servers, verify JSON output

## Configurations

| vCPU | VM Type | Workers | Notes |
|-----:|---------|--------:|-------|
| 2 | c4-highcpu-2 | 1 | Single worker, no cross-shard |
| 4 | c4-highcpu-4 | 2 | The cost story config |
| 8 | c4-highcpu-8 | 4 | Mid-range |
| 16 | c4-highcpu-16 | 8 | 8w is fine under kTLS (confirmed) |

No supplemental 2 vCPU / 2 workers — kTLS eliminates the
worker count regression, and 1w on 2 vCPU was already optimal.

### Order: 16 → 8 → 4 → 2

Same as Phase A. Fail fast on the most expensive VM.

## TS Ceiling Probes (must run first per config)

TLS will shift TS ceilings down. Need fresh probes before
each config's latency tests.

**Expected TS TLS ceilings** (from worker scaling data +
Phase A probe ratios):

| vCPU | Phase A TCP ceiling | Est. TLS ceiling | Probe rates |
|-----:|--------------------:|-----------------:|-------------|
| 2 | 2,000M | ~1,000-1,500M | 500, 1000, 1500, 2000, 3000 |
| 4 | 3,000M | ~2,000-2,500M | 1000, 2000, 2500, 3000, 5000 |
| 8 | 5,000M | ~3,500-4,500M | 2000, 3000, 4000, 5000, 7500 |
| 16 | 7,500M | ~6,500-7,000M | 3000, 5000, 7000, 7500, 10000 |

3 runs per probe rate. Take ceiling as first rate where loss > 5%.
Compute latency loads from actual ceiling.

## Rate Sweep

### Parameters
- 20 peers, 10 active pairs
- 15 seconds per run
- 1400 byte payload
- **DERP over kTLS (HD) / TLS (TS)**

### Runs per rate
- Low rates: **5 runs**
- High rates: **25 runs**

### Rate selection per config

Rates adjusted for expected lower ceilings under TLS.
Final rates depend on probe results.

| vCPU | Rates (estimated) |
|-----:|-------------------|
| 2 | 500M, 1G, 1.5G, 2G, 3G, 5G, 7.5G |
| 4 | 500M, 1G, 2G, 3G, 5G, 7.5G, 10G |
| 8 | 500M, 1G, 2G, 3G, 5G, 7.5G, 10G, 15G |
| 16 | 500M, 1G, 2G, 3G, 5G, 7.5G, 10G, 15G, 20G |

Adjust after probes. If HD is still climbing at the top rate,
add one higher step.

## Latency

### Methodology (same as Phase A)
- Ping/echo, two relay traversals per measurement
- 5000 pings per run, 500 warmup, 4500 samples
- 10 runs at low loads, 15 runs at ceiling/above

### Load levels (scaled to TLS ceiling)

| Level | Description |
|:------|:------------|
| idle | 0 background |
| ceil25 | 25% of TS TLS ceiling |
| ceil50 | 50% of TS TLS ceiling |
| ceil75 | 75% of TS TLS ceiling |
| ceil100 | 100% of TS TLS ceiling |
| ceil150 | 150% of TS TLS ceiling |

Actual rates computed from probe results per config.

## Data Collection

### Per-run
- Throughput, loss, retransmits (bench harness)
- Relay CPU: pidstat with 1s timestamps (per-phase, not per-run)
- System CPU: mpstat with 1s timestamps (per-phase)

### Per-rate (HD only)
- /debug/workers before + after (xfer_drops, send_drops,
  recv imbalance) — **requires P2 fix**

### Per-config
- system_info.txt (VM type, kernel, CPU, worker count, rates)
- HD version (commit hash)
- TS version (`go version -m`)
- `cat /proc/net/tls_stat` before and after HD runs
  (confirm kTLS software path active, count sessions)
- RSS at 1s intervals

### Per-config (optional, one run each)
- TS GC trace at TLS ceiling rate: `GODEBUG=gctrace=1`
- TS GOGC=off at TLS ceiling rate
- These proved valuable in Phase A (GC not the bottleneck,
  GOGC=off makes TS worse). Confirm same holds under TLS.

## Comparison Protocol

Per vCPU config:

1. Resize VM (or use pre-provisioned VM)
2. `modprobe tls`, verify loaded
3. Record system_info
4. Start HD with kTLS, verify `BIO_get_ktls_send` in log
5. **TS ceiling probe** (3 runs × 5 rates)
6. Compute latency load levels from probe
7. HD rate sweep
8. HD latency suite
9. HD GC-equivalent data: /proc/net/tls_stat, RSS
10. Kill HD, `echo 3 > /proc/sys/vm/drop_caches`, wait 10s
11. Start TS with TLS
12. TS rate sweep
13. TS latency suite
14. TS GC trace run (one run at ceiling)
15. TS GOGC=off run (one run at ceiling)
16. Kill TS

**Note:** TS ceiling probe runs HD first (step 5 uses TS, but
we need TS ceiling → run TS probe first, then HD sweep).

Corrected order:
1. Resize VM
2. `modprobe tls`, verify
3. Record system_info
4. Start TS
5. **TS ceiling probe** (3 runs × 5 rates)
6. TS rate sweep
7. TS latency suite
8. TS GC trace + GOGC=off
9. Kill TS, drop caches, wait 10s
10. Compute latency load levels from TS probe
11. Start HD with kTLS
12. HD rate sweep
13. HD latency suite
14. HD worker stats, /proc/net/tls_stat, RSS
15. Kill HD

Run TS first because we need its ceiling to set latency
load levels for both relays.

## Data Organization

```
bench_results/gcp-c4-phase-b/
  16vcpu/
    system_info.txt
    probe/
      ts_tls_3000_r{01..03}.json
      ...
    rate/
      hd_500_r{01..05}.json
      ...
      ts_500_r{01..05}.json
      ...
    latency/
      hd_idle_r{01..10}.json
      hd_ceil25_r{01..10}.json
      ...
      ts_idle_r{01..10}.json
      ...
    cpu/
      hd_rate_pidstat.txt
      hd_rate_mpstat.txt
      hd_rate_rss.txt
      hd_lat_pidstat.txt
      ...
      ts_rate_pidstat.txt
      ...
    workers/
      hd_rate_5000_before.json
      hd_rate_5000_after.json
      ...
    gc_trace/
      ts_gctrace.log
      ts_gctrace_rate.json
      ts_gogcoff.json
    tls_stat/
      hd_before.txt
      hd_after.txt
    DONE
  8vcpu/
    ...
  4vcpu/
    ...
  2vcpu/
    ...
  gen_report.py
  REPORT.md
  plots/
```

## Time Estimate

Per vCPU config:

| Phase | Runs | Time/run | Subtotal |
|-------|-----:|--------:|---------:|
| TS probe (5 rates × 3 runs) | 15 | ~20s | ~5 min |
| TS low rate sweep | 20-35 | ~20s | ~12 min |
| TS high rate sweep | 75-125 | ~20s | ~40 min |
| TS latency (6 loads × 10-15 runs) | 70 | ~35s | ~41 min |
| TS GC trace + GOGC=off | 2 | ~20s | ~1 min |
| HD low rate sweep | 20-35 | ~20s | ~12 min |
| HD high rate sweep | 75-125 | ~20s | ~40 min |
| HD latency (6 loads × 10-15 runs) | 70 | ~35s | ~41 min |
| **Per config** | | | **~3.5 hrs** |

| Config | Time |
|-------:|-----:|
| 16 vCPU | ~3.5 hrs |
| 8 vCPU | ~3.5 hrs |
| 4 vCPU | ~3 hrs |
| 2 vCPU | ~2.5 hrs |
| **Total** | **~12.5 hrs** |

Overnight run. Start evening, results by morning.

## Post-Run Validation

- [ ] Count files: expected vs actual per config
- [ ] Check for empty JSON or worker stat files (P2 regression)
- [ ] Verify /proc/net/tls_stat shows TlsCurrTxSw > 0 (kTLS active)
- [ ] Spot-check: low-rate throughput ≈ offered rate
- [ ] Spot-check: HD and TS match at low rates
- [ ] Check CV% — flag > 15%
- [ ] Compare TS TLS ceiling to worker scaling TS TLS data
  (should be consistent: ~7.8 Gbps at 16 vCPU)
- [ ] Verify no bimodal collapse on HD at any config (kTLS
  should prevent it — if seen, investigate immediately)

## Key Questions Phase B Answers

1. **How much does TLS cost HD?** Phase A 4w TCP vs Phase B
   kTLS at matched configs. Worker scaling showed ~7% at ≤10G.

2. **How much does TLS cost TS?** Phase A TCP vs Phase B TLS.
   Worker scaling showed ~10% at 16 vCPU. Will be worse at
   lower vCPU counts where CPU is tighter.

3. **Does HD's advantage grow or shrink under TLS?** At 16 vCPU
   it was 1.4x under TLS (worker scaling). At 2/4 vCPU where
   TS is more CPU-constrained, expect larger ratios.

4. **What's HD's kTLS ceiling at each config?** Is 2 vCPU
   limited by crypto (single core doing AES-GCM) or by relay
   architecture?

5. **Does the latency advantage hold under TLS?** Phase A
   showed 3-8x p99 advantage. TLS adds per-record processing
   which could affect tail latency differently for kTLS (kernel)
   vs userspace TLS (Go).

6. **Is the cost story real?** Can HD on 2 vCPU with kTLS
   match or exceed TS on 8 vCPU with TLS? That's the
   "cut your relay bill 75%" slide.
