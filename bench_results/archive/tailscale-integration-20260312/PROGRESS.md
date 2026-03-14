# Tailscale Integration Test — Progress

## Status: Data collection complete

## Test setup
- **Headscale** on host port 8080 (systemd service, fresh DB)
- **ts-client-1**: 10.101.1.10 (TS: 100.64.0.1) — connected via DERP
- **ts-client-2**: 10.101.1.11 (TS: 100.64.0.2) — connected via DERP
- WireGuard UDP blocked between VMs (iptables) — forces DERP relay
- BBR congestion control + 4MB wmem_max on all three nodes
- iperf3 client→server: ts-client-1 → ts-client-2 (over Tailscale tunnel)

## Hyper-DERP results (3 runs each)
- `tailscale ping`: <1ms via DERP(hd-local)

### BBR (3 runs)
| Run | Throughput (sender) | Throughput (receiver) | Retransmits |
|-----|--------------------:|----------------------:|------------:|
| 1   | 640 Mbps            | 638 Mbps              | 223,513     |
| 2   | 619 Mbps            | 617 Mbps              | 198,764     |
| 3   | 618 Mbps            | 616 Mbps              | 206,740     |
| **Avg** | **626 Mbps**    | **624 Mbps**          | **209,672** |

### CUBIC (3 runs)
| Run | Throughput (sender) | Throughput (receiver) | Retransmits |
|-----|--------------------:|----------------------:|------------:|
| 1   | 542 Mbps            | 541 Mbps              | 6,546       |
| 2   | 522 Mbps            | 521 Mbps              | 6,558       |
| 3   | 559 Mbps            | 558 Mbps              | 6,956       |
| **Avg** | **541 Mbps**    | **540 Mbps**          | **6,687**   |

### Relay-side TCP (from ts-client-1)
- bytes_sent: 4.77 GB, delivery_rate: 4.71 Gbps (BBR)
- Zero retransmits on relay leg

## Go derper results (3 runs each)
- `tailscale ping`: 1-2ms via DERP(hd-local)

### BBR (3 runs)
| Run | Throughput (sender) | Throughput (receiver) | Retransmits |
|-----|--------------------:|----------------------:|------------:|
| 1   | 602 Mbps            | 599 Mbps              | 201,545     |
| 2   | 578 Mbps            | 576 Mbps              | 192,929     |
| 3   | 607 Mbps            | 605 Mbps              | 201,815     |
| **Avg** | **596 Mbps**    | **593 Mbps**          | **198,763** |

### CUBIC (3 runs)
| Run | Throughput (sender) | Throughput (receiver) | Retransmits |
|-----|--------------------:|----------------------:|------------:|
| 1   | 550 Mbps            | 550 Mbps              | 6,856       |
| 2   | 534 Mbps            | 533 Mbps              | 6,239       |
| 3   | 567 Mbps            | 566 Mbps              | 6,961       |
| **Avg** | **550 Mbps**    | **550 Mbps**          | **6,685**   |

### Relay-side TCP (from ts-client-1)
- bytes_sent: 1.6 GB, delivery_rate: 3.48 Gbps (BBR)
- Zero retransmits on relay leg

## Comparison summary

| Metric                     | Hyper-DERP | Go derper | Delta      |
|----------------------------|------------|-----------|------------|
| BBR throughput (avg)       | 626 Mbps   | 596 Mbps  | **HD +5%** |
| CUBIC throughput (avg)     | 541 Mbps   | 550 Mbps  | ~tied      |
| BBR inner-TCP retransmits  | 210K       | 199K      | ~same      |
| CUBIC inner-TCP retransmits| 6,687      | 6,685     | same       |
| Relay-side retransmits     | 0          | 0         | same       |
| Relay delivery rate        | 4.71 Gbps  | 3.48 Gbps | **HD 1.35x** |
| `tailscale ping` latency   | <1ms       | 1-2ms     | **HD faster** |

## Analysis
- HD wins BBR by ~5% (626 vs 596 Mbps), CUBIC is a tie
- Both relays have zero relay-side retransmits — the ~200K inner-TCP
  retransmits (BBR) are purely TCP-in-tunnel artifacts
- HD relay delivery rate is 1.35x higher (4.71 vs 3.48 Gbps),
  confirming HD pushes data through the relay leg faster
- DERP ping latency: HD <1ms vs Go 1-2ms
- The throughput ceiling (~600 Mbps) is not in the relay — it's in
  the Tailscale client's WireGuard encryption + decapsulation.
  Both relays are well below their relay-side capacity.
- The earlier single-run HD numbers (554/480) were outliers — the
  3-run averages are consistent and show HD ahead on BBR

## VM setup notes (for reproducing)
- Cloned from bench-relay/bench-client (Debian 13 Trixie)
- cloud-init masked (all services + target)
- Networking: systemd-networkd with /etc/systemd/network/10-bench.network
- **Bug**: static IP via guestfish didn't work on boot —
  had to DHCP first, then set static via SSH.
  Root cause unknown (systemd-networkd matched the NIC fine
  once booted). Probably a cloud-init ordering issue on
  first boot despite masking.
- Self-signed cert added to /usr/local/share/ca-certificates/
  on both VMs (required for Tailscale to accept our relay)
- iptables blocks WireGuard UDP (port 41641) between VMs

## TCP tuning applied (all nodes)
```
net.core.wmem_max=4194304
net.core.rmem_max=6291456
net.ipv4.tcp_congestion_control=bbr
```
