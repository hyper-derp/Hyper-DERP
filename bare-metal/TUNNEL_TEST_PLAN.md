# Bare Metal Tunnel Test Plan

## Goal

Repeat the GCP tunnel tests on bare metal with multiple client
pairs. The relay is not the bottleneck here — each WireGuard
pair pushes ~1-2 Gbps, well below HD's 11+ Gbps kTLS ceiling.
Multiple pairs let us test relay behavior under realistic
multi-tenant load without hitting crypto limits.

Key questions:
- Do the 48x fewer retransmits hold on bare metal?
- Are the GCP latency outliers (85ms max) gone on bare metal?
- How does HD scale with 2, 3, 4+ concurrent tunnel pairs?
- Does the retransmit advantage still increase under contention?

## Hardware

| Role | Machine | CPU | IP (25GbE) |
|------|---------|-----|------------|
| Relay + Headscale | Raptor Lake (ksys) | i5-13600KF 16C/24T | 10.50.0.2 |
| Clients (all pairs) | Haswell (hd-test01) | E5-1650 v3 6C/12T | 10.50.0.1 |

SSH: `ssh worker@hd-test01` (NOPASSWD sudo), `ssh root@hd-test01`

## Client Topology

Multiple Tailscale client pairs on the Haswell, each in its
own network namespace. Each pair = one sender + one receiver.
All traffic forced through the DERP relay on the Raptor Lake.

```
Haswell (hd-test01)                     Raptor Lake (ksys)
┌──────────────────────────┐           ┌──────────────────┐
│ ns-1a (sender)   ──iperf3│           │                  │
│ ns-1b (receiver) ──iperf3│──25GbE──→ │ HD / TS relay    │
│ ns-2a (sender)   ──iperf3│←─25GbE── │ Headscale        │
│ ns-2b (receiver) ──iperf3│           │                  │
│ ns-3a (sender)   ──iperf3│           │                  │
│ ns-3b (receiver) ──iperf3│           │                  │
│ ns-4a (sender)   ──iperf3│           │                  │
│ ns-4b (receiver) ──iperf3│           │                  │
└──────────────────────────┘           └──────────────────┘
```

Each pair does ~1-2 Gbps through WireGuard. 4 pairs = ~4-8 Gbps
aggregate — well under the relay's ceiling but enough to stress
the relay's concurrent connection handling and measure retransmit
scaling.

The Haswell's 6C/12T handles WireGuard crypto for multiple pairs.
At ~1 Gbps per pair, 4 pairs uses ~30-40% of the Haswell's crypto
capacity (based on the mpstat data showing 71% at 11 Gbps raw).

## Prerequisites

### Raptor Lake (relay — ksys, local)

```bash
# 1. Headscale (control plane)
wget https://github.com/juanfont/headscale/releases/latest/download/headscale_0.25.1_linux_amd64.deb
sudo dpkg -i headscale_*.deb

# Config: /etc/headscale/config.yaml
#   server_url: http://10.50.0.2:8080
#   listen_addr: 10.50.0.2:8080
#   logtail:
#     enabled: false
#   derp:
#     server:
#       enabled: false
#     urls: []
#     paths:
#       - /etc/headscale/derp.yaml

# DERP map: /etc/headscale/derp.yaml
#   regions:
#     900:
#       regionid: 900
#       regioncode: "test"
#       regionname: "Bare Metal"
#       nodes:
#         - name: "bm-relay"
#           regionid: 900
#           hostname: "10.50.0.2"
#           derpport: 3340
#           insecurefortests: true

# Create user + auth key
sudo headscale users create tunnel-test
sudo headscale preauthkeys create --user tunnel-test \
  --reusable --ephemeral --expiration 24h

# Start
sudo systemctl start headscale

# 2. Go derper (TS comparison relay)
#    Deploy release binary to /usr/local/bin/derper
#    Run with: derper -certmode manual \
#      -certdir /path/to/certs -a :3340

# 3. Hyper-DERP relay
#    Deploy binary to /usr/local/bin/hyper-derp
#    Run with: hyper-derp --port 3341 \
#      --cert /path/to/cert.pem --key /path/to/key.pem \
#      --workers 4 --metrics-port 9090 --debug-endpoints

# 4. TLS certs (self-signed, matching DERP map hostname)
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
  -keyout key.pem -out cert.pem -days 365 -nodes \
  -subj "/CN=10.50.0.2" \
  -addext "subjectAltName=IP:10.50.0.2"
# Copy to derper cert dir as derp.tailscale.com.crt/.key
# (or match whatever certname the DERP map expects)

# 5. kTLS
sudo modprobe tls
lsmod | grep tls
```

### Haswell (client pairs via network namespaces — hd-test01)

```bash
# Install Tailscale client
curl -fsSL https://tailscale.com/install.sh | sh
# Or: apt install tailscale (if in repos)

# Install iperf3
sudo apt install -y iperf3
```

Each pair gets two namespaces (sender + receiver), each running
its own tailscaled instance with separate state dirs.

```bash
# Create pair N (sender = ns-Na, receiver = ns-Nb)
for role in a b; do
  NS="ns-${N}${role}"
  ip netns add $NS
  ip netns exec $NS ip link set lo up

  # veth pair for connectivity to host network
  ip link add veth-${NS} type veth peer name veth-${NS}-in
  ip link set veth-${NS}-in netns $NS
  ip addr add 10.60.${N}.1/24 dev veth-${NS}
  ip link set veth-${NS} up
  ip netns exec $NS ip addr add 10.60.${N}.2/24 dev veth-${NS}-in
  ip netns exec $NS ip link set veth-${NS}-in up

  # Route to relay via host
  ip netns exec $NS ip route add default via 10.60.${N}.1

  # Enable forwarding on host
  iptables -t nat -A POSTROUTING -s 10.60.${N}.0/24 -o ens4f0np0 -j MASQUERADE
  echo 1 > /proc/sys/net/ipv4/ip_forward

  # Tailscaled in namespace
  ip netns exec $NS tailscaled \
    --state=/var/lib/tailscale-${NS}/ \
    --socket=/var/run/tailscale-${NS}/tailscaled.sock \
    --port=$((41640 + N * 2 + (role == 'b' ? 1 : 0))) &

  # Auth
  ip netns exec $NS tailscale \
    --socket=/var/run/tailscale-${NS}/tailscaled.sock \
    up --login-server http://10.50.0.2:8080 \
    --authkey <key> --hostname pair${N}-${role} --accept-dns=false
done
```

### Force DERP

Network namespaces provide natural isolation — no direct UDP
path between namespaces. Tailscale clients will use DERP.
Verify with `tailscale status` in each namespace showing
`relay "test"` for all peers.

## Test Phases

### Phase 1 — Single Pair Baseline (10 runs × 15s)

Replicate GCP Phase 1 on bare metal.

| Test | Description |
|------|-------------|
| tcp1 | iperf3 -c <recv_ts_ip> -t 15 -J |
| tcp4 | iperf3 -c <recv_ts_ip> -t 15 -P 4 -J |
| ping | ping -c 1000 -i 0.01 <recv_ts_ip> |
| latload | iperf3 background + ping simultaneously |

Collect: throughput, retransmits, ping min/avg/max/mdev.

### Phase 2 — Two Pairs (10 runs × 15s)

Both pairs run simultaneously. Same tests as Phase 1 per pair.

Additional metrics:
- Per-pair throughput (fairness)
- Aggregate throughput
- Cross-pair interference: does pair 2 degrade pair 1 latency?

### Phase 3 — Three Pairs (10 runs × 15s)

Same as Phase 2 with 3 pairs. Scaling curve continues.

Additional:
- Mixed workload: 2 pairs bulk TCP, 1 pair ping-only.
  Does bulk traffic destroy interactive latency?

### Phase 4 — Four Pairs (10 runs × 15s)

Push toward Haswell client crypto limits (~4-8 Gbps aggregate).
Same tests. Scaling curve completion.

### Phase 5 — Long Duration (300s)

All pairs active, single TCP stream each, 5 minutes sustained.

Key metrics:
- Retransmit rate per pair over time
- Max latency (the bare metal vs hypervisor question)
- Memory stability (RSS growth)
- Throughput stability (does it degrade over 5 min?)

### Phase 6 — Client Churn (60s)

Same as GCP Phase 4: one pair does bulk transfer, one pair
pings, one pair connects/disconnects every 10-15s.

Key metric: does churn affect bulk throughput and retransmits?

## Comparison Protocol

Per phase:

1. Verify all pairs connected, DERP path confirmed
2. Start TS relay on Raptor Lake
3. Run all tests for this phase (TS)
4. Kill TS, drop caches, wait 10s
5. Start HD relay on Raptor Lake
6. Verify clients reconnected via DERP
7. Run all tests for this phase (HD)
8. Kill HD

Run mpstat on both machines during every phase.

## Scaling Curve (expected)

| Pairs | TS aggregate | HD aggregate | HD/TS | HD retrans ratio |
|------:|-------------:|-------------:|------:|-----------------:|
| 1 | ~1.0 Gbps | ~1.7 Gbps | ~1.7x | ~16x fewer |
| 2 | ~1.8 Gbps | ~3.0 Gbps | ~1.7x | ~21x fewer |
| 3 | ~2.4 Gbps | ~4.0 Gbps | ~1.7x | ~24x fewer |
| 4 | ~3.0 Gbps | ~5.0 Gbps | ~1.7x | ~30x fewer |

These are rough estimates based on GCP data. The ratios should
be similar; absolute throughput depends on Haswell client crypto
capacity.

If the retransmit ratio keeps increasing with more pairs (as it
did on GCP: 16x → 21x → 24x), that confirms the io_uring
batching advantage scales with concurrency.

## What Bare Metal Uniquely Answers

1. **Latency outliers**: GCP max was 85ms (hypervisor). Bare
   metal should be <1ms max. If it's still >10ms, it's a code
   issue.

2. **Retransmit consistency**: 48.7x over 5 min on GCP. Does
   the same hold without hypervisor scheduling noise?

3. **Idle tunnel latency**: bare metal baseline should be
   ~30-50us (vs GCP's ~160us). Shows true relay processing
   overhead.

4. **Scaling under crypto load**: with WireGuard on dedicated
   cores (no hypervisor sharing), does the per-pair throughput
   hold better as pairs increase?

## Data Organization

```
bench_results/bare-metal-tunnel/
  phase1/
    ts/  hd/
  phase2/
    ts/  hd/
  phase3/
    ts/  hd/
  phase4/
    ts/  hd/
  phase5_300s/
    ts/  hd/
  phase6_churn/
    ts/  hd/
  cpu/
    ts_phase{1..6}_relay_mpstat.txt
    ts_phase{1..6}_client_mpstat.txt
    hd_phase{1..6}_relay_mpstat.txt
    hd_phase{1..6}_client_mpstat.txt
  system_info.txt
  REPORT.md
```

## Time Estimate

| Phase | Time |
|-------|-----:|
| Setup (namespaces, Headscale, verify) | ~1.5 hr |
| Phase 1 (1 pair, TS + HD) | ~30 min |
| Phase 2 (2 pairs, TS + HD) | ~30 min |
| Phase 3 (3 pairs, TS + HD) | ~30 min |
| Phase 4 (4 pairs, TS + HD) | ~30 min |
| Phase 5 (300s, TS + HD) | ~15 min |
| Phase 6 (churn, TS + HD) | ~15 min |
| **Total** | **~3.5 hrs** |
