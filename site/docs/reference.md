---
title: CLI & Config Reference
description: >-
  Complete reference for all CLI flags, config file options,
  and compile-time constants.
sidebar_position: 6
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

## Config File Options

YAML format, parsed with rapidyaml. Pass with `--config <path>`.

### port

DERP relay listen port.

| | |
|---|---|
| CLI | `--port <N>` |
| YAML | `port` |
| Default | `3340` |
| Range | 1–65535 |

### workers

Number of io_uring worker threads. Each worker owns a disjoint set of peer connections and runs its own event loop.

| | |
|---|---|
| CLI | `--workers <N>` |
| YAML | `workers` |
| Default | `0` (auto: one per CPU) |
| Range | 0–32 |

### pin_cores

Pin each worker thread to a specific CPU core.

| | |
|---|---|
| CLI | `--pin-workers "0,2,4,6"` |
| YAML | `pin_cores: [0, 2, 4, 6]` or `pin_cores: "0,2,4,6"` |
| Default | not pinned |
| Range | core IDs 0–1023 |

The number of cores listed must match the worker count.

### sockbuf

Per-socket send and recv buffer size. Sets both `SO_RCVBUF` and `SO_SNDBUF`.

| | |
|---|---|
| CLI | `--sockbuf <bytes>` |
| YAML | `sockbuf` |
| Default | `0` (OS default, typically 128–256 KB) |
| Range | 0–268435456 (256 MB) |

### max_accept_rate

Rate limit on new connection accepts per second.

| | |
|---|---|
| CLI | `--max-accept-rate <N>` |
| YAML | `max_accept_rate` |
| Default | `0` (unlimited) |
| Range | 0–1000000 |

### tls_cert / tls_key

TLS certificate and private key for DERP connections. Both must be set to enable TLS.

| | |
|---|---|
| CLI | `--tls-cert <path>` / `--tls-key <path>` |
| YAML | `tls_cert` / `tls_key` |
| Default | empty (no TLS) |

### sqpoll

Enable io_uring SQPOLL mode.

| | |
|---|---|
| CLI | `--sqpoll` |
| YAML | `sqpoll: true` |
| Default | `false` |

Requires `CAP_SYS_NICE` or root.

### log_level

Controls log verbosity.

| | |
|---|---|
| CLI | `--log-level <level>` |
| YAML | `log_level` |
| Default | `info` |
| Values | `debug`, `info`, `warn`, `error` |

### metrics.port

Prometheus metrics HTTP server port.

| | |
|---|---|
| CLI | `--metrics-port <N>` |
| YAML | `metrics.port` |
| Default | `0` (disabled) |

### metrics.tls_cert / metrics.tls_key

Separate TLS certificate for the metrics endpoint.

| | |
|---|---|
| YAML | `metrics.tls_cert` / `metrics.tls_key` |
| Default | empty (plain HTTP) |

### metrics.debug_endpoints

Expose `/debug/*` endpoints on the metrics server. These show per-peer state including public keys.

| | |
|---|---|
| CLI | `--debug-endpoints` |
| YAML | `metrics.debug_endpoints: true` |
| Default | `false` |

## Compile-Time Constants

These values are set in `include/hyper_derp/types.h` and cannot be changed at runtime.

### io_uring

| Constant | Value | Purpose |
|----------|------:|---------|
| `kUringQueueDepth` | 4096 | Submission queue depth per worker |
| `kMaxCqeBatch` | 256 | Max CQEs processed per iteration |
| `kBusySpinDefault` | 256 | Spin iterations before blocking (>2 workers) |
| `kBusySpinLowWorker` | 64 | Spin iterations for 1–2 workers |

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
- `SendPressureLow(peers) = High / N` where N is 8 for 1–2 workers, 6 for 3–4 workers, 4 for 5+ workers

See [troubleshooting: 4 vCPU stall](/docs/troubleshooting/#high-latency-spikes-at-4-vcpu) for details on the backpressure mechanism.
