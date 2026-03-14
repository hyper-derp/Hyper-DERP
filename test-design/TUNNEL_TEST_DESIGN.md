# Tunnel Test Design — Real Tailscale Clients Through DERP Relay

## Objective

Measure end-to-end performance of real Tailscale/WireGuard traffic
relayed through HD vs Go derper. The synthetic bench measures raw relay
throughput with a custom client; this test answers: **does the relay
advantage translate to real-world client experience?**

## What This Test Adds Beyond Synthetic Benchmarks

The synthetic bench controls offered load and measures relay-side
throughput/loss. It doesn't capture:

- WireGuard encryption overhead on clients interacting with relay load
- Tailscale's connection management (key rotation, peer discovery)
- Realistic packet size distribution (not just fixed 1400B)
- Bidirectional traffic (synthetic bench is unidirectional per pair)
- End-to-end latency as perceived by the application, not just relay
- Relay behavior under mixed traffic (bulk + interactive simultaneously)

## Phase 0 — Tailscale Familiarization

Before any benchmarking, stand up a minimal 2-VM setup to learn how
the Tailscale client actually behaves. We haven't used Tailscale
before; discovering client quirks during the real test run wastes time
and money.

### Setup

- 2x cheapest GCE VMs (e2-micro or e2-small), same region
- Install Tailscale on both, join a test tailnet
- Run the Go derper on one of them (or a third micro VM)

### Things to verify

1. **Custom DERP map injection.** Configure clients to use only our
   self-hosted derper. Confirm no traffic goes to Tailscale's public
   DERP infrastructure. Check with `tailscale netcheck` and
   `tailscale status`.

2. **Forcing relay path.** Apply the iptables UDP block between
   clients. Verify `tailscale status` shows "relay" not "direct".
   Time how long failover takes (STUN timeout → DERP fallback).

3. **InsecureForTests behavior.** Does the client accept a non-TLS
   DERP connection? If not, we need TLS on the relay for tunnel
   tests (different from our synthetic bench setup).

4. **DERP map with default regions removed.** Does the client
   complain or silently fall back to hardcoded defaults? Need to
   confirm total isolation.

5. **Auth key workflow.** Create an ephemeral reusable auth key,
   `tailscale up --authkey=...`, verify auto-cleanup on disconnect.
   Check device limit behavior.

6. **Basic connectivity.** `ping` and `iperf3` through the tunnel.
   Confirm traffic is actually traversing the relay (check derper
   logs or HD /debug/workers).

7. **Client resource usage.** How much CPU/RAM does the Tailscale
   client consume idle and under load on a small VM? Informs client
   VM sizing for later phases.

### Expected time: 1-2 hours. Cost: < $0.50.

### Exit criteria

- We know how to configure clients to use only our relay
- We know whether TLS is required on the relay
- We have a working auth key flow
- We can confirm traffic goes through DERP (not direct)
- We have a rough sense of client overhead

Once this is done, tear down the micro VMs and proceed to Phase 1
on the real test infrastructure.

## Infrastructure

### VMs (GCE, europe-west3-b, same VPC as existing benchmarks)

| Role        | Machine Type   | Count | Notes                          |
|-------------|----------------|------:|--------------------------------|
| Relay       | c4-highcpu-4   |     1 | Runs HD or TS, one at a time   |
| Client      | c4-highcpu-2   |   2-8 | Tailscale node, paired sender/receiver |

Client count scales with test phase:
- Phase 1: 2 VMs (1 pair)
- Phase 2: 4 VMs (2 pairs)
- Phase 3: 8 VMs (4 pairs)

### Why c4-highcpu-4 for relay

Matches the configuration where HD shows its strongest advantage (5-6x).
If the advantage holds end-to-end here, the story is clear.

### Why c4-highcpu-2 for clients

Cheap. Clients shouldn't be the bottleneck — WireGuard on a 2 vCPU
can push 1-2 Gbps, which is enough to stress a 4 vCPU relay with
4 pairs.

## Forcing DERP Relay

**Critical requirement.** Tailscale clients will establish direct
WireGuard paths and bypass DERP if they can reach each other directly.
All traffic must go through the relay or the test is meaningless.

### Approach: Firewall block on direct UDP

On every client VM, block direct UDP to other client VMs:

```bash
# Block direct WireGuard (UDP 41641) between client VMs
# Allow only TCP to relay VM
for CLIENT_IP in <other_client_ips>; do
  iptables -A OUTPUT -d $CLIENT_IP -p udp -j DROP
  iptables -A INPUT -s $CLIENT_IP -p udp -j DROP
done
```

Tailscale will fail direct path negotiation and fall back to DERP.

### Verification

```bash
# On each client, after connections are established:
tailscale status
# Should show "relay" not "direct" for all peers

# Also check relay-side:
# HD: /debug/workers should show active peers
# TS: watch derper logs
```

## Custom DERP Map

Clients must use our relay, not Tailscale's public DERP servers.

```json
{
  "Regions": {
    "900": {
      "RegionID": 900,
      "RegionCode": "test",
      "RegionName": "Test Relay",
      "Nodes": [{
        "Name": "test-relay",
        "RegionID": 900,
        "HostName": "<relay-internal-ip>",
        "DERPPort": 443,
        "InsecureForTests": true
      }]
    }
  }
}
```

Set via `--derp-map` or tailnet ACL policy. Disable default DERP
regions so no traffic leaks to Tailscale infrastructure.

## Authentication

Use Tailscale ephemeral auth keys:

```bash
# Generate on tailnet admin console or via API
# Ephemeral = auto-removed when client disconnects
# Reusable = same key for all containers/VMs
tailscale up --authkey=tskey-auth-XXXXX --hostname=client-N
```

Set high device limit on the key. Ephemeral cleanup avoids polluting
the tailnet.

## Test Phases

### Phase 1 — Single Pair Baseline (2 client VMs)

**Purpose:** Establish baseline end-to-end throughput and latency
through the relay with minimal variables.

#### Tests

1. **Bulk throughput (TCP through tunnel)**
   ```bash
   # Receiver VM:
   iperf3 -s
   # Sender VM (use Tailscale IP of receiver):
   iperf3 -c <receiver-tailscale-ip> -t 30 -P 1
   ```
   - 30 seconds, single stream
   - Repeat with 2, 4, 8 parallel streams (-P)
   - Run 10 times per configuration

2. **Bulk throughput (UDP through tunnel)**
   ```bash
   iperf3 -c <receiver-tailscale-ip> -u -b 0 -t 30
   ```
   - UDP mode, unlimited bandwidth target
   - Measures actual achievable throughput + loss
   - 10 runs

3. **Latency (ICMP through tunnel)**
   ```bash
   ping -c 1000 -i 0.01 <receiver-tailscale-ip>
   ```
   - 1000 pings, 10ms interval
   - Extract: min, avg, max, mdev, full distribution
   - 10 runs

4. **Latency under load**
   - Run iperf3 bulk transfer in background
   - Simultaneously run ping measurement
   - Captures tail latency when relay is busy
   - 10 runs per load level (idle, 1-stream, 4-stream)

#### Data to collect per run

| Metric               | Source                  | Format       |
|----------------------|-------------------------|--------------|
| Throughput (Mbps)    | iperf3 --json           | JSON         |
| Retransmits          | iperf3 --json           | JSON         |
| UDP loss %           | iperf3 --json (UDP)     | JSON         |
| Ping RTT             | ping output, parsed     | CSV          |
| Relay CPU %          | pidstat -p <pid> 1      | CSV          |
| Client CPU %         | pidstat 1 (per VM)      | CSV          |
| Relay worker stats   | HD: curl /debug/workers | JSON         |
| Relay metrics        | HD: curl /metrics       | Prometheus   |

### Phase 2 — Two Pairs (4 client VMs)

**Purpose:** Introduce cross-shard forwarding and resource sharing.

Same tests as Phase 1, all pairs running simultaneously.
Additional measurements:

- **Fairness:** Do both pairs get equal throughput? Report per-pair
  numbers, not just aggregate.
- **Cross-pair interference:** Does adding pair 2 degrade pair 1's
  latency? Compare Phase 1 latency vs Phase 2 per-pair latency.

### Phase 3 — Four Pairs (8 client VMs)

**Purpose:** Approach relay saturation with real traffic.

Same tests. Additional:

- **Scaling curve:** Plot aggregate throughput and per-pair throughput
  vs pair count (1, 2, 4). Does it scale linearly?
- **Mixed workload:** 2 pairs doing bulk transfer, 2 pairs doing
  interactive (low-rate ping). Does bulk traffic destroy interactive
  latency?

### Phase 4 — Chaos / Realistic Scenarios

Only if Phases 1-3 produce clean data. Ideas:

- **Client churn:** Script clients connecting/disconnecting every
  10-30 seconds while load runs. Measures control plane impact on
  data plane.
- **Asymmetric load:** One pair at full blast, others at trickle.
  Tests fairness under skew.
- **Long duration:** 5-minute sustained load. Exposes memory leaks,
  GC pauses (Go), buffer bloat.

## Comparison Protocol

For every test, run once with HD relay, once with TS relay:

1. Start HD on relay VM
2. Verify clients connect, confirm DERP relay path
3. Run full test suite
4. Kill HD
5. `echo 3 > /proc/sys/vm/drop_caches` on relay VM
6. Start TS derper on relay VM (release build, GOMAXPROCS default)
7. Verify clients connect, confirm DERP relay path
8. Run identical test suite
9. Kill TS

Same client VMs, same auth, same iperf3 parameters. Only variable
is the relay binary.

## Key Metrics for the Story

### Primary (the headline numbers)

- **End-to-end throughput ratio** (HD / TS) at each pair count
- **Relay CPU at matched throughput** — if both relays achieve X Mbps
  end-to-end, how much CPU does each use?
- **P99 latency under load** — this is the user-experience metric

### Secondary (the engineering detail)

- Throughput scaling: does HD scale linearly with pairs? Does TS?
- Fairness: variance between pairs under equal load
- Mixed workload: interactive latency while bulk runs
- Retransmit rate: proxy for relay-induced packet reordering or loss

### What Bad Results Would Look Like

Be prepared for:
- **WireGuard crypto dominates.** If client CPU maxes out on crypto,
  both relays look identical because the bottleneck is client-side.
  This is a real possibility on c4-highcpu-2 clients.
  *Mitigation:* Report relay CPU alongside throughput. Even if
  throughput matches, HD at 15% CPU vs TS at 60% is a clear win.
- **Tailscale overhead masks relay.** The Tailscale client itself
  (STUN, magicsock, netcheck) adds overhead beyond WireGuard.
  Both relays suffer equally from this.
- **Small advantage at low pair count.** With 1 pair the relay is
  idle; differences emerge under contention.

## Data Organization

```
tunnel-test/
  results/
    phase1/
      hd/
        run-01-tcp-1stream.json
        run-01-udp.json
        run-01-ping-idle.csv
        run-01-relay-cpu.csv
        ...
      ts/
        (same structure)
    phase2/
      ...
  scripts/
    setup-clients.sh      # install tailscale, configure DERP map
    block-direct.sh       # firewall rules to force relay
    run-phase1.sh         # orchestrate Phase 1 test suite
    run-phase2.sh         # orchestrate Phase 2
    run-phase3.sh         # orchestrate Phase 3
    collect-relay-stats.sh # poll /debug/workers and /metrics
    parse-results.py      # extract metrics into analysis-ready CSV
  TEST_DESIGN.md          # this file
  RESULTS.md              # analysis (populated after runs)
```

## Statistical Requirements

Same rigor as synthetic benchmarks:
- Minimum 10 runs per configuration
- Report: mean, stddev, 95% CI, CV%
- Welch's t-test for HD vs TS comparisons
- Flag CV > 10%
- For latency: full percentile ladder (p50, p90, p95, p99, p999)

## Cost Estimate

| Phase | Client VMs | Hours (est.) | Cost (est.)   |
|------:|-----------:|-------------:|---------------|
|     1 |          2 |            2 | ~$0.30        |
|     2 |          4 |            2 | ~$0.50        |
|     3 |          8 |            3 | ~$1.20        |
|     4 |          8 |            2 | ~$0.80        |
| **Total** |      |              | **~$2.80**    |

Based on c4-highcpu-2 at ~$0.07/hr, c4-highcpu-4 at ~$0.13/hr.
Relay VM likely already running for other benchmarks.

## Open Questions

- [ ] Can we use `InsecureForTests` to skip TLS on the DERP
      connection, matching the synthetic bench setup? Or does
      Tailscale client require TLS to the relay?
- [ ] Does Tailscale client respect a DERP map that removes all
      default regions? Need to verify no fallback to public relays.
- [ ] What Tailscale client version? Pin to a specific version so
      results are reproducible.
- [ ] Do we need to disable Tailscale's bandwidth estimation /
      path selection logic, or does forcing DERP handle this?
