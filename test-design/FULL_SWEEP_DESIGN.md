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

## TLS Strategy

### Hypothesis: kTLS will eliminate the high-worker-count regression

Earlier testing with the NetBird Go relay showed that TLS
backpressure eliminated loss on loopback. The mechanism is sound:
TLS encryption on the send path adds per-record overhead that
rate-limits output, preventing SPSC ring overflow. **However, this
has NOT been confirmed for HD's kTLS path yet.** The worker scaling
test must include kTLS runs on HD to verify.

Key evidence so far:
- Increasing SPSC ring capacity from 4096 to 16384 did NOT fix
  the plain TCP 8-worker collapse — 94.5% loss still observed at
  20G. This rules out ring overflow (H1) as the sole cause.
- The issue is N² cross-shard polling/signaling overhead that
  manifests when the send path is unconstrained (plain TCP).
- kTLS *should* throttle sends via per-record crypto overhead,
  preventing the cascade — but this needs confirmation on HD.

### Implications (pending kTLS confirmation)

- **If kTLS fixes HD's 8-worker collapse:** plain TCP benchmarks
  are a stress test, not representative of production. The 16 vCPU
  regression wouldn't exist in production. Phase B becomes the
  primary dataset for publication.
- **If kTLS does NOT fix it:** the N² overhead is real regardless
  of transport. Deployment guidance becomes "cap at 4-6 workers"
  and the architecture may need work for high worker counts.
- **TS will likely degrade more under TLS than HD.** Go's
  `crypto/tls` runs in userspace (goroutine per connection),
  burning CPU on AES-GCM. HD offloads to the kernel via kTLS —
  near-zero userspace cost. NetBird relay testing showed Go relays
  collapse much sooner with TLS enabled.

### Approach: Two-phase sweep

**Phase A (done):** Plain TCP sweep across all configs.
Established raw relay ceiling, exposed the cross-shard scaling
issue at high worker counts, and confirmed SPSC architecture
works correctly at 2-6 workers.

**Phase B (next):** kTLS sweep across all configs. Same
methodology as Phase A. Key questions:
- **Does kTLS fix HD's 8-worker collapse?** (must answer first)
- How much throughput does kTLS cost HD at each config?
- How much does TLS hurt TS? (expect significant degradation,
  especially at 2-4 vCPU)
- Does the HD/TS ratio increase with TLS? (expected: yes)
- Does HD's latency advantage hold or grow with crypto overhead?

If kTLS fixes the 8-worker issue, Phase B becomes the primary
dataset for publication. If not, the 2/4/8 vCPU plain TCP data
remains the lead story, with a note about 16 vCPU worker limits.

Phase B uses identical parameters (rates, runs, latency loads)
for direct comparison. The TS ceiling **will shift** with TLS —
rerun the probe phase to determine new latency load levels.

### Phase B additional considerations

- **Run a quick kTLS sanity check on 16 vCPU / 8 workers before
  the full sweep.** 5 runs at 15G and 20G. If 8w is still broken
  under kTLS, adjust the 16 vCPU config to 4 workers before
  committing to the full overnight run.
- TS ceiling will likely drop significantly with TLS. Adjust
  rate selection per config accordingly — high rates may need
  to come down.
- TS may not survive high rates at 2 vCPU with TLS at all.
  Be prepared for TS to become unmeasurable at offered rates
  above 1-2G.
- Report both Phase A and Phase B in final publication:
  Phase A shows the architectural ceiling, Phase B shows the
  production reality.

## Rate Sweep

### Parameters
- 20 peers, 10 active pairs
- 15 seconds per run
- 1400 byte payload (WireGuard MTU)
- **Phase A: DERP over plain TCP, no TLS**
- **Phase B: DERP over kTLS (follow-up run)**

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
- [ ] **Phase B only:** `modprobe tls` on relay VM, verify `lsmod | grep tls`,
      check HD startup log for `BIO_get_ktls_send` confirmation
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

- **TLS**: Two-phase approach. Phase A (this run) is plain TCP to
  establish raw relay ceiling and expose architectural issues.
  Phase B (follow-up) adds kTLS to get production-relevant numbers.
  See TLS Strategy section above.

## Results Informing Phase B

### Answered by worker scaling test

1. **Ring size fix (4096 → 16384):** Did NOT fix the plain TCP
   8-worker collapse. 94.5% loss still observed at 20G with 4x
   larger rings. Rules out ring overflow as sole cause. Keep
   larger rings anyway — no downside.

2. **Worker count cliff:** Degradation starts between 6 and 8
   workers on plain TCP. 2w and 4w are stable; 6w mostly stable;
   8w collapses.

### Open — must answer before full Phase B sweep

1. **Does kTLS fix HD's 8-worker collapse?** NetBird Go relay
   showed TLS backpressure helped on loopback, but HD has not
   been tested yet. Run a quick sanity check: 16 vCPU / 8w,
   kTLS, 5 runs at 15G and 20G. This determines whether 16 vCPU
   keeps 8 workers or drops to 4 for Phase B.
   **Pre-requisite:** `modprobe tls` on the relay VM before
   starting HD. Verify with `lsmod | grep tls`. Without this,
   OpenSSL falls back to userspace TLS and kTLS offload is
   silently disabled.

### Still required before Phase B

1. **Rerun TS ceiling probes with TLS.** TS performance will drop
   significantly — expect ceilings to shift down by 30-60%.
   Latency load levels must be recalculated from new TS ceilings.

2. **kTLS prerequisites.** Verify kTLS is active on the relay VM:
   check HD startup log for `BIO_get_ktls_send` confirmation.
   Verify kernel supports TLS offload: `modprobe tls` and check
   `/proc/net/tls_stat`. Generate or deploy test certificates.

3. **Decide rate ranges for Phase B.** If TS ceiling drops from
   ~5G to ~2G at 8 vCPU, testing up to 20G is wasteful. Adjust
   per config after probes complete.

4. **Verify bench client supports TLS connections.** The bench
   harness may need a flag or config change to connect over TLS
   instead of plain TCP.
