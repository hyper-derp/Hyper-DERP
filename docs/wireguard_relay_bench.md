# WireGuard Relay Benchmark — XDP Fast Path

Sustained-load characterization of `mode: wireguard` with the XDP fast path attached. Measured against the libvirt fleet (`hd-r2` relay, `hd-c1`/`hd-c2` clients), single virtio bridge, native-mode XDP on `virtio_net`. Receiver tuned per the [quickstart](wireguard_relay_quickstart.md) (`net.core.rmem_max=32M`, iperf3 `-w 8M`); without that the receiver overflows and you measure iperf3 instead of the relay.

This is *one VM box*, single-queue NICs, 2 vCPU + 1 GiB per VM. **Read these as relative ratios, not absolute capacity** — a multi-queue NIC on real hardware will move every ceiling.

## Headline

- **CPU is not the bottleneck.** Sustained 2 Gbit/s UDP = ~7 % of one of the relay's two vCPUs. RSS at 18 MiB after 86 M forwarded packets.
- **Throughput cliff is the virtio TX ring.** 256 entries (max via ethtool). Above ~2 Gbit/s offered, `tx_xdp_tx_drops` accumulates because the host vhost-net thread can't drain fast enough.
- **Latency under load is lower than idle latency.** 0.149 ms avg under a 1 Gbit/s blast vs 0.521 ms idle — the softirq stays scheduled and IRQ paths stay hot in cache.
- **Daemon restart mid-blast costs ~560 ms of forwarding.** Total loss across a 30 s sustained blast that included one restart: 0.37 %.

## UDP @ 1400 B — sustained loss vs offered rate

| offered rate | direct | userspace | **XDP** |
| --- | --- | --- | --- |
| 100 Mbit/s | 0 % | 0 % | **0 %** |
| 500 Mbit/s | 0 % | 0.61 % | **0 %** |
| 1 Gbit/s | 0 % | 3 % | **0 %** |
| 2 Gbit/s | 0.55 % | saturates | **0.30 %** |
| 3 Gbit/s | saturates | — | falls off (~41 % loss) |

XDP is indistinguishable from direct through 1 Gbit/s. At 2 Gbit/s both are limited by the same virtio TX queueing dynamics. Above 2 Gbit/s the offered rate exceeds what the bridge + TX rings can drain steadily and loss goes non-linear.

## Packet rate ceiling — small packets vs large

UDP at increasing payload sizes, target rate scaled up to find the cliff (`-w 8M`):

| size | best clean rate | best clean pps |
| --- | --- | --- |
| 64 B | ~270 Mbit/s @ 2.9 % loss | ~4.4 M pps |
| 200 B | ~200 Mbit/s clean / 488 Mbit/s @ 2.3 % | ~1 M pps clean, 2.5 M pps marginal |
| 500 B | ~1 Gbit/s @ 0.31 % | ~2 M pps |
| 1400 B | ~1 Gbit/s clean / ~2 Gbit/s @ 0.30 % | ~178 k pps |

Smaller packets stress the per-packet path more than the byte-rate path — the BPF program runs once per packet regardless of size. The pps ceiling lands around **3-4 Mpps** on this single-queue VM, again gated by TX ring fill.

## Concurrent flows

`iperf3 -P 4` versus single stream:

| | single stream | 4 parallel | scaling |
| --- | --- | --- | --- |
| TCP | 3.86 Gbit/s | **4.59 Gbit/s** aggregate | +20 % |

TCP scales sub-linearly because all four flows hit the same RX queue → same softirq CPU → same XDP program. With a multiqueue NIC + RPS this would scale much better.

## Sustained 60 s @ 1 Gbit/s UDP

```
[ ID] Interval           Transfer     Bitrate         Lost/Total Datagrams
[  5]   0.00-60.00  sec  6.98 GBytes   1000 Mbit/s    2717/5357146 (0.051%)
```

- 5.36 M packets in each direction, 0.051 % receiver-side loss
- Every 5 s bucket reads 1000 Mbit/s — no slope, no degradation over the run
- Relay's `tx_xdp_tx_drops` grew by **+302** over the whole minute (rest is iperf3 receive variance)
- `rx_xdp_drops` stayed at 0 — the BPF program never dropped a packet
- Daemon RSS climbed from 18.0 MiB → 18.4 MiB while handling 86 M packets

This is the operating point the relay is designed for.

## Latency under load

```
              min     avg     max     mdev
idle          0.255   0.521   1.547   0.264   ms
under 1 G UDP 0.087   0.149   2.211   0.176   ms
```

200 pings each, 50 ms interval. Under sustained 1 Gbit/s UDP forwarding, ICMP RTT is **lower** than idle. Two effects:

- The RX softirq stays hot — no scheduling gap between ping arrivals.
- NIC IRQ coalescing is working continuously, packet path is in cache.

Max RTT grew 1.55 → 2.21 ms (+0.66 ms tail jitter), but the median moved the other direction.

## CPU profile during 2 Gbit/s UDP

`mpstat -P ALL 1` on the relay during a sustained 2 Gbit/s blast:

```
CPU    %sys   %iowait  %soft   %idle
all    0.00   0.30     5.5     94.0
0      0.00   0.50     2.1     97.0    (TX completions land here)
1      0.00   0.00     9.5     90.0    (RX softirq + XDP runs here)
```

Total: ~5-7 % of one of the two vCPUs. Single-queue NIC pins all RX work to CPU 1. The relay has roughly **14× headroom** before CPU becomes the bottleneck — every other limit (virtio TX ring, bridge, receiver socket) hits well before that.

Interrupt rate during the run: ~94 k IRQ/s for `virtio0-input.0` (CPU 1), ~83 k IRQ/s for `virtio0-output.0` (CPU 0). At 178 kpps that's ~2 packets per IRQ — very little NIC-side batching, which is also fixable with a real multi-queue NIC + ethtool coalescing.

## Disruption recovery

30 s @ 500 Mbit/s UDP, with `systemctl restart hyper-derp` issued at t=8 s:

| | bitrate | datagrams |
| --- | --- | --- |
| sender total | 500 Mbit/s | 1,339,287 (0 lost) |
| receiver total | 498 Mbit/s | 1,334,361 (4926 lost, **0.37 %**) |

Per-bucket throughput stayed at 500 Mbit/s through the restart; the loss is concentrated in the 560 ms restart window (XDP detached, daemon stopped, daemon restarted, BPF reattached, MAC learning re-bootstraps). Receiver never noticed a slowdown — packets queued in flight.

Equivalent test for `wg link remove + add`: 6 packets dropped during the 3 s window (one per ping interval), forwarding resumed within one cold-start cycle after re-add. Counters report `drop_no_link=6` matching the icmp_seq gap exactly.

## What's actually limiting us

Ranked by which would unlock the next factor of throughput on this VM:

1. **virtio TX ring depth (256).** Single biggest cap. Set higher in libvirt VM XML (`<driver tx_queue_size='1024' .../>`); requires recreating the VM, not a guest-level knob.
2. **Single-queue NIC.** All RX softirqs land on one CPU. `<driver queues='4' .../>` in libvirt + ethtool combined-queues + RPS would parallelise.
3. **Receiver UDP socket.** Already tuned via `rmem_max`; would also parallelise with multi-queue.

Nothing on that list is a hyper-derp issue — the userspace forwarder has been a control plane only ever since iteration-2 attached. The BPF program at 7 % of one CPU has another order of magnitude of headroom.

## Reproduce

The raw runs are committed to `build/wg-bench-results.txt` for reference. The setup is:

```bash
# Once, on each iperf3 server side
sudo sysctl -w net.core.rmem_max=33554432

# Then per test
iperf3 -s -D -p 5201    # server
iperf3 -c <peer> -p 5201 -u -l 1400 -b <rate> -t 60 -w 8M -i 5
```

For the disruption recovery + CPU profile + latency-under-load tests, see the `tests/integration/wg_relay_fleet.sh` driver and the inline scripts in `wireguard_relay_quickstart.md`.
