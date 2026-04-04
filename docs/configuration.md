# Configuration

## Config File

Hyper-DERP reads a YAML config file via `--config` and
accepts CLI flag overrides. CLI flags take precedence.

```sh
hyper-derp --config /etc/hyper-derp/hyper-derp.yaml
hyper-derp --config /etc/hyper-derp/hyper-derp.yaml --port 443
```

Example config (`dist/hyper-derp.yaml`):

```yaml
port: 3340
workers: 0
# pin_cores: [0, 2, 4, 6]
sqpoll: false
# tls_cert: /etc/hyper-derp/cert.pem
# tls_key: /etc/hyper-derp/key.pem
log_level: info
metrics:
  # port: 9100
  debug_endpoints: false
```

## CLI Flags

CLI flags override config file values. If no `--config` is
given, defaults are used.

| Flag | Default | Description |
|------|--------:|-------------|
| `--config` | none | YAML config file path |
| `--port` | 3340 | Listen port |
| `--workers` | 0 (auto) | Worker count. 0 = auto |
| `--pin-workers` | none | Pin to cores (e.g. `0,2,4`) |
| `--sockbuf` | 0 (OS) | Socket buffer size (bytes) |
| `--max-accept-rate` | 0 | Max accepts/sec (0=off) |
| `--tls-cert` | none | PEM cert (enables kTLS) |
| `--tls-key` | none | PEM key (with --tls-cert) |
| `--metrics-port` | 0 | Prometheus HTTP port (0=off) |
| `--debug-endpoints` | off | Enable /debug/* endpoints |
| `--sqpoll` | off | io_uring SQPOLL mode |
| `--log-level` | info | debug, info, warn, error |
| `--help` | -- | Show usage |
| `--version` | -- | Show version |

### Details

- `--workers 0` auto-detects via
  `sysconf(_SC_NPROCESSORS_ONLN)`. If `--pin-workers`
  is set without `--workers`, the worker count is
  inferred from the pin list length (`main.cc:220`).
- `--pin-workers` accepts a comma-separated core list
  (e.g. `0,2,4,6`).
- `--sockbuf` sets both SO_SNDBUF and SO_RCVBUF per
  socket. Values above `net.core.wmem_max` /
  `net.core.rmem_max` are silently capped by the kernel.
- `--max-accept-rate` range: 0-1000000. 0 = unlimited.
- `--tls-cert` and `--tls-key` must both be specified or
  both omitted (`main.cc:191`). When set, kTLS is probed
  at startup. If the kernel TLS module is not loaded,
  the server exits with an error (`server.cc:372`).
- `--sqpoll` requires `CAP_SYS_NICE` (granted by the
  systemd unit's `AmbientCapabilities`). The kernel
  polls the SQ on a dedicated thread, eliminating
  `io_uring_enter` syscalls.
- `--debug-endpoints` exposes peer keys via `/debug/*`
  HTTP endpoints. Do not enable in production without
  network-level access control.

## Systemd Configuration

The installed systemd unit (`dist/hyper-derp.service`)
uses a YAML config file:

```ini
ExecStart=/usr/bin/hyper-derp --config /etc/hyper-derp/hyper-derp.yaml
```

Edit the config and restart:

```sh
sudo systemctl daemon-reload
sudo systemctl restart hyper-derp
```

### Systemd Hardening

The unit (`dist/hyper-derp.service`) includes security
hardening:

| Directive | Value | Purpose |
|-----------|-------|---------|
| `DynamicUser` | yes | Runs as ephemeral user |
| `ProtectSystem` | strict | Read-only filesystem |
| `ProtectHome` | yes | No access to /home |
| `PrivateTmp` | yes | Isolated /tmp |
| `NoNewPrivileges` | yes | No privilege escalation |
| `MemoryDenyWriteExecute` | yes | No W+X pages |
| `AmbientCapabilities` | `CAP_SYS_NICE` | For SQPOLL |
| `LimitMEMLOCK` | infinity | For io_uring buffers |
| `SystemCallFilter` | `@system-service` + io_uring | Restricted syscalls |
| `RestrictAddressFamilies` | `AF_INET AF_INET6 AF_UNIX` | No raw sockets |

## Worker Count Guidance

- **With kTLS (production):** use default (`--workers 0`
  or vCPU / 2). More workers means more parallel crypto
  throughput. 8 workers on 16 vCPU outperforms 4 workers
  under kTLS.
- **Without TLS (testing only):** cap at 4 workers.

## kTLS Prerequisites

The kernel TLS module must be loaded before starting:

```sh
sudo modprobe tls
lsmod | grep tls
```

HD auto-detects kTLS support via OpenSSL at startup
(`server.cc:372`, `ProbeKtls`). The probe creates a
loopback socket pair and attempts to install the TLS ULP
(`ktls.h:59`). Without the module loaded, the server
exits with `KtlsInitFailed`.

After a successful TLS 1.3 handshake, OpenSSL 3.x
auto-installs kTLS for both TX and RX. The fd is detached
from the SSL object and used directly with io_uring.
`read()`/`write()` operate on plaintext; AES-GCM runs in
the kernel.

## System Tuning

For detailed sysctl settings, file descriptor limits,
CPU pinning, and memory footprint analysis, see
[OPERATIONS.md](../OPERATIONS.md).

Key sysctls for production:

```sh
# Socket buffer limits (required for --sockbuf)
net.core.rmem_max = 16777216
net.core.wmem_max = 16777216

# Connection backlog
net.core.somaxconn = 8192

# File descriptor limit
fs.file-max = 1048576
```

## Metrics

### Enabling Metrics

```sh
hyper-derp --metrics-port 9090
```

The metrics endpoint serves Prometheus-format metrics on
a plain HTTP port (no TLS). This is intentional -- the
metrics port is for internal monitoring.

Add `--debug-endpoints` to enable `/debug/*` endpoints.
These expose per-peer keys and per-worker state. Do not
enable in production without network-level access control.

### Prometheus Scrape Config

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
| `hyper_derp_recv_bytes_total` | counter | Total bytes received |
| `hyper_derp_send_bytes_total` | counter | Total bytes sent |
| `hyper_derp_send_drops_total` | counter | Send failures (queue full) |
| `hyper_derp_xfer_drops_total` | counter | Cross-shard ring drops |
| `hyper_derp_slab_exhausts_total` | counter | Slab pool exhaustions |
| `hyper_derp_send_errors_total` | counter | Send errors (by reason label) |
| `hyper_derp_recv_enobufs_total` | counter | Provided buffer exhaustions |
| `hyper_derp_peers_active` | gauge | Currently connected peers |
| `hyper_derp_workers` | gauge | Worker thread count |

Per-worker stats are defined in `types.h:204`
(`WorkerStats`) and aggregated by `DpGetStats`
(`data_plane.h:60`).

### Alerting Recommendations

```yaml
# Buffer pool pressure
- alert: DerpBufferExhaustion
  expr: rate(hyper_derp_recv_enobufs_total[5m]) > 0
  labels:
    severity: warning

# Cross-shard drops (critical - messages lost)
- alert: DerpXferDrops
  expr: rate(hyper_derp_xfer_drops_total[5m]) > 0
  labels:
    severity: critical

# Elevated send errors
- alert: DerpSendErrors
  expr: rate(hyper_derp_send_errors_total[5m]) > 10
  labels:
    severity: warning
```

## Memory Footprint

Per-worker memory (approximate):

| Component | Size | Notes |
|-----------|-----:|-------|
| Peer hash table | 335 MB | `kHtCapacity` (4096) slots |
| Route table | 544 KB | 4096 x 136 B |
| fd_map + fd_gen + notif_map | 1.5 MB | `kMaxFd` (65536) entries |
| Slab allocator | 1 MB | `kSlabSize` (65536) x 16 B |
| Provided buffers | 33 MB | `kPbufCount` (512) x 65 KB |
| Frame pool | 32 MB | `kFramePoolCount` (16384) x 2 KB |
| io_uring ring | ~1 MB | `kUringQueueDepth` (4096) |
| **Total per worker** | **~403 MB** | |

With 8 workers: ~3.2 GB baseline. The peer hash table
dominates because each `Peer` struct (`types.h:152`)
contains a heap-allocated 65 KB read buffer for frame
reassembly. The hot struct itself is ~96 bytes; the read
buffer is allocated separately to keep hash table probing
cache-friendly.
