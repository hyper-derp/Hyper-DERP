# Tunnel Test Phase 0 — Tailscale Familiarization Results

**Date:** 2026-03-14
**Infrastructure:** GCP c4-highcpu-16 (relay), c4-highcpu-8 (client), europe-west3-b
**Tailscale:** 1.82.0, Headscale 0.25.1 (self-hosted control plane)
**Go derper:** release build, TLS, port 3340
**Hyper-DERP:** P3 bitmask build (a258a89), kTLS, 4 workers, port 3341

## Open Questions Answered

### Q1: InsecureForTests — does the client accept non-TLS DERP?

**No.** The Tailscale client always attempts TLS to the DERP relay,
even with `insecurefortests: true` in the DERP map. With a plain
HTTP derper, the client logs:

```
tls: first record does not look like a TLS handshake
```

`insecurefortests: true` skips **certificate validation** but still
requires TLS. The derper must run with `-certmode manual` and certs.

### Q2: OmitDefaultRegions — does it isolate from public DERP?

**Yes.** Headscale's DERP map with only region 900 (our relay) works.
`tailscale netcheck` shows only our region. No traffic to Tailscale's
public DERP infrastructure. `logtail.enabled: false` in Headscale
config prevents log uploads.

### Q3: Auth key flow

Headscale preauthkeys work identically to Tailscale's:

```bash
sudo headscale preauthkeys create --user tunnel-test \
  --reusable --ephemeral --expiration 24h
sudo tailscale up --login-server http://10.10.0.2:8080 \
  --authkey <key> --hostname <name> --accept-dns=false
```

### Q4: Forcing relay path

UDP block between VMs forces DERP relay:

```bash
sudo iptables -A OUTPUT -d <other_vm> -p udp -j DROP
sudo iptables -A INPUT -s <other_vm> -p udp -j DROP
```

`tailscale status` confirms `relay "test"`. `tailscale ping`
shows `via DERP(test)`.

### Q5: Client resource usage

| Metric | Value |
|--------|-------|
| RSS | 117 MB |
| VSZ | 1.3 GB |
| CPU | 5.5% idle |

### Q6: Headscale eliminates phone-home surface

Zero contact with Tailscale Inc servers. All coordination is
local via Headscale on 10.10.0.2:8080. `logtail: enabled: false`
prevents any log exfiltration. Tailscale clients try to reach
`log.tailscale.com` but timeout harmlessly (no internet).

## Setup Requirements

### nftables

The benchmark VMs use restrictive nftables (`policy drop`). Must
add rules **before** the drop rule (not after — `nft add` appends
after drop, use `nft insert` with position):

```bash
# On both VMs — insert before the drop rule:
sudo nft insert rule inet filter output position <before_drop> \
  oif "tailscale0" accept
sudo nft insert rule inet filter output position <before_drop> \
  ip daddr 100.64.0.0/10 accept
sudo nft insert rule inet filter input position <before_drop> \
  iif "tailscale0" accept
sudo nft insert rule inet filter input position <before_drop> \
  ip saddr 100.64.0.0/10 accept
```

**Caution:** `systemctl restart tailscaled` does NOT clear these,
but a full nftables reload will. Re-add after any nft flush.

### Self-signed cert with Go derper

Go derper with `-certmode manual` outputs the exact `CertName` to
use in the DERP map. However, `insecurefortests: true` is simpler
and sufficient for benchmarking (skips cert validation).

The Go derper adds a second cert to the TLS chain (its DERP key).
Using `certname` with a sha256 pin fails with `unexpected multiple
certs presented`. Use `insecurefortests: true` instead.

## Phase 0 Throughput Results (single run each)

| Metric | Go derper (TS) | Hyper-DERP (HD) | Ratio |
|--------|------:|------:|------:|
| TCP 1-stream | 1,000 Mbps | 1,690 Mbps | **1.69x** |
| TCP 4-stream | 1,180 Mbps | 1,670 Mbps | **1.42x** |
| Retransmits (1-stream) | 6,391 | 799 | **8x fewer** |

## Phase 0 Latency Results (100 pings, 10ms interval)

| Metric | Go derper (TS) | Hyper-DERP (HD) |
|--------|------:|------:|
| Min | 0.213 ms | 0.185 ms |
| Avg | 0.648 ms | 0.363 ms |
| Max | 0.922 ms | 0.721 ms |

## Key Takeaway

The relay advantage **translates to real end-to-end client
experience**. HD delivers 1.7x throughput, 1.8x lower latency,
and 8x fewer retransmits through real Tailscale WireGuard tunnels.
This is not just a synthetic relay benchmark — it's the actual
user-perceived difference.

Phase 1 will add statistical rigor (10 runs per config, CV%,
confidence intervals).
