---
title: Configuration
description: >-
  How to configure Hyper-DERP for common deployment scenarios.
sidebar_position: 2
---

Hyper-DERP reads a YAML config file and accepts CLI flag overrides. CLI flags take precedence over file values.

```sh
hyper-derp --config /etc/hyper-derp/hyper-derp.yaml
```

## TLS / kTLS

Both `tls_cert` and `tls_key` must be set to enable TLS. HD performs the handshake in userspace (OpenSSL) then installs the session keys in the kernel for kTLS offload.

```yaml
tls_cert: /etc/hyper-derp/cert.pem
tls_key: /etc/hyper-derp/key.pem
```

kTLS requires kernel 5.19+ with the `tls` module loaded:

```bash
sudo modprobe tls
# Persist across reboots:
echo tls | sudo tee /etc/modules-load.d/tls.conf
```

Verify kTLS is active after starting HD:

```bash
cat /proc/net/tls_stat
```

Without TLS, HD accepts plain TCP DERP connections. This is only useful for testing or when TLS is terminated by a reverse proxy.

## Workers

Each worker owns a disjoint set of peer connections and runs its own io_uring event loop. Default is one per CPU core.

```yaml
workers: 4
```

For kTLS, the optimal count is usually the physical core count — hyperthreads add little because the bottleneck is memory bandwidth and kernel crypto.

| vCPU | Optimal workers | Peak throughput |
|-----:|----------------:|----------------:|
| 4 | 2 | 6,091 Mbps |
| 8 | 4 | 12,316 Mbps |
| 16 | 8–10 | 16,545 Mbps |

### CPU Pinning

Pin workers to physical cores to reduce cache thrashing from thread migration. The number of core IDs must match the worker count.

```yaml
workers: 4
pin_cores: [0, 1, 2, 3]
```

On a machine with hyperthreading, pin to physical cores only (skip the hyperthread siblings).

## Metrics

Enable the Prometheus metrics endpoint by setting a port:

```yaml
metrics:
  port: 9090
```

This serves `/metrics` (Prometheus), `/health` (JSON), and optionally `/debug/workers` and `/debug/peers`.

The metrics server can use its own TLS certificate, separate from the DERP cert:

```yaml
metrics:
  port: 9090
  tls_cert: /etc/hyper-derp/metrics-cert.pem
  tls_key: /etc/hyper-derp/metrics-key.pem
```

### Debug Endpoints

`/debug/*` endpoints expose per-peer state including public keys. Do not enable in production.

```yaml
metrics:
  port: 9090
  debug_endpoints: true
```

## Socket Buffers

Larger buffers absorb traffic bursts but increase per-connection memory. For high-throughput deployments, raise both the HD setting and the system maximum:

```yaml
sockbuf: 2097152
```

```bash
sysctl -w net.core.rmem_max=4194304
sysctl -w net.core.wmem_max=4194304
```

## SQPOLL

io_uring SQPOLL mode uses a kernel thread to poll the submission queue, eliminating syscall overhead. Costs one busy-spinning kernel thread per worker.

```yaml
sqpoll: true
```

Requires `CAP_SYS_NICE` or root. The packaged systemd unit already grants `CAP_SYS_NICE`.

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

Pair with kernel tuning from the [operations guide](/docs/operations/#kernel-tuning).

## Full Reference

See [CLI & Config Reference](/docs/reference/) for every option, its type, default, range, and the full list of compile-time constants.
