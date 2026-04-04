---
title: Configuration
description: >-
  Complete reference for all CLI flags, config file options,
  and tuning parameters.
sidebar_position: 2
---

## CLI Flags

```
hyper-derp [OPTIONS]

  --config <path>          YAML config file path
  --port <N>               Listen port (default: 3340)
  --workers <N>            Worker threads, 0=auto (default: 0)
  --pin-workers <list>     Pin workers to CPUs (e.g. "0,2,4,6")
  --sockbuf <bytes>        Per-socket send/recv buffer (0=OS default)
  --max-accept-rate <N>    Max accepts/sec (0=unlimited)
  --tls-cert <path>        TLS certificate file
  --tls-key <path>         TLS private key file
  --metrics-port <N>       Metrics HTTP port (0=disabled)
  --sqpoll                 Enable io_uring SQPOLL mode
  --debug-endpoints        Enable /debug/* metrics endpoints
  --log-level <level>      debug, info, warn, or error
  --version                Print version and exit
  --help                   Print help and exit
```

CLI flags override config file values.

## Config File

YAML format, parsed with rapidyaml. Pass with `--config <path>`.

```yaml
# Listen port (1-65535)
port: 3340

# Worker threads. 0 = one per CPU core. Max 32.
workers: 0

# Pin workers to specific CPU cores.
# Array or comma-separated string.
# pin_cores: [0, 2, 4, 6]
# pin_cores: "0,2,4,6"

# Per-socket SO_RCVBUF / SO_SNDBUF in bytes.
# 0 = OS default. Max 256 MB.
# sockbuf: 2097152

# Maximum accepted connections per second.
# 0 = unlimited. Max 1,000,000.
# max_accept_rate: 0

# Enable io_uring SQPOLL mode.
# Requires CAP_SYS_NICE or root.
sqpoll: false

# TLS certificate and key for DERP connections.
# Both must be set to enable kTLS.
# tls_cert: /etc/hyper-derp/cert.pem
# tls_key: /etc/hyper-derp/key.pem

# Log level: debug, info, warn, error
log_level: info

# Metrics HTTP server
metrics:
  # Port to listen on. 0 = disabled.
  # port: 9090

  # Separate TLS for metrics endpoint (optional).
  # tls_cert: /etc/hyper-derp/metrics-cert.pem
  # tls_key: /etc/hyper-derp/metrics-key.pem

  # Expose /debug/* endpoints.
  # WARNING: exposes peer public keys.
  debug_endpoints: false
```

## Option Reference

### port

DERP relay listen port. Clients connect here over TLS.

| | |
|---|---|
| CLI | `--port <N>` |
| YAML | `port` |
| Default | `3340` |
| Range | 1--65535 |

### workers

Number of io_uring worker threads. Each worker owns a disjoint set of peer connections and runs its own event loop.

| | |
|---|---|
| CLI | `--workers <N>` |
| YAML | `workers` |
| Default | `0` (auto: one per CPU) |
| Range | 0--32 |

Setting `0` uses all available CPUs. For kTLS workloads, the optimal count is usually the physical core count -- hyperthreads add little because the bottleneck is memory bandwidth and kernel crypto.

Benchmark data for optimal worker counts:

| vCPU | Optimal workers | Peak throughput |
|-----:|----------------:|----------------:|
| 4 | 2 | 6,091 Mbps |
| 8 | 4 | 12,316 Mbps |
| 16 | 8--10 | 16,545 Mbps |

See [benchmarks: worker optimization](/benchmarks/peer-scaling/#worker-optimization) for full sweep data.

### pin_cores

Pin each worker thread to a specific CPU core. Reduces cache thrashing from thread migration.

| | |
|---|---|
| CLI | `--pin-workers "0,2,4,6"` |
| YAML | `pin_cores: [0, 2, 4, 6]` or `pin_cores: "0,2,4,6"` |
| Default | not pinned |
| Range | core IDs 0--1023 |

The number of cores listed must match the worker count. If `workers: 4`, provide exactly 4 core IDs.

For kTLS, pin workers to physical cores (skip hyperthreads). On a 4-core/8-thread CPU:

```yaml
workers: 4
pin_cores: [0, 1, 2, 3]
```

### sockbuf

Per-socket send and recv buffer size. Sets both `SO_RCVBUF` and `SO_SNDBUF`.

| | |
|---|---|
| CLI | `--sockbuf <bytes>` |
| YAML | `sockbuf` |
| Default | `0` (OS default, typically 128--256 KB) |
| Range | 0--268435456 (256 MB) |

Larger buffers absorb traffic bursts but increase per-connection memory. For high-throughput deployments, also raise the system maximum:

```bash
sysctl -w net.core.rmem_max=4194304
sysctl -w net.core.wmem_max=4194304
```

### max_accept_rate

Rate limit on new connection accepts per second. Protects against connection floods.

| | |
|---|---|
| CLI | `--max-accept-rate <N>` |
| YAML | `max_accept_rate` |
| Default | `0` (unlimited) |
| Range | 0--1000000 |

### tls_cert / tls_key

TLS certificate and private key for DERP connections. Both must be set to enable TLS. When set, HD performs the TLS handshake in userspace (OpenSSL) then installs session keys in the kernel for kTLS offload.

| | |
|---|---|
| CLI | `--tls-cert <path>` / `--tls-key <path>` |
| YAML | `tls_cert` / `tls_key` |
| Default | empty (no TLS) |

Without TLS, HD accepts plain TCP DERP connections. This is only useful for testing or when TLS is terminated by a reverse proxy.

kTLS requires kernel 5.19+ with `CONFIG_TLS` enabled. Verify with:

```bash
modprobe tls
cat /proc/net/tls_stat
```

### sqpoll

Enable io_uring SQPOLL mode. A kernel thread polls the submission queue, eliminating syscall overhead for SQE submission.

| | |
|---|---|
| CLI | `--sqpoll` |
| YAML | `sqpoll: true` |
| Default | `false` |

Requires `CAP_SYS_NICE` or root. Costs one busy-spinning kernel thread per worker. Reduces latency at the cost of CPU.

### metrics

Prometheus-format metrics HTTP server.

| | |
|---|---|
| YAML | `metrics.port` |
| Default | `0` (disabled) |
| CLI | `--metrics-port <N>` |

Set a port to enable. The metrics endpoint serves at `http://host:port/metrics`.

Optionally, the metrics server can use its own TLS certificate (separate from the DERP TLS cert):

```yaml
metrics:
  port: 9090
  tls_cert: /etc/hyper-derp/metrics-cert.pem
  tls_key: /etc/hyper-derp/metrics-key.pem
```

### debug_endpoints

Expose `/debug/*` endpoints on the metrics server. These show per-peer state including public keys.

| | |
|---|---|
| CLI | `--debug-endpoints` |
| YAML | `metrics.debug_endpoints: true` |
| Default | `false` |

Do not enable in production -- it exposes peer public keys.

### log_level

Controls log verbosity.

| | |
|---|---|
| CLI | `--log-level <level>` |
| YAML | `log_level` |
| Default | `info` |
| Values | `debug`, `info`, `warn`, `error` |

## Compile-Time Constants

These values are set in `include/hyper_derp/types.h` and cannot be changed at runtime. They affect performance characteristics and resource usage.

### io_uring

| Constant | Value | Purpose |
|----------|------:|---------|
| `kUringQueueDepth` | 4096 | Submission queue depth per worker |
| `kMaxCqeBatch` | 256 | Max CQEs processed per iteration |
| `kBusySpinDefault` | 256 | Spin iterations before blocking (>2 workers) |
| `kBusySpinLowWorker` | 64 | Spin iterations for 1--2 workers |

### Memory

| Constant | Value | Purpose |
|----------|------:|---------|
| `kSlabSize` | 65536 | Slab allocator pool size per worker |
| `kPbufCount` | 512 | Provided buffers per worker |
| `kPbufSize` | ~65541 | Provided buffer size (frame header + max payload) |
| `kFramePoolCount` | 16384 | Frame pool buffers per worker (32 MiB) |
| `kFramePoolBufSize` | 2048 | Frame pool buffer size |

### Networking

| Constant | Value | Purpose |
|----------|------:|---------|
| `kMaxFd` | 65536 | Maximum tracked file descriptors |
| `kHtCapacity` | 4096 | Peer hash table capacity (power of 2) |
| `kMaxSendsInflight` | 16 | Max concurrent sends per peer |
| `kMaxSendQueueDepth` | 2048 | Max queued sends per peer |
| `kPollWriteBatch` | 16 | Sends submitted per POLLOUT |

### Backpressure

| Constant | Value | Purpose |
|----------|------:|---------|
| `kSendPressureMax` | 32768 | Absolute send pressure cap |
| `kPressurePerPeer` | 512 | Per-peer pressure contribution |
| `kRecvPauseMinBatches` | 8 | Min batches recv stays paused |
| `kRecvBudget` | 512 | Max concurrent recv SQEs per worker |
| `kRecvDeferSize` | 4096 | Deferred recv ring size |
| `kXferSpscSize` | 16384 | Cross-worker SPSC ring size |
| `kCmdRingSize` | 1024 | Command ring size |

The adaptive send pressure threshold is computed from peer count:
- `SendPressureHigh(peers) = peers * 512` (clamped to 32768)
- `SendPressureLow(peers) = High / N` where N is 8 for 1--2 workers, 6 for 3--4 workers, 4 for 5+ workers

See [troubleshooting: 4 vCPU stall](/docs/troubleshooting/#high-latency-spikes-at-4-vcpu) for details on the backpressure mechanism.

## Example Configs

### Minimal (testing)

```yaml
port: 3340
log_level: debug
```

No TLS, auto workers, no metrics. Accepts plain TCP.

### Production

```yaml
port: 443
workers: 8
pin_cores: [0, 1, 2, 3, 4, 5, 6, 7]
sockbuf: 2097152

tls_cert: /etc/hyper-derp/cert.pem
tls_key: /etc/hyper-derp/key.pem

log_level: info

metrics:
  port: 9090
```

### High-throughput relay

```yaml
port: 443
workers: 10
pin_cores: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
sockbuf: 4194304
sqpoll: true

tls_cert: /etc/hyper-derp/cert.pem
tls_key: /etc/hyper-derp/key.pem

log_level: warn

metrics:
  port: 9090
```

Requires `CAP_SYS_NICE` for SQPOLL. Pair with kernel tuning from the [operations guide](/docs/operations/#kernel-tuning).
