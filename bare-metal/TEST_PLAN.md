# Bare Metal Test Plan

## Hardware

| Role | Machine | CPU | Cores | RAM | NIC | Port |
|------|---------|-----|------:|----:|-----|------|
| Relay | Dell T5810 (hd-test01) | Xeon E5-1650 v3 @ 3.5 GHz | 6C/12T | 32 GB | CX4 LX (ens4f0np0) | 25GbE |
| Client | Raptor Lake dev workstation (ksys) | 13th Gen Intel | 16C/24T | — | CX4 LX (enp3s0f0np0) | 25GbE |

## Network

| | Relay (hd-test01) | Client (ksys) |
|---|---|---|
| LAN IP | 192.168.0.134 | local |
| 25GbE IP | 10.50.0.1 | 10.50.0.2 |
| NIC interface | ens4f0np0 | enp3s0f0np0 |
| NIC PCI | 04:00.0 | 03:00.0 |
| NIC firmware | 14.31.1014 | 14.31.1014 |
| Kernel | 6.12.74+deb13+1-amd64 | 6.12.73+deb13-amd64 |

**DAC**: SFP28 25GbE passive copper, 1m (LO-SFP28-CBL-01)
**Topology**: direct card-to-card, no switch

## Access

| Machine | SSH | User | Sudo |
|---------|-----|------|------|
| hd-test01 | `ssh worker@hd-test01` | worker | NOPASSWD |
| hd-test01 | `ssh root@hd-test01` | root | — |
| ksys | local | karl | sudo |

Root password on hd-test01: `ggg`

## Baseline (validated)

| Metric | Value |
|--------|-------|
| iperf3 throughput | 23.6 Gbps (4 streams) |
| iperf3 retransmits | 0 (post-tuning) |
| Ping RTT | 0.15 ms avg |
| Link speed | 25,000 Mb/s both sides |

## System Tuning (applied, persistent via /etc/sysctl.d/)

Both machines have:
- sysctl: 16MB socket buffers, BBR congestion control, 250K
  backlog (/etc/sysctl.d/99-benchmark.conf)
- NIC: 8192 rx/tx ring buffers (ethtool -G)
- CPU: performance governor
- irqbalance: disabled
- BIOS (hd-test01): "Memory Mapped I/O above 4GB" enabled
  (required for CX4 LX BAR mapping on T5810)

## Prerequisites

```bash
# On relay (hd-test01):
modprobe tls
# Verify: lsmod | grep tls

# Deploy HD binary + Go derper
# Deploy certs for kTLS

# Verify perf works:
perf stat -e cycles,instructions ls
# If denied: echo 1 > /proc/sys/kernel/perf_event_paranoid
```

## Test 1: kTLS Throughput Sweep

Same methodology as GCP Phase B. HD kTLS vs TS TLS.

### Configuration

| Parameter | Value |
|-----------|-------|
| Workers | 2 and 4 (test both) |
| Peers | 20 |
| Active pairs | 10 |
| Duration | 15s |
| Payload | 1400B |
| Transport | kTLS (HD) / TLS (TS) |
| Runs (low) | 5 |
| Runs (high) | 25 |

### Rates

Start conservative, push to wire rate:

500M, 1G, 2G, 3G, 5G, 7.5G, 10G, 12.5G, 15G, 17.5G, 20G, 25G

The 25G rate is wire rate on a single port. If HD is still
climbing at 20G, 25G tells us if the NIC is the limit.

### Worker counts

6C/12T Haswell — test both:
- **2 workers**: 2 cores relay, 4 for kernel/accept/control/crypto
- **4 workers**: 4 cores relay, 2 for kernel/accept/control/crypto

Run 2w first. If 4w shows regression (too few cores for kernel
work at 25GbE), 2w is the answer.

### TS ceiling probe

3 runs at: 1G, 3G, 5G, 7.5G, 10G
Determine first rate where TS TLS loss > 5%.

### Latency

Same as Phase B: 6 load levels scaled to TS ceiling.
10 runs low loads, 15 runs at ceiling/above.

### Protocol

1. Deploy HD + TS to hd-test01
2. modprobe tls, verify
3. Record system_info
4. Start TS, run probe, rate sweep, latency suite
5. Kill TS, echo 3 > /proc/sys/vm/drop_caches
6. Start HD (2w), run rate sweep, latency suite
7. Kill HD, drop caches
8. Start HD (4w), run rate sweep, latency suite
9. Kill HD

## Test 2: perf Profiling

Run AFTER Test 1 with known-good configuration (best worker
count from Test 1).

### CPU profile at relay ceiling

```bash
# Record 10s under full load (run bench client in parallel)
perf record -g -p $(pgrep hyper-derp) -- sleep 10
perf report

# Stat counters
perf stat -e cycles,instructions,cache-misses,cache-references,\
L1-dcache-load-misses,LLC-load-misses,LLC-store-misses,\
branch-misses \
-p $(pgrep hyper-derp) -- sleep 10
```

### What to look for

- **IPC** (instructions per cycle): >1.5 is good, <1.0 means
  memory-stalled
- **Cache miss rate**: >2-3% means data structures don't fit
  L3, fix data layout
- **Where cycles go**: hash lookup? memcmp? send syscall?
  io_uring submission? This determines optimization targets.
- **Compare to TS**: same perf stat on Go derper at matched
  load for context

### P6: TCP segment analysis

```bash
# Capture during relay forwarding
tcpdump -i ens4f0np0 -w /tmp/relay.pcap -c 50000 port <derp_port>

# On relay:
ss -tin dst 10.50.0.2

# Check: segment sizes, pacing, cwnd
```

## Test 3: Plain TCP (optional, for comparison)

Quick run without kTLS at 2 rates (10G, 20G) with 2w and 4w.
Answers: "how much does kTLS cost on Haswell?" and "does the
backpressure collapse happen on bare metal?"

10 runs per rate. No latency suite needed.

## Data Organization

```
bench_results/bare-metal-haswell/
  2w_ktls/
    system_info.txt
    probe/
    rate/
    latency/
    workers/
    cpu/
    DONE
  4w_ktls/
    ...
  perf/
    hd_2w_15g.perf.data
    hd_2w_15g_stat.txt
    ts_15g_stat.txt
    relay_15g.pcap
  tcp_comparison/
    hd_2w_10g_r{01..10}.json
    hd_2w_20g_r{01..10}.json
    hd_4w_10g_r{01..10}.json
    hd_4w_20g_r{01..10}.json
  REPORT.md
  plots/
```

## Expected Results

Based on the math (6 Haswell cores at 3.5 GHz, ~65% IPC vs
Sapphire Rapids):

| Metric | Estimate | Basis |
|--------|---------|-------|
| HD kTLS peak (2w) | 8-12 Gbps | GCP 8v was 7.6G on 4w |
| HD kTLS peak (4w) | 10-15 Gbps | More workers but fewer kernel cores |
| TS TLS ceiling | 3-5 Gbps | GCP 8v was 4G |
| HD/TS ratio | 2-3x | Similar to GCP 8 vCPU |
| Idle RTT | ~30 us | Direct copper, no hypervisor |
| p99 at ceiling | 100-500 us | No hypervisor jitter |

The key question: **does bare metal eliminate the latency
outliers?** GCP showed 85ms max stalls from hypervisor
preemption. Bare metal should show max latency under 1ms.

If HD saturates 25GbE (even partially), that's the headline:
"2014 workstation pushes 15+ Gbps through a DERP relay."

## Time Estimate

| Test | Time |
|------|-----:|
| Test 1 (2w kTLS) | ~3 hrs |
| Test 1 (4w kTLS) | ~3 hrs |
| Test 2 (perf) | ~30 min |
| Test 3 (TCP, optional) | ~1 hr |
| **Total** | **~7.5 hrs** |
