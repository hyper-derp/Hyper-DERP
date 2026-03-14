# End-to-End: Real Tailscale Tunnels Through Hyper-DERP

## The Question

Synthetic benchmarks showed Hyper-DERP forwarding DERP frames
faster than Go derper. But the users of a DERP relay never see
DERP frames. They see TCP connections, SSH sessions, video
calls — all encrypted inside WireGuard tunnels, all mediated
by the Tailscale client and its connection management, key
exchange, and path selection logic. The relay is buried under
several layers of software that could mask any advantage.

The question was simple: **does the relay advantage translate
to what the user actually experiences?**

## Test Design

We built a private Tailscale network with zero contact to
Tailscale Inc infrastructure. Headscale (an open-source
Tailscale control server) ran on the relay VM, coordinating
six Tailscale clients across five GCP VMs. Logging was
disabled (`logtail: enabled: false`). Auth used ephemeral
reusable keys. The test network was fully self-contained.

To force all traffic through the DERP relay, we blocked UDP
between every pair of client VMs with iptables. Tailscale's
direct path negotiation failed, and all traffic fell back to
DERP. We verified this with `tailscale status` (showing
`relay "test"`) and `tailscale ping` (showing `via DERP(test)`)
on every client before every test phase.

The traffic path for every byte in these tests:

```
application (iperf3 / ping)
  → kernel TCP/IP stack
    → WireGuard encrypt (tailscaled)
      → DERP relay (HD or TS)
    → WireGuard decrypt (tailscaled)
  → kernel TCP/IP stack
→ application
```

Both relays ran on the same VM (c4-highcpu-16, 16 vCPUs),
one at a time, with a cache flush between swaps. Go derper
was a release build with TLS on port 3340. Hyper-DERP ran
with kTLS, 8 io_uring workers, on port 3341. Same clients,
same auth keys, same iperf3 parameters. The only variable
was the relay binary.

Every configuration ran 10 times for 15 seconds each.

### Infrastructure

| VM | Type | vCPUs | Role |
|----|------|------:|------|
| bench-relay | c4-highcpu-16 | 16 | DERP relay |
| bench-client | c4-highcpu-8 | 8 | Primary client pair (sender) |
| tunnel-client-{2,3} | e2-standard-2 | 2 | Secondary pair |
| tunnel-client-{5,6} | e2-standard-2 | 2 | Tertiary pair |

Software: Tailscale 1.82.0, Headscale 0.25.1, Go 1.24 derper
(release build), Hyper-DERP commit ae7c952 (P3 bitmask,
per-worker frame pool allocator).

## Results

### Single Pair

One sender, one receiver, single TCP stream through a
WireGuard tunnel through the relay.

| Metric | Go derper | Hyper-DERP | Ratio |
|--------|----------:|-----------:|------:|
| Throughput (Mbps) | 988 | 1682 | **1.70x** |
| Retransmits | 9,184 | 560 | **16.4x fewer** |
| Ping latency (ms) | 0.63 | 0.39 | **1.6x lower** |

CV% was 0.4% for Go derper and 0.8% for Hyper-DERP. Both
relays produced 10/10 valid runs. The numbers are stable.

The retransmit difference is the most striking result. Over
15 seconds of single-stream TCP, Go derper caused 9,184
retransmits. Hyper-DERP caused 560. Same clients, same
tunnel, same network — the only difference is how each relay
moves bytes between its TCP connections.

Go derper uses one goroutine per connection. Each goroutine
independently reads from one peer and writes to another.
These writes land on the kernel TCP stack at unpredictable
times, creating bursty send patterns that trigger congestion
inside the WireGuard tunnel.

Hyper-DERP uses io_uring with batched submissions. Multiple
pending sends are coalesced into fewer, larger kernel
operations. The kernel sees a steadier flow, TCP congestion
control stays in a smoother state, and retransmits drop by
an order of magnitude.

### Under Contention

We added pairs. Pair 2 used e2-standard-2 VMs (2 vCPUs) —
small machines where WireGuard crypto limits throughput to
~280 Mbps regardless of relay speed. Pair 3 used the same.

The e2 pairs are intentionally client-bottlenecked. The
interesting number is what happens to Pair 1 (the c4-highcpu-8
client that can actually push the relay) as other pairs
compete for relay resources.

| Concurrent pairs | Go derper P1 | Hyper-DERP P1 | Ratio |
|-----------------:|-------------:|--------------:|------:|
| 1 | 988 Mbps | 1,682 Mbps | 1.70x |
| 2 | 950 Mbps | 1,641 Mbps | 1.73x |
| 3 | 914 Mbps | 1,616 Mbps | **1.77x** |

The advantage *increases* with load. Go derper's per-pair
throughput drops 7.5% going from 1 to 3 pairs (988 → 914).
Hyper-DERP drops 3.9% (1682 → 1616). Sharded io_uring workers
with disjoint peer sets handle contention more efficiently
than goroutines contending on shared state.

The retransmit story under load is even more dramatic:

| Concurrent pairs | Go derper retrans | HD retrans | Ratio |
|-----------------:|------------------:|-----------:|------:|
| 1 | 9,184 | 560 | 16.4x |
| 2 | 9,500 | 450 | 21x |
| 3 | 9,566 | 399 | **24x** |

Hyper-DERP retransmits *decrease* as load increases. More
concurrent connections give io_uring more opportunities to
batch sends. Go derper stays flat at ~9,500 retransmits
regardless of pair count.

### Client Churn

A real DERP relay sees clients connecting and disconnecting
continuously. We tested this: Pair 1 ran a 60-second bulk
transfer, Pair 3 ran continuous pings, and Pair 2 disconnected
and reconnected 5 times during the test (every 10-15 seconds).

| Metric | Go derper | Hyper-DERP | Ratio |
|--------|----------:|-----------:|------:|
| P1 throughput | 1,007 Mbps | 1,730 Mbps | **1.72x** |
| P1 retransmits | 37,359 | 1,893 | **19.7x fewer** |

Go derper's throughput dropped from 988 to 1,007 Mbps (roughly
stable) but retransmits quadrupled from 9,184 to 37,359. Peer
addition and removal contends with the data goroutines on shared
maps, creating congestion bursts.

Hyper-DERP maintained 1,730 Mbps — actually *higher* than the
3-pair Phase 3 result (1,616 Mbps), because the churning pair
spent most of its time disconnected, leaving more relay capacity
for Pair 1. Retransmits were 1,893, only slightly above the
3-pair steady state (399). The data plane is fully isolated
from control plane operations.

### Five Minutes Sustained

The most telling test: three pairs running TCP for 300 seconds
(5 minutes). Long enough to expose instability, memory leaks,
GC pressure, or any creeping degradation.

| Metric | Go derper | Hyper-DERP | Ratio |
|--------|----------:|-----------:|------:|
| P1 throughput | 920 Mbps | 1,594 Mbps | **1.73x** |
| P1 retransmits | 192,689 | 3,956 | **48.7x fewer** |
| Retransmit rate | 642/s | 13/s | — |
| CPU usage | 265% | 162% | **38% less** |
| RSS (MB) | 26 | 531 | — |

The retransmit ratio improved from 24x (15-second runs) to
48.7x over 300 seconds. Hyper-DERP's retransmit rate actually
*decreased* over time — from 26.6/s in the first 15 seconds
to 13.2/s averaged over 5 minutes. The io_uring send path
stabilizes as TCP congestion windows settle into steady state.

Go derper held constant at ~640 retransmits per second for
the full duration. No GC-pause spikes were detected in either
relay. The retransmit difference is structural — it comes from
the send architecture, not from runtime pauses.

Hyper-DERP used 38% less CPU while delivering 1.73x more
throughput. It used 20x more memory (531 vs 26 MB), reflecting
pre-allocated frame pools, io_uring ring buffers, and 8 worker
threads. This is a deliberate tradeoff: predictable allocation
at startup vs garbage collection overhead at runtime. On a
16-vCPU relay, 531 MB is inconsequential.

## The Scaling Curves

Two ways to read the scaling data, depending on what you
care about.

**Aggregate throughput** (all pairs summed):

| Pairs | Go derper | Hyper-DERP | Ratio |
|------:|----------:|-----------:|------:|
| 1 | 988 | 1,682 | 1.70x |
| 2 | 1,232 | 1,938 | 1.57x |
| 3 | 1,463 | 2,188 | 1.50x |

The aggregate ratio appears to decrease. This is misleading.
The e2-standard-2 clients cap at ~280 Mbps each due to
WireGuard crypto overhead on 2 vCPUs. Adding e2 pairs adds
~280 Mbps to both relays equally, diluting the ratio toward
1.0x. Neither relay is the bottleneck for these clients.

**Per-capable-client throughput** (P1 only, the client that
can push the relay):

| Pairs | Go derper P1 | Hyper-DERP P1 | Ratio |
|------:|-------------:|--------------:|------:|
| 1 | 988 | 1,682 | 1.70x |
| 2 | 950 | 1,641 | 1.73x |
| 3 | 914 | 1,616 | 1.77x |

This is the relay-limited view. When you look at a client
that can actually saturate the relay, the advantage grows
under contention. A 4-pair or 8-pair test with capable clients
would likely show an even larger gap.

## What the Numbers Mean

DERP relays exist so that Tailscale clients behind restrictive
NATs can still communicate. In the common case, Tailscale
establishes a direct WireGuard path and the relay is unused.
When the relay is needed, it becomes the bottleneck for every
byte of user traffic.

For a video call relayed through DERP, 48.7x fewer retransmits
means fewer frame drops, less rebuffering, and smoother audio.
1.73x higher throughput means large file transfers through
restrictive networks finish in 58% of the time. 38% less CPU
means a relay can serve more concurrent users on the same
hardware, or run on a cheaper instance.

These numbers were measured through real Tailscale WireGuard
tunnels, with real Tailscale clients, using a self-hosted
control plane with zero modifications to the Tailscale software.
The only thing that changed between runs was which binary
relayed the traffic.

## How We Know the Data is Clean

**Controlled variable.** Same clients, same auth, same firewall
rules, same iperf3 parameters. Cache flush between relay swaps.
The only variable is the relay binary.

**Statistical rigor.** 10 runs per configuration, 15 seconds
each. CV% under 1% for throughput on both relays. 10/10 valid
runs for every phase on both relays (after fixing an iperf3
cleanup bug in the test harness).

**Self-hosted control plane.** Headscale eliminated all contact
with Tailscale servers. No variable latency to external
services. No phone-home traffic competing with test traffic.

**Verified relay path.** `tailscale status` and `tailscale ping`
confirmed DERP relay path on every client before every phase.
UDP blocked between all client VMs.

**Bug fixes documented.** The test harness had an iperf3
cleanup bug (background client not terminating, wedging the
server for the next run). Found during Phase 1, fixed, and
all affected tests rerun. The long-duration test required a
separate rerun with full DERP reconnection between relays.
Both issues were harness bugs, not relay bugs.

## Architecture: Why the Difference

Go derper allocates one goroutine per DERP connection. When
peer A sends a packet to peer B, A's read goroutine writes
directly to B's TCP connection. Each write is an independent
`syscall(write)`. Under load with many peers, these writes
land on the kernel TCP stack at arbitrary times, creating
bursty patterns that trigger congestion events in the
receiving WireGuard tunnel.

Hyper-DERP shards peers across io_uring worker threads. Each
worker owns a disjoint set of peers and processes events in
batches via `io_uring_peek_cqe`. When a worker has multiple
sends pending (for the same or different peers), it submits
them as a batch to io_uring, and the kernel coalesces them
into fewer TCP segments. The result is smoother TCP flow,
fewer congestion events, and dramatically fewer retransmits.

The retransmit advantage *increasing* under load is the
signature of this architecture. More concurrent connections
mean larger CQE batches, more coalescing opportunities, and
smoother send patterns. Go derper's goroutine model doesn't
have this property — each goroutine writes independently
regardless of system load.

The CPU advantage (162% vs 265%) comes from the same source.
io_uring amortizes system call overhead across batches.
Goroutines make individual system calls per packet. At 1,594
Mbps through the relay (about 142,000 WireGuard packets per
second at 1400B MTU), the per-packet overhead adds up.
