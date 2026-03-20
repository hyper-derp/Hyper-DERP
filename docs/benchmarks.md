# Benchmarks

## Methodology

All benchmarks follow a rigorous protocol designed to
produce reproducible, statistically valid results.

### Test Parameters

- **25 runs** per high-rate data point, 5 at low rates.
- **15-second test windows** per run.
- **95% confidence intervals** using t-distribution
  (appropriate for small sample sizes).
- **TS ceiling probes** -- Tailscale derper's maximum
  throughput is measured first, then latency tests are
  scaled to that ceiling per vCPU configuration.
- **Strict isolation** -- one server at a time, cache drops
  between runs, no co-located workloads.
- **Go derper baseline** -- v1.96.1 release build
  (`-trimpath`, stripped).

### Test Infrastructure

- **Scale test tool** -- `derp-scale-test` (built from
  `tools/bench/`). Generates configurable peer counts, message
  rates, and payload sizes against a DERP relay.
- **Tunnel tests** -- real Tailscale clients with
  self-hosted Headscale control plane. Zero contact with
  Tailscale coordination servers.
- **Platforms tested** -- GCP c4-highcpu (Intel Xeon
  Platinum 8581C), AWS c7i (Intel Xeon Platinum 8488C).

### What "Loss" Means

Loss percentages in the throughput table reflect messages
remaining in TCP send buffers when the 15-second test
window closes -- the gap between offered rate and relay
throughput. Hyper-DERP has zero internal packet drops
(no `xfer_drops`, `send_drops`, or `slab_exhausts`) at
all configurations. The reported loss is not relay packet
loss.

## Throughput (GCP c4-highcpu, kTLS)

HD kTLS vs TS TLS, measured at the offered rate shown.

| vCPU | HD Peak | TS Ceiling | Ratio | HD Loss | TS Loss |
|-----:|--------:|-----------:|------:|--------:|--------:|
| 2 | 2,977 Mbps | 1,448 Mbps | 11.8x at 5G | 2.8% | 93.0% |
| 4 | 5,106 Mbps | 2,395 Mbps | 3.7x at 7.5G | 0.8% | 74.2% |
| 8 | 7,366 Mbps | 3,802 Mbps | 1.9x at 10G | 0.4% | 46.1% |
| 16 | 12,068 Mbps | 7,743 Mbps | 1.6x at 20G | 1.7% | 18.9% |

The advantage grows as resources shrink. At 2 vCPU, TS
drops 93% of offered traffic at 5 Gbps while HD delivers
nearly 3 Gbps.

## Tail Latency (p99, at TS TLS ceiling)

Measured at the Tailscale derper's maximum sustainable
throughput per vCPU count.

| vCPU | HD p99 | TS p99 | Ratio |
|-----:|-------:|-------:|------:|
| 2 | 554 us | 2,718 us | 4.9x |
| 4 | 1,601 us | 2,512 us | 1.6x |
| 8 | 1,097 us | 1,408 us | 1.3x |
| 16 | 636 us | 1,268 us | 2.0x |

## Real Tailscale Tunnel Test

End-to-end through WireGuard tunnels with real Tailscale
clients and self-hosted Headscale control plane. 10 runs,
15 seconds each.

| Metric | Go derper | Hyper-DERP | Ratio |
|--------|----------:|-----------:|------:|
| Throughput (1 pair) | 988 Mbps | 1,682 Mbps | 1.70x |
| Retransmits (1 pair) | 9,184 | 560 | 16x fewer |
| Retransmits (3 pairs) | 9,566 | 399 | 24x fewer |
| Retransmits (5 min) | 192,689 | 3,956 | 48x fewer |
| Throughput under churn | 1,007 Mbps | 1,730 Mbps | 1.72x |

HD retransmits decrease under load. io_uring batched sends
coalesce into smoother TCP flow, producing fewer congestion
events. TS retransmits stay constant regardless of load.

## Cross-Cloud Consistency (AWS c7i)

| vCPU | GCP HD/TS | AWS HD/TS |
|-----:|----------:|----------:|
| 8 | 2.0x | 2.0-2.2x |
| 16 | 1.6x | 1.3x |

The advantage is architectural, not platform-specific.

## Why HD Is Faster

1. **io_uring vs goroutines.** Batched I/O submission
   amortizes syscall overhead. One `io_uring_enter` handles
   dozens of pending sends. Go derper makes one `write`
   syscall per packet per goroutine.

2. **kTLS vs userspace TLS.** AES-GCM encryption runs in
   the kernel. Worker threads never touch crypto. Go's
   `crypto/tls` encrypts in userspace per goroutine,
   competing for CPU.

3. **Sharded workers vs shared state.** Each worker owns a
   disjoint peer set. Cross-shard forwarding uses lock-free
   SPSC rings. Go derper's goroutines contend on shared
   mutex-protected maps.

4. **Send coalescing.** Batched io_uring submissions produce
   smoother TCP flow. Fewer congestion events means fewer
   retransmits. This is why retransmits decrease under load.

## Full Reports

Detailed reports with per-configuration breakdowns,
confidence intervals, and raw data:

- GCP Phase B: `bench_results/gcp-c4-phase-b/PHASE_B_REPORT.md`
- GCP Full Sweep: `bench_results/gcp-c4-full-sweep/REPORT.md`
- AWS Phase B: `bench_results/aws-c7i-phase-b/REPORT.md`

These reports and the underlying data (223 MB, 13,372
files) are preserved in the
[HD-bench-data](https://github.com/hyper-derp/HD-bench-data)
repository.

## Reproducing Benchmarks

### Build the Scale Test Tool

```sh
cmake --preset default
cmake --build build -j --target derp-scale-test
```

The `derp-scale-test` binary is built from sources in
`tools/bench/`. It generates configurable synthetic DERP traffic
against a relay server.

### Run a Basic Throughput Test

```sh
# Start the relay (on the server machine)
./build/hyper-derp --port 443 --workers 4 \
  --tls-cert cert.pem --tls-key key.pem

# Run scale test (on the client machine)
./build/derp-scale-test \
  --relay <server-ip>:443 \
  --peers 50 --rate 5000 --duration 15 --runs 25
```

### Run a Tunnel Test

Tunnel tests require:
- Two machines with Tailscale installed
- A Headscale control plane (or Tailscale account)
- Both machines using the relay under test as their DERP
  server

Use `iperf3` through the WireGuard tunnel to measure
end-to-end throughput and `ss -ti` to collect retransmit
counts.

### Test Automation

The `scripts/` directory contains automation for
multi-configuration benchmark sweeps, including:
- Server provisioning and configuration
- Automated run execution with statistical collection
- Result aggregation and reporting

See the scripts for environment variable configuration
(relay address, credentials, test parameters).
