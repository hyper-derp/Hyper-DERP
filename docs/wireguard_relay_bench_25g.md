# WireGuard Relay Benchmark — 25 GbE on Real Silicon

Sustained-load characterization of `mode: wireguard` running on a real Mellanox CX-4 LX 25 GbE NIC, with two independent topologies:

- **Path A**: single-NIC `XDP_TX` (the iteration-2 PR #9 baseline)
- **Path B**: dual-NIC `XDP_REDIRECT` (the wg-relay-xdp-redirect branch)

Numbers measured against the libvirt fleet are in [wireguard_relay_bench.md](wireguard_relay_bench.md). This doc is the real-hardware follow-up.

## Setup

```
workstation                           hd-test01 (relay)
  ns-A (10.99.0.1, wg0)               Xeon E5-1650 v3 (Haswell), 12 cores
    macvlan 10.10.1.10/24             32 GiB RAM
    on enp3s0f0np0 (CX-4 LX) ── DAC #1 ── ens4f0np0 10.10.1.2/24
                                                                      hyper-derp
                                                                      mode: wireguard
                                                                      xdp_interface: ens4f0np0[,ens4f1np1]
  ns-B (10.99.0.2, wg0)
    macvlan 10.10.2.11/24
    on enp3s0f1np1 (CX-4 LX) ── DAC #2 ── ens4f1np1 10.10.2.2/24
```

Both DAC links negotiate 25 Gbps, native XDP attaches successfully on `mlx5_core` for both NICs. Receiver tuning: `net.core.rmem_max=64M`, iperf3 `-w 16M`.

**Caveat that matters for the numbers:** the two clients are netns on the *same* workstation, so the workstation has to run sender encrypt + receiver decrypt + two iperf3 instances + macvlan demux for the same traffic. The relay has way more headroom than these numbers show; the workstation co-host caps single-flow benchmarks well below what the relay would do against two independent endpoint machines.

## Wire baseline

Raw 25 G between workstation and haswell, no WG, no relay:

| | bitrate | loss |
| --- | --- | --- |
| TCP | 21.3 Gbit/s | n/a |
| UDP @ 1400 B target 25 G | 7.71 Gbit/s | 0 % |

Single-stream UDP is sender-bound (one iperf3 thread sendmsg loop). TCP gets to ~85 % of nominal on the wire thanks to TSO/GSO superframes.

## Path A: single-NIC XDP_TX

Both clients ride the same DAC. Same code path as PR #9.

### Throughput

| | libvirt (PR #9) | **haswell Path A** |
| --- | --- | --- |
| TCP single stream | 4.08 Gbit/s | **10.7 Gbit/s** |
| UDP @ 1400 B clean | ~1 Gbit/s | ~2 Gbit/s |
| UDP @ 1400 B cliff | ~2 Gbit/s | **~4.8 Gbit/s** |
| Latency steady avg | 1.10 ms | **0.71 ms** |

UDP @ 1400 B sweep:

| offered | achieved | loss |
| --- | --- | --- |
| 500 Mbit/s | 500 Mbit/s | 0 % |
| 1 Gbit/s | 1 Gbit/s | 0.01 % |
| 2 Gbit/s | 2 Gbit/s | 0.06 % |
| 4 Gbit/s | 4 Gbit/s | 0.25 % |
| 6 Gbit/s | 4.6 Gbit/s | 1.1 % |
| 8 Gbit/s | 4.7 Gbit/s | 2.7 % |
| 10 Gbit/s | 4.8 Gbit/s | 1.9 % |

### CPU profile (8 Gbit/s offered, 4.8 Gbit/s achieved)

| CPU | softirq | idle |
| --- | --- | --- |
| 7 | 48 % | 52 % |
| all others | 0 % | 100 % |

RSS hashes the single source IP `10.10.1.10` to one queue → one softirq context → one CPU. With 11 idle cores, the relay has room to multiply throughput if we can spread RX work across them.

## Path B: dual-NIC XDP_REDIRECT

Each client lives on its own DAC and its own NIC on the relay. The BPF program uses `bpf_redirect_map` to send packets out the partner's NIC, with per-NIC source-MAC and source-IP rewriting (otherwise WG drops the inbound packet because its endpoint check fails).

### Single-direction (ns-A → ns-B)

Counter-intuitive: Path B is *worse* than Path A on a single-flow unidirectional benchmark.

| | Path A | Path B |
| --- | --- | --- |
| TCP single stream | 10.7 Gbit/s | **9.5 Gbit/s** |
| UDP @ 1400 B cliff | 4.8 Gbit/s | **4.4 Gbit/s** |
| CPU 7 softirq at cliff | 48 % | **94 %** |

The reason is structural: a unidirectional flow only loads *one* NIC's RX queue (ens4f0np0 on the haswell). XDP_REDIRECT's per-packet cost is a few hundred ns higher than XDP_TX (extra map lookup for `wg_nic_macs` and `wg_nic_ips`, then the cross-CPU TX queue handoff). With only one CPU active either way, the higher per-packet cost loses.

**Path B's win is parallelism, not per-packet speed.** It only shows up when traffic actually exercises both NICs' RX queues — i.e. bidirectional flows or multi-peer setups.

### Bidirectional

`iperf3 --bidir` runs one stream each way simultaneously. ns-A→ns-B traffic ingresses on ens4f0np0 (RSS to CPU 7); ns-B→ns-A traffic ingresses on ens4f1np1 (RSS to CPU 0). Both softirqs run in parallel.

UDP @ 1400 B per-direction sweep:

| offered/dir | TX dir loss | RX dir loss | aggregate |
| --- | --- | --- | --- |
| 2 Gbit/s | 0.25 % | 0.15 % | **4 Gbit/s clean** |
| 4 Gbit/s | 7 % | 5.8 % | ~6.4 Gbit/s |
| 6 Gbit/s | 6.5 % | 6 % | ~6.4 Gbit/s |

TCP `-P 4 --bidir`:

| direction | aggregate |
| --- | --- |
| TX | 6.38 Gbit/s |
| RX | 7.05 Gbit/s |
| **total** | **~13.4 Gbit/s** |

### CPU profile (4 Gbit/s/dir bidirectional)

| CPU | softirq | idle |
| --- | --- | --- |
| 0 | 84 % | 16 % |
| 7 | 89 % | 11 % |
| all others | ~0 % | ~100 % |

Both NICs' softirqs running on different CPUs, both saturating at the same time. This is exactly what Path B was supposed to unlock.

## Honest summary

**Where Path B wins:** bidirectional or multi-peer workloads where each NIC's RX queue actually has work. The bidirectional 25 G fleet test put 4 Gbit/s clean per direction (8 Gbit/s aggregate) through the relay using two different CPUs in parallel.

**Where Path B loses:** single-source unidirectional benchmarks. The relay's BPF program does one extra pair of map lookups per packet (egress NIC's MAC + IP) plus a cross-NIC TX-queue handoff, so per-packet cost is higher than XDP_TX. With only one CPU active in either case, the simpler XDP_TX path ends up faster.

**The actual bottleneck on these benches isn't the relay** — it's the workstation hosting both clients. CPU 7 on the relay was at ~90 % softirq, but that's only one of twelve cores; the relay binary has another order of magnitude of headroom. Move ns-A and ns-B to different physical machines and the relay's real capacity should be visible: extrapolating from the per-packet CPU cost (~10 ns/packet on the BPF program), 25 Gbit/s of WG payload at 1400 B would be ~2.2 Mpps × 2 (in + out) = 4.4 Mpps total, well within what one Haswell core can sustain.

## Operator guidance

Pick the topology by what your traffic actually looks like:

- **Single uplink, simple**: Path A (`xdp_interface: <one-nic>`). One BPF instance, XDP_TX bounce. Cheapest per packet.
- **Asymmetric uplinks or two upstream segments**: Path B (`xdp_interface: nic1,nic2` plus `wg peer nic <name> <iface>`). Each peer pinned to its NIC; cross-NIC redirect handles the bounce.
- **Many peers with mixed traffic**: Path B almost always wins because it actually parallelises across NICs.

The same daemon binary handles both — it's a config choice, not a build-time toggle.

## Reproduce

Setup script + driver script are in `tests/integration/wg_relay_fleet.sh` (libvirt) and `tests/integration/wg_relay_haswell.sh` (this doc's setup, follow-up branch).

Receiver tuning is non-negotiable for clean numbers:

```bash
sudo sysctl -w net.core.rmem_max=67108864 net.core.wmem_max=67108864
iperf3 -c <peer> -p 5201 -u -l 1400 -b <rate> -t 10 -w 16M [--bidir]
```
