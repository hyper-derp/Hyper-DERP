---
title: >-
  Hyper-DERP: C++/io_uring DERP relay -- 2-10x throughput,
  40% lower tail latency than Tailscale's derper
date: 2026-04-04
author: "Karl"
tags: ["announcement", "release", "benchmarks"]
description: >-
  Announcing Hyper-DERP: a high-performance DERP relay
  server written in C++23 with io_uring. Full benchmark
  comparison against Tailscale's derper across 4,903 runs.
---

## The Interview

I work for a company that produces IR cameras for
industrial applications. I created a Raspi edge device
with the accompanying software. Among many things it will
be able to forward data and control streams from one
industrial net into another. I had some rough ideas how I
wanted the relay to work but hadn't gotten serious about
it.

Then I had an interview at NetBird, a VPN startup in
Berlin that had just gotten Series A funding. In
preparation for the interview I looked over their code,
got to their relay, and no further.

The relay was written in Go with userspace TLS. Every
packet makes its way into the userspace, gets decrypted,
has its header rewritten and encrypted again before being
sent back out. The whole time fighting the Go runtime -
goroutine scheduling, garbage collection and context
switches.

So naturally I did what every reasonable person would do:
rip out the data plane and replace it with C.

I started with decoupling the NetBird relay data plane.
Which worked out fine, I ran benchmarks on the loopback
and I soon realized that NetBird's relay isn't much of a
benchmark. It's a startup prototype - not serious systems
engineering. Outrunning it was hardly sport.

Then I had a look at Tailscale's `derper`. Built by a
proper engineering team, years of production hardening,
real effort and thought behind it -- i had found a worthy
opponent.

## What is DERP?

DERP (Distributed Encrypted Relay Protocol) is Tailscale's
fallback when peer-to-peer WireGuard connections cannot be
negotiated. So if you are behind a symmetric NAT, a
restrictive firewall or CGNAT this will become your
permanent networking route.

It works like this: Both peers connect to the relay on
port 443. Peer A sends a `SendPacket` tagged with B's
public key. The relay rewrites the header to `RecvPacket`,
puts in A's public key and sends the packet on its way to
B. Every packet gets decrypted, rewritten and encrypted in
userspace.

## Making It Go Brrrr

I use OpenSSL in userspace to do the TLS handshake then
install the key in kernel TLS and promptly forget about
it. The kernel will not give the key back and we only
handle the key for a few microseconds in userspace,
instead of the entire connection life cycle where derper
holds it. From here on out the kernel handles all
decryption/encryption and we will only have to deal with a
plain socket. Which also turns out to be great because you
can offload the TLS onto a smart NIC if you so choose,
going from fast to stupid fast.

If you go with `epoll` you will do the following for each
arriving packet: wait for socket readiness, `read()`,
rewrite the peer ID and `write()`. Two syscalls per
packet, at scale this is millions of kernel transitions
per second. Each one flushing the pipeline and trashing
the cache.

io_uring inverts this. Instead of asking the kernel 'Is
this socket ready?' we tell the kernel 'I need these 50
reads and 30 writes done' and after we harvest the
results. One syscall does what `epoll` needs hundreds for.
Drain completions, rewrite headers, enqueue sends and
submit - one pass.

From here the rest of the architecture just flows. Having
pinned one io_uring per core gives us a very clean
separation. No shared state, no locks, pure speed.

I give each shard a list of client public key hashes. Same
shard forwarding does a hash table lookup and directly
enqueues; cross-shard uses a lock-free SPSC ring between
the worker pairs. All sends are deferred and flushed with
one `io_uring_submit()` per batch. Memory comes from slab
allocators and frame pools - no malloc in the hot path.

Ending up with a shard-per-core, share-nothing design.
This is basically what Seastar (ScyllaDB's framework)
does, if you go down the path of removing all possible
context switches from the forwarding path this is where
you end up.

## Benchmarks

4,903 benchmark runs across three test suites on GCP
c4-highcpu VMs (Intel Xeon Platinum 8581C). 4 client VMs
(c4-highcpu-8), 20 peers, 10 sender/receiver pairs,
~1400-byte messages at WireGuard MTU, token-bucket pacing.
20 runs per data point, 95% confidence intervals (Welch's
t). Go derper v1.96.4, go1.26.1, release build.

Getting this benchmark suite right took three rounds. The
first used a single client VM -- which turned out to be
measuring the client's CPU limits, not the relay's. The
8 vCPU result improved 62% when we switched to 4 clients.
The "latency" suite measured throughput, not latency. The
tunnel suite's SSH automation was broken on GCP's Debian
VMs. Each failure taught something about what makes a
relay benchmark credible. The full history of what failed
and why is in [BENCHMARK_HISTORY.md](https://github.com/hyper-derp/HD.Benchmark/blob/master/docs/BENCHMARK_HISTORY.md).

## Throughput

Go's network model is built on `epoll`. Each goroutine
parks itself on a socket and waits for the kernel to
signal readiness. The goroutine wakes up, calls `read()`,
copies the packet into userspace, rewrites the header,
calls `write()` to send it back out, and parks again.
That's two syscalls, two kernel transitions, one packet.

And in between those calls the goroutine sits in the
scheduler's run queue doing nothing -- but it still has a
stack, it still needs to be scheduled, and when it wakes
back up it might be on a different core with cold caches.
Multiply by thousands of connections and the scheduler
spends most of its time juggling goroutines that are
waiting for I/O, not doing work.

| Config | HD Peak (Mbps) | +/-CI | HD Loss | TS Ceiling (Mbps) | TS Loss @ HD Peak | HD/TS |
|--------|---------------:|------:|--------:|------------------:|------------------:|------:|
| 2 vCPU (1w) | 3,730 | 77 | 1.65% | 1,870 | 92% | **10.8x** |
| 4 vCPU (2w) | 6,091 | 79 | 1.97% | 2,798 | 74% | **3.5x** |
| 8 vCPU (4w) | 12,316 | 247 | 0.68% | 4,670 | 44% | **2.7x** |
| 16 vCPU (8w) | 16,545 | 746 | 1.51% | 7,834 | 17% | **2.1x** |

Once you starve TS of vCPUs the picture becomes
devastating. TS needs 16 vCPUs to deliver 7,834 Mbps. HD
delivers more than that on 8 (12,316 +/- 247 Mbps). Half
the cores, more throughput.

At 2 vCPU the ratio gets extreme. HD peaks at
3,730 +/- 77 Mbps with one worker. TS delivers 1,870 Mbps
at 3 Gbps offered, but push to 5 Gbps and it collapses to
324 Mbps with 92% loss -- the Go runtime has consumed the
entire CPU budget.

### The Cost Story

HD on a smaller VM matches or exceeds TS on a larger one:

| TS deployment | TS throughput | HD equivalent | HD throughput | VM savings |
|---------------|-------------:|---------------|-------------:|-----------:|
| TS on 16 vCPU | 7,834 Mbps | HD on 8 vCPU | 8,371 +/- 162 Mbps | **2x** |
| TS on 8 vCPU | 4,670 Mbps | HD on 4 vCPU | 5,457 +/- 114 Mbps | **2x** |
| TS on 4 vCPU | 2,798 Mbps | HD on 2 vCPU | 3,536 +/- 63 Mbps | **2x** |

Across the board, HD delivers the same throughput on half
the vCPUs -- 50% compute cost reduction for a relay fleet.

## Packet Loss

Throughput is just a part of the whole. You need to
examine what happens to the packets. With a 16 vCPU setup
derper will *only* lose about 17% of your packets at
25 Gbps offered.

Once you starve derper of vCPUs this issue compounds, with
the Go runtime taking up more and more of the CPU budget,
derper will become a packet shredder. With 8 vCPUs the
loss becomes 44% at 20 Gbps offered, 4 vCPU 74% at 10 Gbps
and with only 2 vCPUs the loss is at 92% at 5 Gbps.

The story of packet loss is different for HD. When HD's
send queues fill up it pauses recv, at high rates the
kernel advertises a smaller window back to the sender.
Eventually the window hits zero.

The senders TCP stack respects this pause. But there is a
timing gap -- all packets that are in flight when the
window hits zero get dropped by the kernel. In the tested
ranges this stays below 2% loss.

## Latency Under Load

At idle both relays return pings in about 110-115 us on
GCP -- the kernel TCP stack dominates and neither relay
adds anything you'd notice. The median stays close even
under load. The story is in the tail.

480 runs, 2,160,000 total latency samples. Per-packet DERP
relay RTT via ping/echo, 5,000 pings per run, 10 runs per
load level. Background load from dedicated client VMs.

### 8 vCPU -- HD flat, TS degrades

| Load | HD p50 | HD p99 | HD p999 | TS p50 | TS p99 | TS p999 |
|------|-------:|-------:|--------:|-------:|-------:|--------:|
| Idle | 114 us | 129 us | 143 us | 112 us | 129 us | 162 us |
| 25% | 115 us | 138 us | 158 us | 117 us | 148 us | 233 us |
| 50% | 122 us | 149 us | 172 us | 119 us | 157 us | 251 us |
| 75% | 124 us | 152 us | 171 us | 119 us | 163 us | 252 us |
| 100% | 121 us | 147 us | 169 us | 121 us | 185 us | 272 us |
| 150% | 121 us | 153 us | 184 us | 124 us | 218 us | 289 us |

HD p99 is load-invariant: 129-153 us from idle through
150%. TS p99 rises from 129 to 218 us (+69%). At 150%
load, HD is 1.42x better on p99 and 1.57x better on p999.

### 16 vCPU -- HD dominates

| Load | HD p50 | HD p99 | HD p999 | TS p50 | TS p99 | TS p999 |
|------|-------:|-------:|--------:|-------:|-------:|--------:|
| Idle | 106 us | 119 us | 133 us | 104 us | 117 us | 145 us |
| 50% | 110 us | 127 us | 140 us | 105 us | 138 us | 258 us |
| 100% | 109 us | 130 us | 144 us | 107 us | 190 us | 275 us |
| 150% | 105 us | 127 us | 141 us | 109 us | 214 us | 286 us |

At 150% load: HD p99 = 127 us, TS p99 = 214 us. HD is
1.69x better on p99, 2.03x better on p999. HD's latency
actually decreases slightly at 150% -- the io_uring
busy-spin loop is always active, reducing syscall
overhead. TS degrades monotonically.

That's the Go scheduler fighting relay traffic for CPU
time -- goroutines servicing connections get preempted by
goroutines handling the background load, and the unlucky
ones wait.

## Peer Scaling

Twenty peers with ten pairs is a clean benchmark, but a
production relay might have hundreds of peers. So I tested
20 through 100 peers at 8 vCPU with 10 Gbps offered.

| Peers | HD (Mbps) | HD Loss | TS (Mbps) | TS Loss | HD/TS |
|------:|----------:|--------:|----------:|--------:|------:|
| 20 | 8,371 | 0.1% | 4,495 | 44% | 1.9x |
| 40 | 8,006 | 0.5% | 3,538 | 57% | 2.3x |
| 60 | 6,880 | 0.7% | 3,146 | 63% | 2.2x |
| 80 | 7,827 | 0.4% | 2,905 | 66% | 2.7x |
| 100 | 7,665 | 0.5% | 2,775 | 68% | **2.8x** |

TS loses 38% throughput and gains 24 percentage points of
loss going from 20 to 100 peers. HD stays flat. The ratio
amplifies from 1.9x to 2.8x.

TS creates 2 goroutines per peer. At 100 peers that's 200
goroutines competing for CPU, plus scheduling overhead and
cache pressure from goroutine stack switches. HD's sharded
hash table is O(1) per peer regardless of count.

## Through the Tunnel

Everything above is synthetic -- a custom bench tool
pushing DERP frames at controlled rates. The question is
whether any of it survives contact with a real WireGuard
tunnel.

I set up Headscale with 4 Tailscale clients on GCP,
blocked direct UDP between them to force all traffic
through the DERP relay, and ran iperf3 UDP + TCP + ICMP
ping concurrently. 20 runs per data point, 720 total runs.

| Config | HD UDP @ 8G | TS UDP @ 8G | HD TCP Retx | TS TCP Retx | HD Ping | TS Ping |
|--------|------------:|------------:|------------:|------------:|--------:|--------:|
| 4 vCPU | 2,100 Mbps | 2,115 Mbps | 4,852 | 5,217 | 0.90 ms | 0.55 ms |
| 8 vCPU | 2,053 Mbps | 2,060 Mbps | 4,552 | 4,484 | 0.98 ms | 0.90 ms |
| 16 vCPU | 2,059 Mbps | 2,223 Mbps | 4,291 | 4,617 | 0.91 ms | 1.19 ms |

Both relays deliver identical UDP throughput (~2 Gbps).
The relay isn't the bottleneck -- Tailscale's userspace
WireGuard (wireguard-go, ChaCha20-Poly1305) is. Loss is
negligible for both (<0.04%). TCP retransmits: HD produces
7-8% fewer at max load on 4 and 16 vCPU. Tied at 8 vCPU.
Tunnel latency: both ~0.5-1.0 ms, dominated by WireGuard
crypto and network RTT.

Switching from TS to HD as the relay is transparent to
applications running through WireGuard tunnels. The
performance advantages measured in the relay benchmarks
represent additional headroom -- capacity to serve more
tunnels, more peers, or handle traffic bursts without
dropping packets.

## The kTLS Cache Cliff

The most interesting finding wasn't a win -- it was a
discontinuity. kTLS adds a roughly linear crypto tax at
moderate load. Push harder and it stays linear. Then at
saturation something breaks: LLC miss rate jumps from 2.6%
to 40% in a single step. The AES-GCM working set -- cipher
state, IV buffers, scratch space for every active
connection -- overwhelms L3 and starts evicting everything
else. Throughput doesn't degrade gradually. It hits a wall.

This was measured on bare metal (Haswell Xeon E5-1650 v3,
15 MB L3, ConnectX-4 Lx 25GbE). At 2 workers, kTLS costs
48% of throughput compared to plain TCP (3,833 vs 7,383
Mbps). At 4 workers, the tax drops to 27% (6,300 vs 8,652
Mbps) because the crypto work spreads across more cores.

This is also why HD's variance is higher under kTLS (CV
6-10% vs TS's rock-solid <1%). The system oscillates
around the cliff edge -- crypto pressure pushes the
working set out of cache, throughput drops, fewer packets
means less crypto pressure, the working set fits again,
throughput recovers, and the cycle repeats. TS never sees
this because its crypto runs in userspace with a more
predictable memory footprint.

One data point tells the whole story: HD's user-space
relay code (ForwardMsg -- frame parsing, hash lookup, SPSC
enqueue, frame construction) consumes 2% of CPU cycles.
The kernel TLS stack consumes 25%. User-code optimization
is closed. The next win is NIC TLS offload.

## Where HD Loses

HD doesn't win everywhere, and the losses are instructive.

**Idle latency.** At idle, TS is slightly better. At 2
vCPU: TS p99 = 128 us vs HD p99 = 143 us. At 8 vCPU both
match at 129 us. If latency is your only metric, idle
traffic won't show a difference.

**4 vCPU backpressure stall.** HD at 4 vCPU (2 workers)
has intermittent multi-millisecond latency stalls at
>=50% load. Three consecutive runs at 100% load hit p99 of
593, 2,579, and 3,923 us. The backpressure mechanism
oscillates -- when the send queue fills, recv pauses; the
queue drains, recv resumes, a burst floods in, the queue
fills again. The cycle is ~42ms with 2 workers, fast
enough to trap ping packets in the kernel TCP buffer for
tens of milliseconds. TS at 4 vCPU has steady ~172 us p99
(aside from one 12ms GC pause at 50% load).

The fix is three parts: wider hysteresis for low worker
counts (resume at 1/8 instead of 1/4), minimum pause
duration (8 CQE batch iterations before checking the low
threshold), and reduced busy-spin for 2-worker configs
(64 vs 256 iterations, giving kTLS more CPU time).
Verification data shows p99 dropping from 825 us to 151 us
at 100% load.

**Tunnel throughput parity.** Both relays deliver identical
~2 Gbps through WireGuard tunnels. The relay benchmarks
show 2-10x advantages, but real applications running
through Tailscale won't see it until the WireGuard crypto
ceiling moves. With kernel WireGuard (wg.ko) replacing
wireguard-go, the tunnel ceiling would move from ~2 Gbps
to 10+ Gbps, and HD's relay advantage would become
directly visible.

**2 vCPU bare metal.** On Haswell with only 2 kTLS workers,
TS wins on throughput: 4,100 Mbps vs HD's 3,833 Mbps. Two
cores aren't enough for both relay work and AES-GCM --
the 48% kTLS overhead eats the architectural advantage.
TS spreads work across all 6C/12T via goroutines. Four
workers fix it (HD 6,680 Mbps).

## What's Next

**Kernel WireGuard client.** The tunnel benchmark proved
the relay isn't the bottleneck -- wireguard-go is. A
kernel WireGuard integration (wg.ko) would move the
tunnel ceiling from ~2 Gbps to 10+ Gbps, where HD's
relay advantage becomes directly visible to applications.

**Custom protocol.** The whole reason I built a relay was
to stream IR camera feeds between industrial networks.
Right now HD speaks DERP and nothing else. A client SDK
that lets you relay arbitrary protocols through it would
turn HD from a Tailscale component into a general-purpose
secure relay -- which is what I actually need.

**NIC TLS offload.** The bare metal profiling proved that
kTLS consumes 25% of CPU cycles and costs 27-48% of
throughput. ConnectX-5/6 with hardware kTLS offload would
eliminate the crypto tax entirely, including the cache
cliff and kTLS-driven variance. For the next hardware
purchase: TLS offload capability over raw line rate.

**eBPF steering.** Currently, all connections land on the
accept thread and get assigned to workers via FNV-1a hash.
An eBPF XDP program could steer incoming connections
directly to the correct worker's io_uring, eliminating
the accept thread as a serialization point.

## Closing Thoughts

Tailscale made a reasonable bet, and honestly a good one:
Go gives them memory safety, trivial cross-compilation,
goroutine concurrency that makes the control path simple,
and a hiring pool of engineers that can contribute on day
one. For 95% of what Tailscale does, that's absolutely the
right call -- and that 95% is why millions of people have a
VPN that just works.

The relay happens to be the unlucky 5% where those
tradeoffs get punished -- a hot data plane that touches
every byte, where the Go runtime's scheduling, garbage
collection, and syscall overhead become the bottleneck
rather than the network.

Tailscale clearly knows this. Rather than rewriting derper,
they built Peer Relay -- a mechanism introduced in v1.86
that lets nodes in your tailnet relay traffic directly over
WireGuard, bypassing DERP entirely. Their own docs now
recommend it over custom DERP servers when performance is
a concern.

That works when relay performance isn't critical. But if
you're running industrial infrastructure, enterprise
networks, or anything where the relay is a permanent path
carrying real sustained traffic -- not a fallback you hope
never gets used -- you need the relay itself to be fast.
That's what HD is for.

None of this takes away from what Tailscale built. They
made mesh networking accessible to millions of people who
never would have touched WireGuard on their own, and that
matters more than any benchmark. HD just picks up where
their architecture has to stop.

Comparing the two is inherently unfair. That's kind of
the point.

Now go and enjoy your VPN in HD! Hyper-DERP.
