# WireGuard Relay Benchmark — GCP

Real-cloud characterization of `mode: wireguard` on GCP. Companion to [`wireguard_relay_bench.md`](wireguard_relay_bench.md) (libvirt) and [`wireguard_relay_bench_25g.md`](wireguard_relay_bench_25g.md) (Mellanox 25 GbE).

## TL;DR

- **`gve` driver only supports XDP on the GQI_QPL queue format.** That's all GCP families before C3/N4: `n1`, `n2`, `n2d`, `c2`, `t2d`, `e2`. The newer Sapphire/Genoa/Granite Rapids families (**`c3`, `c3d`, `c4`, `n4`, `h3`, `h4`**) use **DQO_RDA**, where XDP attach returns `Operation not supported` for both native and generic mode.
- **Cloud bench numbers below are on `n2-standard-4`**, with native-mode XDP attached. RX=1 / TX=1 (gve XDP requires reserving half the channels for XDP_TX, and `n2-standard-4` has max 4 channels).
- **The relay's per-CPU cost is small** — sustained 1 Gbit/s UDP forwarding burns ~2 % of one of the four vCPUs. The single-flow ceilings we measured are GCP per-flow rate caps and iperf3 sender limits, not the relay.

## Setup

```
wg-client-a (n2-standard-4)  ──┐
                               │  europe-west3-a, single VPC
wg-relay    (n2-standard-4)  ──┤  10.20.0.0/24, free intra-zone egress
                               │  hyper-derp mode: wireguard, xdp on ens4
wg-client-b (n2-standard-4)  ──┘
```

WG MTU set to 1380 (gVNIC default MTU is 1460; default WG MTU 1420 + 80 B WG overhead would fragment on the underlying NIC). Receiver tuning: `net.core.rmem_max=64M`, iperf3 `-w 16M`.

## Path: native XDP on `gve` GQI

Boot-up evidence:

```
gve 0000:00:04.0: Driver is running with GQI QPL queue format.
gve 0000:00:04.0: GVE queue format 2

# After ethtool -L ens4 rx 1 tx 1:
ip link set ens4 xdpdrv obj wg_relay.bpf.o sec xdp        # OK
ip link show ens4
2: ens4: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1460 xdp ...

[info] wg-relay xdp: attached on ens4 (ifindex=2, mode=native)
```

Failure mode if you don't reduce queues first:

```
gve: XDP load failed: The number of configured RX queues 2 should be equal
to the number of configured TX queues 2 and the number of configured RX/TX
queues should be less than or equal to half the maximum number of RX/TX
queues 2
```

## Throughput

UDP @ 1380 B (matches MTU) sweep:

| offered | achieved | loss |
| --- | --- | --- |
| 500 Mbit/s | 500 Mbit/s | 0 % |
| 1 Gbit/s | 997 Mbit/s | 0.17 % |
| 2 Gbit/s | 1.05 Gbit/s | 0.88 % |
| 4 Gbit/s | 1.03 Gbit/s | 0.85 % |
| 6 Gbit/s | 1.02 Gbit/s | 0.39 % |
| 8 Gbit/s | 973 Mbit/s | 0.19 % |
| 10 Gbit/s | 971 Mbit/s | 0.019 % |

Above 1 Gbit/s offered, achieved bitrate plateaus at ~1 Gbit/s — the iperf3 single-thread sender and per-flow caps lock things in there.

| | bitrate |
| --- | --- |
| TCP single stream | 3.04 Gbit/s |
| TCP -P 4 aggregate | 3.07 Gbit/s |
| Sustained 30 s @ 1 Gbit/s UDP | 0.1 % loss, 0 slope across 5 s buckets |

TCP escapes the UDP per-packet cap because GRO/GSO coalesce into bigger superframes — the same effect we saw on virtio + Mellanox.

## CPU profile

Relay during sustained 1 Gbit/s UDP forwarding:

| CPU | softirq |
| --- | --- |
| 0 | ~2 % |
| 1 | ~1 % |
| 2, 3 | ~0 % |

All four vCPUs near-idle. The relay's BPF processing is so cheap that even one CPU's softirq budget is barely used at the per-flow ceiling. **The relay is not the bottleneck on this VM size.** Extrapolating from 24.5 M packets through XDP at <2 % CPU per second of traffic, this n2-standard-4 has headroom for ~25-50 Gbit/s if the cloud network would let one flow fill it.

## Comparison

Across the three platforms we benched:

| | libvirt virtio | Mellanox 25 G | **GCP n2 gVNIC** |
| --- | --- | --- | --- |
| TCP single stream | 4.08 Gbit/s | 10.7 Gbit/s | **3.04 Gbit/s** |
| UDP @ 1400 B clean | ~1 Gbit/s | ~2 Gbit/s | ~1 Gbit/s |
| Latency steady avg | 1.10 ms | 0.71 ms | **0.60 ms** |
| Relay CPU at clean ceiling | 7 % of 1 vCPU | 48 % of CPU 7 | **2 % of CPU 0** |

GCP's per-flow caps are aggressive — single-flow throughput on n2-standard-4 is bracketed below the libvirt+virtio bench. Latency on the cloud VM is the lowest of any setup tested (0.60 ms) — same-zone GCP networking is fast and consistent. **Most importantly, the relay is so unloaded at the cap that scaling out to many concurrent peers should still see clean per-flow numbers** as long as the aggregate stays under the VM's egress limit.

## What this means for cloud deployment

**Rule of thumb:** on cloud, the relay isn't your throughput limit until you have many simultaneous high-bandwidth flows. Single-flow numbers are gated by the cloud network. The XDP fast path keeps the relay's per-packet cost low enough that you can pack many peers onto one VM.

**Today on GCP:**

- **`n1`, `n2`, `n2d`, `c2`, `t2d`, `e2`**: native XDP works (after `ethtool -L` reducing queues to half-max). Recommended for new deployments where XDP performance matters.
- **`c3`, `c3d`, `c4`, `n4`, `h3`, `h4`**: XDP **not supported** in upstream `gve` for the DQO queue format. Run hyper-derp without `xdp_interface` (userspace fallback) and expect ~1-2 Gbit/s per flow. Gap is being closed in upstream Linux; not in 6.12.

**Set the WG MTU.** gVNIC's default MTU is 1460, not 1500. Set `MTU = 1380` in your `wg-quick` config (or `ip link set wg0 mtu 1380`) — otherwise WG packets fragment on the gVNIC and TCP throughput collapses.

**Reduce gve channels before XDP attach.** `ethtool -L ens4 rx 1 tx 1` (or `rx 2 tx 2` on bigger boxes — the rule is half-of-max). gve's XDP requires reserving the other half for XDP_TX queues.

**On bigger n2 sizes** (n2-standard-8, n2-standard-16, …), the channel maximum doubles, so `ethtool -L rx 2 tx 2` etc. becomes possible. We tried this on n2-standard-8 and saw **single-flow throughput go down**, not up: 2.22 Gbit/s TCP single stream vs 3.04 Gbit/s on n2-standard-4. Two reasons:

1. **RSS hashes a single source-IP flow to one queue** regardless of how many queues are configured. More channels only help when there are multiple distinct source flows.
2. **GCP's per-flow rate caps appear to differ between machine sizes** — n2-standard-8 saw a lower per-flow ceiling on the same single-stream test.

A proper "scale across queues" bench needs multiple source IPs (multiple WG peers, each on a distinct macvlan or instance) so RSS spreads them across queues. We didn't run that variant here — adding it is future work for an operator who wants to size the relay against many concurrent peers.

**The takeaway for sizing:** a single hyper-derp relay on n2-standard-4 will be the bottleneck at ~1 Gbit/s for any *one* tunnel. To carry more, scale the *number of peers* (each gets their own per-flow allowance) or use multiple relay VMs behind a stateless L4 load balancer that hashes by client source IP. Vertical scaling alone won't lift a single tunnel above the per-flow cap.

## Operator setup script (verbatim)

```bash
# Relay box
sudo apt-get install -y wireguard-tools ethtool clang libbpf-dev
sudo apt-get install -y /tmp/hyper-derp_*.deb
sudo /usr/sbin/ethtool -L ens4 rx 1 tx 1     # gve XDP precondition
sudo install -d -m 1777 /tmp/einheit
sudo bash -c 'cat > /etc/hyper-derp/hyper-derp.yaml <<EOF
mode: wireguard
wg_relay:
  port: 51820
  roster_path: /var/lib/hyper-derp/wg-roster
  xdp_interface: ens4
  xdp_bpf_obj_path: /usr/lib/hyper-derp/wg_relay.bpf.o
log_level: info
einheit:
  ctl_endpoint: ipc:///tmp/einheit/hd-relay.ctl
  pub_endpoint: ipc:///tmp/einheit/hd-relay.pub
EOF'
sudo bash -c 'umask 000; nohup hyper-derp \
  --config /etc/hyper-derp/hyper-derp.yaml \
  > /tmp/hyper-derp.log 2>&1 & disown'

# Each client
sudo apt-get install -y wireguard-tools
sudo sysctl -w net.core.rmem_max=67108864 net.core.wmem_max=67108864
sudo ip link add wg0 type wireguard
echo "<priv>" | sudo wg set wg0 listen-port 51820 private-key /dev/stdin
sudo wg set wg0 peer <other-pub> endpoint <relay-ip>:51820 \
    allowed-ips <other-tunnel-ip>/32 persistent-keepalive 25
sudo ip addr add <our-tunnel-ip>/24 dev wg0
sudo ip link set wg0 mtu 1380           # critical on gVNIC
sudo ip link set wg0 up

# On the relay, register peers + link via the einheit CLI
hd-cli wg peer add alice <client-a-internal-ip>:51820
hd-cli wg peer add bob   <client-b-internal-ip>:51820
hd-cli wg link add alice bob
```

## Reproduce

The bench numbers above are produced by spinning up three `n2-standard-4` instances in one zone and following the script above. Total bench time: ~10 min. Cost: ~$0.10 of compute + free intra-zone traffic.

```bash
gcloud compute instances create wg-{relay,client-a,client-b} \
  --zone=europe-west3-a --machine-type=n2-standard-4 \
  --image-family=debian-13 --image-project=debian-cloud \
  --network-interface=subnet=<subnet>,nic-type=GVNIC

# After bench:
gcloud compute instances delete wg-relay wg-client-a wg-client-b \
  --zone=europe-west3-a --quiet
```
