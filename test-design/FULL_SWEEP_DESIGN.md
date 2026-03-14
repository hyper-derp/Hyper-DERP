# Full vCPU Sweep — Test Design (2026-03-14)

## Goal

Definitive GCP benchmark dataset across 2, 4, 8, 16 vCPU.
25 runs at high rates for tight confidence intervals.
All configs use SPSC code + release Go build.

## What Changed Since Earlier Runs

| Issue | Before | Now |
|-------|--------|-----|
| Go derper build | Debug (1.96.1-ERR-BuildInfo) | Release, stripped, -trimpath |
| Cross-shard model | MPSC + Treiber stack (CAS contention) | SPSC rings + batched eventfd |
| 8 vCPU result | TS won (0.93x) | HD wins (2-3x, prelim) |

All prior 4/8/16 vCPU data is obsolete. Everything must be rerun.

## Configurations

| vCPU | VM Type | Workers | Notes |
|-----:|---------|--------:|-------|
| 2 | c4-highcpu-2 | 1 (+ supplemental 2) | Primary: 1 worker (no cross-shard). Supplemental: 2 workers. |
| 4 | c4-highcpu-4 | 2 | Strongest prior result. Rerun with Go release. |
| 8 | c4-highcpu-8 | 4 | SPSC fix validated in prelim. Full run. |
| 16 | c4-highcpu-16 | 8 | Previously contaminated. Clean rerun. |

### Order: 16 → 8 → 4 → 2

Start with the most expensive VM and work down. If something goes
wrong (script bug, config error), you find out on the first run and
fix it before burning hours on the full matrix.

## Rate Sweep

### Parameters (unchanged)
- 20 peers, 10 active pairs
- 15 seconds per run
- 1400 byte payload (WireGuard MTU)
- DERP over plain TCP, no TLS

### Runs per rate
- Low rates (500M, 1G, 2G, 3G): **5 runs**
  Was 3. Costs almost nothing in time, gives a real CI instead
  of a 3-sample joke. Also catches any weird startup effects.
- High rates (5G, 7.5G, 10G, 15G, 20G): **25 runs**

### Rate selection per config

Not every config needs every rate. Rates above the VM's NIC cap
or well past saturation produce noise, not signal.

| vCPU | Rates to run | Rationale |
|-----:|-------------|-----------|
| 2 | 500M, 1G, 2G, 3G, 5G, 7.5G, 10G | Likely saturates ~3-5G |
| 4 | 500M, 1G, 2G, 3G, 5G, 7.5G, 10G, 15G | Saturates ~10G |
| 8 | 500M, 1G, 2G, 3G, 5G, 7.5G, 10G, 15G, 20G | Full range |
| 16 | 500M, 1G, 2G, 3G, 5G, 7.5G, 10G, 15G, 20G, 25G | Extended to find HD ceiling |

If a config shows zero loss at its highest rate, consider adding
one higher rate to find the actual ceiling. But don't fish for it
during the run — decide beforehand.

### Per-run data (rate sweep)

Collected by bench harness (existing):
- messages_sent, messages_recv → loss%
- throughput_mbps
- connect_time_ms, send_errors
- timestamp, config metadata

**Add if not already present:**
- relay_cpu_pct: `pidstat -p <relay_pid> 1 15` running in parallel,
  report mean CPU% over the 15s window. Captures per-run CPU, not
  just a summary after all runs.
- total_system_cpu: `mpstat 1 15` in parallel, for context on
  kernel/IRQ overhead.

## Latency

### Current methodology (keep)
- Ping/echo pattern: send through relay, echo back through relay.
  Two relay traversals per measurement. This is fine — it's
  consistent and documented.
- 5000 pings per run, first 500 discarded as warmup → 4500 samples.
- 10 runs per load level → 45,000 samples per data point.

### Change 1: Scale background loads to config capacity

Fixed loads (idle, 1G, 3G, 5G, 8G) don't make sense across configs.
8G on 2 vCPU is way past saturation; 8G on 16 vCPU is light load.
Measure at percentages of each config's observed TS saturation
point, which is the meaningful comparison threshold.

| Load level | Description | Rationale |
|:-----------|:------------|:----------|
| idle | 0 background | Baseline. Measures kernel TCP RTT only. |
| 25% of TS ceiling | Light load | Both servers comfortable. |
| 50% of TS ceiling | Medium load | TS starting to work. |
| 75% of TS ceiling | Heavy load | TS near saturation. |
| 100% of TS ceiling | At TS limit | TS at its max. HD should have headroom. |
| 150% of TS ceiling | Past TS limit | TS dropping packets. HD still handling it? |

Determine TS ceiling from the rate sweep data (first rate where
TS loss > 5%) before starting latency tests. This means rate sweep
must complete before latency for each config.

**Estimated TS ceilings** (from prior data, approximate):

| vCPU | Est. TS ceiling | 25% | 50% | 75% | 100% | 150% |
|-----:|----------------:|----:|----:|----:|-----:|-----:|
| 2 | ~1.5G | 375M | 750M | 1.1G | 1.5G | 2.2G |
| 4 | ~2.5G | 625M | 1.2G | 1.9G | 2.5G | 3.8G |
| 8 | ~5G | 1.2G | 2.5G | 3.8G | 5G | 7.5G |
| 16 | ~8G | 2G | 4G | 6G | 8G | 12G |

Adjust once actual rate sweep data is in.

### Change 2: Capture raw latency samples

The bench harness already records raw latency_ns in the JSON.
Ensure `--raw-latency` or equivalent flag is set. Raw samples
allow post-hoc analysis:
- Distribution plots (histograms, CDFs)
- Bimodality detection
- Outlier identification (which run, which sample)
- Recomputation of any percentile

### Change 3: More runs for the "at TS ceiling" and "past TS ceiling" levels

These are the most interesting data points. Consider 15 runs
instead of 10 at these load levels. The low-load levels are
boring (both servers identical) — 10 is fine there.

| Load level | Runs |
|:-----------|-----:|
| idle | 10 |
| 25% | 10 |
| 50% | 10 |
| 75% | 10 |
| 100% | 15 |
| 150% | 15 |

### Latency reporting

Per data point:
- p50, p90, p95, p99, p999, max
- Mean, stddev
- 95% CI on p50 and p99 (bootstrap or t-distribution across runs)
- Flag any run with max > 10x the p999 of other runs (outlier stall)
- CDF plot per load level, HD vs TS overlaid

## Additional Data Collection

### Per-run (collected in parallel with each benchmark run)

| Metric | Tool | Why |
|--------|------|-----|
| Relay CPU% | `pidstat -p <pid> 1` | Per-run efficiency, not just aggregate |
| System CPU% | `mpstat 1` | Kernel/IRQ overhead context |
| HD worker stats | `curl /debug/workers` at start+end | Traffic distribution, queue depths |
| Memory RSS | `ps -o rss -p <pid>` at 1s intervals | Detect leaks, GC pressure |

### Per-config (collected once)

| Metric | Tool | Why |
|--------|------|-----|
| System info | `uname -a`, `lscpu`, `cat /proc/version` | Reproducibility |
| Network config | `ethtool -i eth0`, `sysctl net.core` | Document kernel tuning |
| HD version | Binary version or git commit hash | Reproducibility |
| TS version | `./derper --version` or `go version -m` | Confirm release build |
| Socket buffers | `sysctl net.core.rmem_max` etc. | Document defaults |

### Go-specific: GC trace (one run per config, optional)

```bash
GODEBUG=gctrace=1 ./derper ... 2>gc_trace.log
```

Run TS once at its ceiling rate with GC tracing. This gives:
- GC pause durations and frequency
- Heap size over time
- Evidence for whether GC is contributing to TS tail latency

Don't do this for every run — GC tracing has overhead. One run
per config at the TS ceiling rate is enough to tell the story.

### GOGC=off run (one per config, optional)

Run TS once with `GOGC=off` at its ceiling rate. If throughput
doesn't change, GC isn't the bottleneck. If it improves
significantly, that's a data point for the "Go overhead" argument.

## Comparison Protocol

For each vCPU config, strict alternation:

1. Boot relay VM at target size (or resize if supported)
2. Record system_info
3. Start HD
4. Run full rate sweep (HD)
5. Run full latency suite (HD)
6. Kill HD, `echo 3 > /proc/sys/vm/drop_caches`
7. Wait 10 seconds
8. Start TS derper
9. Run full rate sweep (TS)
10. Run full latency suite (TS)
11. Kill TS

**Do NOT interleave** (e.g., HD rate → TS rate → HD latency → TS
latency). Each server gets a clean, uninterrupted run. Interleaving
introduces ordering effects and makes the script more fragile.

## Data Organization

```
bench_results/gcp-c4-full-20260314/
  2vcpu/
    system_info.txt
    rate/
      hd_500_r{01..05}.json
      hd_1000_r{01..05}.json
      ...
      hd_5000_r{01..25}.json
      ...
      ts_500_r{01..05}.json
      ...
    latency/
      hd_idle_r{01..10}.json
      hd_ts25pct_r{01..10}.json
      hd_ts50pct_r{01..10}.json
      hd_ts75pct_r{01..10}.json
      hd_ts100pct_r{01..15}.json
      hd_ts150pct_r{01..15}.json
      ts_idle_r{01..10}.json
      ...
    cpu/
      hd_5000_r01_pidstat.csv
      hd_5000_r01_mpstat.csv
      ...
    gc_trace/
      ts_ceiling_gctrace.log
      ts_ceiling_gogcoff.json
    DONE
  4vcpu/
    ...
  8vcpu/
    ...
  16vcpu/
    ...
  gen_report.py
  FULL_SWEEP_DESIGN.md  (this file, copied here for provenance)
  plots/
  RESULTS.md
```

## Time Estimate

Per vCPU config, one server:

| Phase | Runs | Time/run | Subtotal |
|-------|-----:|--------:|---------:|
| Low rate sweep (4 rates × 5 runs) | 20 | ~20s | ~7 min |
| High rate sweep (5 rates × 25 runs) | 125 | ~20s | ~42 min |
| Latency (6 loads × 10-15 runs) | 70 | ~35s | ~41 min |
| **Per server** | | | **~90 min** |
| **Per config (HD + TS)** | | | **~3 hrs** |

| Config | Time |
|-------:|-----:|
| 16 vCPU | ~3.5 hrs (extra rate) |
| 8 vCPU | ~3 hrs |
| 4 vCPU | ~3 hrs |
| 2 vCPU (1 worker) | ~2.5 hrs (fewer rates) |
| 2 vCPU (2 workers, supplemental) | ~2.5 hrs |
| **Total** | **~14.5 hrs** |

This is an overnight run. Start in the evening, results by morning.

## Pre-flight Checklist

Before starting:

- [ ] Go derper: `go version -m ./derper` — no debug flags, clean version
- [ ] HD binary: built from correct commit with SPSC changes
- [ ] Confirm worker count matches table above for each VM size
- [ ] Test connectivity: bench client → relay VM, port open
- [ ] Verify isolation: no other user processes on relay VM
- [ ] Verify bench script handles VM resize (or have separate VMs ready)
- [ ] Dry run: 1 rate, 1 run, both servers, check JSON output is sane
- [ ] Disk space: ~50MB per config (25 runs × 4 rates × ~50KB JSON)
- [ ] Screen/tmux on client VM so disconnection doesn't kill the run

## Post-Run Validation

Before analyzing:

- [ ] Count files: expected vs actual per config
- [ ] Check for empty or truncated JSON files
- [ ] Verify timestamps are sequential (no clock jumps)
- [ ] Spot-check: low-rate throughput ≈ offered rate (sanity)
- [ ] Spot-check: HD and TS match at low rates (both lossless)
- [ ] Check CV at each rate — flag anything > 15%
- [ ] Check for bimodal distributions at high rates (histogram the raw throughputs)

## Resolved Questions

- **2 vCPU workers**: Primary run with 1 worker. Supplemental run
  with 2 workers (oversubscription case). Both get reported.

- **16 vCPU extended rates**: Add 25G to the 16 vCPU rate table.
  This won't be directly comparable to AWS (ENA caps lower), but
  it finds HD's ceiling on this hardware. Note the non-comparability
  in the report.

- **Socket buffer tuning**: Defaults only. Buffer tuning is a
  separate single-variable experiment, not part of the comparison.
