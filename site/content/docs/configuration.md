---
title: "Configuration"
description: "CLI flags, systemd setup, and tuning."
weight: 3
---

All configuration is via command-line flags. The systemd unit
passes flags through an `EnvironmentFile`.

## CLI Flags

| Flag | Default | Description |
|------|--------:|-------------|
| `--port` | 3340 | Listen port |
| `--workers` | 0 (auto) | Worker count (0 = auto) |
| `--pin-workers` | none | Pin to cores (e.g. `0,2,4`) |
| `--sockbuf` | 0 (OS) | Socket buffer size (bytes) |
| `--max-accept-rate` | 0 | Max accepts/sec (0 = off) |
| `--tls-cert` | none | PEM cert (enables kTLS) |
| `--tls-key` | none | PEM key (with `--tls-cert`) |
| `--metrics-port` | 0 | Prometheus HTTP port (0 = off) |
| `--debug-endpoints` | off | Enable `/debug/*` endpoints |
| `--sqpoll` | off | io_uring SQPOLL mode |
| `--log-level` | info | debug, info, warn, error |

## Worker Count Guidance

- **With kTLS (production):** use default (vCPU / 2). More
  workers means more parallel crypto throughput.
- **Without TLS (testing only):** cap at 4 workers.

## Systemd Configuration

The installed unit reads options from:

```
/etc/hyper-derp/hyper-derp.conf
```

See
[OPERATIONS.md](https://github.com/hyper-derp/hyper-derp/blob/main/OPERATIONS.md)
for sysctl settings, CPU pinning, memory footprint, and
Prometheus alerting.

## Detailed Reference

The full configuration reference with flag details and
source file pointers is in the repository at
[docs/configuration.md](https://github.com/hyper-derp/hyper-derp/blob/main/docs/configuration.md).
