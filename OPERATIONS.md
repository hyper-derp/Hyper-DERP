# Operations Guide

## System Requirements

- Linux kernel 6.1+ (SINGLE_ISSUER, DEFER_TASKRUN,
  multishot recv, provided buffer rings)
- liburing 2.3+, libsodium 1.0.18+, OpenSSL 3.x

## System Tuning

### Sysctl Settings

```sh
# Increase socket buffer limits (required for --sockbuf)
sysctl -w net.core.rmem_max=16777216
sysctl -w net.core.wmem_max=16777216

# Increase connection backlog
sysctl -w net.core.somaxconn=8192

# Increase file descriptor limit
sysctl -w fs.file-max=1048576

# TCP tuning
sysctl -w net.ipv4.tcp_max_syn_backlog=8192
sysctl -w net.core.netdev_max_backlog=8192
```

Persist in `/etc/sysctl.d/99-hyper-derp.conf`.

### File Descriptor Limits

Each peer uses one fd. With 10,000 peers across 8 workers:

```sh
# /etc/security/limits.d/hyper-derp.conf
*  soft  nofile  65536
*  hard  nofile  65536
```

Or in the systemd unit:
```ini
[Service]
LimitNOFILE=65536
```

### CPU Pinning

For maximum throughput, pin workers to dedicated cores and
isolate them from the scheduler:

```sh
# Isolate cores 2-9 from general scheduling
# Add to kernel cmdline: isolcpus=2-9

# Pin 8 workers to isolated cores
hyper-derp --workers 8 --pin-workers 2,3,4,5,6,7,8,9
```

## Memory Footprint

Per-worker memory (approximate):

| Component | Size | Notes |
|-----------|-----:|-------|
| Peer hash table | 335 MB | 4096 slots × ~82 KB each |
| Route table | 544 KB | 4096 × 136 B |
| fd_map + fd_gen + notif_map | 1.5 MB | 64K entries |
| Slab allocator | 1 MB | 65536 × 16 B |
| Provided buffers | 33 MB | 512 × 65 KB |
| io_uring ring | ~1 MB | 4096 SQ/CQ depth |
| **Total per worker** | **~371 MB** | |

With 8 workers: ~3 GB baseline. The peer hash table
dominates because each Peer struct contains a 65 KB read
buffer for frame reassembly.

## Configuration

### Systemd

Edit `/etc/hyper-derp/hyper-derp.yaml`:

```yaml
port: 3340
workers: 8
pin_cores: [2, 3, 4, 5, 6, 7, 8, 9]
sockbuf: 4194304
max_accept_rate: 1000
log_level: info
metrics:
  port: 9090
```

Then: `systemctl restart hyper-derp`

See [docs/configuration.md](docs/configuration.md) for
all options and the YAML config format.

### Metrics

Prometheus scrape config:

```yaml
scrape_configs:
  - job_name: hyper-derp
    static_configs:
      - targets: ['relay:9090']
    scrape_interval: 15s
```

### Available Metrics

| Metric | Type | Description |
|--------|------|-------------|
| `hyper_derp_recv_bytes_total` | counter | Bytes received |
| `hyper_derp_send_bytes_total` | counter | Bytes sent |
| `hyper_derp_send_drops_total` | counter | Send queue full drops |
| `hyper_derp_xfer_drops_total` | counter | Cross-shard ring drops |
| `hyper_derp_slab_exhausts_total` | counter | SendItem slab exhaustions |
| `hyper_derp_send_errors_total` | counter | By reason: epipe, econnreset, eagain, other |
| `hyper_derp_recv_enobufs_total` | counter | Provided buffer exhaustions |
| `hyper_derp_frame_pool_hits_total` | counter | Frame pool allocs from pool |
| `hyper_derp_frame_pool_misses_total` | counter | Frame pool allocs via malloc |
| `hyper_derp_peers_active` | gauge | Connected peers |
| `hyper_derp_workers` | gauge | Worker thread count |

Also available: `/health` (JSON health check),
`/debug/workers` and `/debug/peers` (require
`--debug-endpoints`). See
[docs/configuration.md](docs/configuration.md#endpoints)
for response formats.

### Alerting Recommendations

```yaml
# Alert if buffer pool is exhausting
- alert: DerpBufferExhaustion
  expr: rate(hyper_derp_recv_enobufs_total[5m]) > 0
  labels:
    severity: warning
  annotations:
    summary: "Provided buffer pool under pressure"

# Alert if cross-shard drops are occurring
- alert: DerpXferDrops
  expr: rate(hyper_derp_xfer_drops_total[5m]) > 0
  labels:
    severity: critical
  annotations:
    summary: "Cross-shard messages being dropped"

# Alert if send errors spike
- alert: DerpSendErrors
  expr: rate(hyper_derp_send_errors_total[5m]) > 10
  labels:
    severity: warning
  annotations:
    summary: "Elevated send error rate"
```

## Troubleshooting

### High recv_enobufs

The provided buffer pool (512 buffers by default) is being
exhausted faster than buffers are returned. Causes:
- Too many peers on one worker (unbalanced key hashing)
- Recv processing slower than ingress rate

Fix: Increase `kPbufCount` in `types.h` and rebuild. Or
reduce per-worker peer count by adding more workers.

### High xfer_drops

The cross-shard SPSC ring (16K entries) is full. Causes:
- Burst of cross-shard traffic (many peers sending to peers
  on different workers)
- Consumer worker is busy with local I/O

Fix: Increase `kXferRingSize` in `types.h`. Consider
whether peer-to-worker assignment can be improved.

### High slab_exhausts

The per-worker SendItem slab (64K nodes) is exhausted.
Causes:
- Sustained high send rate with slow consumers
- Many peers with deep send queues

Fix: Increase `kSlabSize` in `types.h`. The slab falls
back to malloc when exhausted, but malloc in the hot path
hurts latency.

### SEND_ZC EOPNOTSUPP

The kernel or socket type doesn't support zero-copy sends.
The worker automatically falls back to regular send for that
peer (sets `no_zc` flag). This is normal for AF_UNIX
sockets or older kernels.

## Contact

- **Security vulnerabilities:** security@hyper-derp.dev
- **Release and packaging:** release@hyper-derp.dev
