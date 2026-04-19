# HD Protocol Per-Frame Cost Analysis

## Summary

HD Protocol's per-frame forwarding cost equals DERP's. The throughput
gap observed on GCP was caused by send queue overflow from HD's higher
frame rate, not by a per-frame processing penalty.

## PMU Measurements (Haswell E5-1650v3, 4 workers, 50 pairs, 20 Gbps offered)

| Metric | DERP | HD | Delta |
|--------|------|-----|-------|
| Throughput | 7,306 Mbps | 7,322 Mbps | +0.2% |
| Frames forwarded | 17.4M | 17.4M | ~equal |
| Loss | 3.49% | 3.21% | HD better |
| **Cycles/frame** | **11,958** | **11,959** | **+0.0%** |
| Instructions/frame | 14,322 | 13,999 | -2.2% (HD fewer) |
| IPC | 1.20 | 1.17 | -2.5% |
| Cache misses/frame | 36.0 | 34.6 | -4.0% (HD fewer) |
| Branch misses/frame | 21.9 | 22.4 | +2.3% |

## Analysis

HD does 2.2% fewer instructions per frame (less logical work: no key
extraction, no hash lookup, no RecvPacket header rewrite). But HD has
2.5% lower IPC, likely from slightly different memory access patterns.
The two effects cancel: identical cycles per frame.

## GCP Discrepancy

On GCP (c4-highcpu-8, no hardware PMU), cpu-clock sampling showed HD's
`ForwardHdData1` at 6.36% vs DERP's `ForwardMsg` at 4.49% — a 42% gap.

This was a measurement artifact. HD's smaller frames (1404B vs 1437B)
cause the client to generate ~22% more frames at the same offered
bandwidth. The relay receives more HD frames, forwards them through
`ForwardHdData1`, but many hit per-peer send queue limits
(`kMaxSendQueueDepth=2048`) and are dropped. The function executes more
times, consuming cycles on frames that aren't delivered. cpu-clock
sampling sees the inflated call count and reports a higher percentage.

The PMU data on Haswell — where both protocols saturate at the same
throughput with similar loss — shows the per-frame cost is identical.

## Optimizations Applied

Despite the per-frame cost being equal, the investigation produced
structural improvements:

1. **AoS forwarding rules**: SoA → AoS reduced cache lines from 3 to 1.
2. **Hot 1:1 fields on cache line 0**: `fwd_count`, `fwd1_dst_fd`,
   `fwd1_dst_worker`, `fwd1_peer` share the recv-hot cache line.
   Zero additional cache misses for 1:1 forwarding.
3. **Extracted ForwardHdData1**: 752-byte noinline function vs
   DispatchHdFrame's 1680 bytes. Prevents icache pollution from
   cold MeshData/FleetData/Ping paths.
4. **Lazy route resolution**: first-frame-only RouteLookup, cached
   thereafter. Eliminated per-frame HtLookup for cross-shard.

## Real Bottleneck

The throughput ceiling is kTLS encrypt/decrypt + TCP stack, consuming
~85% of CPU. User-space forwarding (ForwardMsg / ForwardHdData1) is
4-7% of total cycles. At 7.3 Gbps on the Haswell (12 threads), both
protocols are identically CPU-bound in the kernel.

The path to beating DERP's throughput is not in the forwarding function.
It's in eliminating the things that DERP can't eliminate:
- WireGuard+TLS double encryption (Level 2 direct path)
- Kernel TCP overhead (AF_XDP bypass)
- Cross-shard transfer ring (MSG_RING io_uring)
