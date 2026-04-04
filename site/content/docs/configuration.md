---
title: "Configuration"
description: >-
  CLI flags, config file format, worker tuning, socket
  buffer sizing, SQPOLL mode.
weight: 3
draft: true
---

## CLI Flags

```
hyper-derp [OPTIONS]

  -c, --config <path>     Config file path
  -l, --listen <addr>     Listen address (default: :443)
  -w, --workers <N>       Worker count (default: auto)
  --mesh-key <key>        Mesh key for multi-server setup
  --certdir <path>        TLS certificate directory
  --hostname <name>       Server hostname for TLS SNI
  --stun-port <port>      STUN port (default: 3478)
  --metrics-addr <addr>   Metrics endpoint (default: :9090)
  --no-ktls               Disable kernel TLS offload
  --sqpoll                Enable SQPOLL mode
  --verbose               Verbose logging
```

## Config File

YAML format (rapidyaml). CLI flags override config file
values.

```yaml
port: 3340
workers: 4
# pin_cores: [0, 2, 4, 6]
sqpoll: false

# kTLS -- both required to enable
# tls_cert: /etc/hyper-derp/cert.pem
# tls_key: /etc/hyper-derp/key.pem

log_level: info

metrics:
  # port: 9100
  debug_endpoints: false
```

## Worker Count Tuning

Default: one worker per available CPU. For kTLS workloads,
the optimal count usually matches the physical core count.
Hyperthreads add little benefit since the bottleneck is
memory bandwidth and kernel TLS.

## Socket Buffer Sizing

The `rcvbuf` / `sndbuf` values set `SO_RCVBUF` /
`SO_SNDBUF` on each client socket. Larger buffers help
with bursty traffic but increase per-connection memory.

For high-throughput deployments, also set the system max:

```bash
sysctl -w net.core.rmem_max=4194304
sysctl -w net.core.wmem_max=4194304
```

## SQPOLL Mode

With `--sqpoll`, io_uring runs a kernel thread that polls
the submission queue, eliminating syscall overhead for
submits. Requires `CAP_SYS_NICE` or running as root.
Reduces latency at the cost of one busy-spinning kernel
thread per worker.
