---
title: Troubleshooting
description: Common issues and fixes for Hyper-DERP.
sidebar_position: 6
---

## HD won't start

### "io_uring_setup: Operation not permitted"

The kernel denies io_uring access. This happens on older kernels or when io_uring is restricted by seccomp or `io_uring_disabled` sysctl.

```bash
# Check if io_uring is disabled
cat /proc/sys/kernel/io_uring_disabled
# 0 = allowed for all, 1 = restricted, 2 = disabled
```

**Fix**: Requires kernel 5.11+. On kernels 6.1+ where `io_uring_disabled` exists, set it to 0:

```bash
sysctl -w kernel.io_uring_disabled=0
```

If running in a container, the container runtime must allow io_uring syscalls. Docker's default seccomp profile blocks io_uring on older versions.

### "kTLS not available"

HD logs this when the kernel doesn't support TLS offload. HD will still work -- it falls back to userspace TLS via OpenSSL. Performance will be lower.

**Fix**: Requires kernel 5.19+ with `CONFIG_TLS` and `CONFIG_TLS_DEVICE` enabled. Load the module:

```bash
sudo modprobe tls
cat /proc/modules | grep tls
```

Verify kTLS is active after starting HD:

```bash
cat /proc/net/tls_stat
# TlsCurrTxSw and TlsCurrRxSw should be > 0
```

### "bind: Address already in use"

Another process is listening on the same port.

```bash
ss -tlnp | grep 3340
```

**Fix**: Stop the other process, or change HD's port in the config file.

### "failed to open certificate"

HD can't read the TLS certificate or key file.

**Fix**: Check paths and permissions:

```bash
ls -la /etc/hyper-derp/cert.pem /etc/hyper-derp/key.pem
# Must be readable by the HD process user
```

## Performance Issues

### Low throughput compared to benchmarks

The published benchmarks use 4 distributed client VMs on GCP. Common reasons for lower numbers in your setup:

1. **Single client bottleneck.** One client process can't generate enough traffic to saturate HD. Use multiple client machines or processes.

2. **Network is the limit.** Check your NIC bandwidth:
   ```bash
   iperf3 -c <relay-ip> -t 10
   ```
   If this is lower than your expected relay throughput, the network is the bottleneck.

3. **kTLS not active.** Check `/proc/net/tls_stat`. Without kTLS, HD uses userspace TLS and loses a significant amount of throughput.

4. **Wrong worker count.** Default is one per CPU, but the optimal count depends on your workload. See [Configuration: Worker Count Tuning](/docs/configuration/#worker-count-tuning).

### High latency spikes at 4 vCPU

HD has a known backpressure oscillation at 4 vCPU (2 workers) under sustained load above 50% of the relay ceiling. The p99 latency can spike to several milliseconds.

**Workaround**: Use 4 workers on 4 vCPU (`--workers 4`). This reduces the per-worker load and prevents the oscillation. The throughput penalty is small.

**Status**: A fix (wider hysteresis, minimum pause duration, reduced busy-spin) is in development. See the [latency benchmark analysis](/benchmarks/latency/) for details.

### High memory usage

Memory scales with worker count and buffer ring size. Each worker allocates a provided buffer ring at startup.

Check the current configuration:

```bash
# Default buffer ring: 1024 buffers x 65536 bytes = 64 MB per worker
# 8 workers = 512 MB just for buffer rings
```

**Fix**: Reduce `buf_ring_size` or `buf_size` in the config if your traffic doesn't need large buffers. For WireGuard traffic (1400-byte frames), `buf_size: 4096` is sufficient.

## Tailscale/Headscale Integration

### Clients won't connect to HD

HD speaks the DERP protocol. Clients need to know about your relay via the DERP map.

For **Headscale**, add HD to the DERP map in `config.yaml`:

```yaml
derp:
  server:
    enabled: false
  urls: []
  paths:
    - /etc/headscale/derp.yaml
```

And in `/etc/headscale/derp.yaml`:

```json
{
  "Regions": {
    "900": {
      "RegionID": 900,
      "RegionCode": "hd",
      "RegionName": "Hyper-DERP",
      "Nodes": [{
        "Name": "hd1",
        "RegionID": 900,
        "HostName": "derp.example.com",
        "DERPPort": 3340,
        "InsecureForTests": false
      }]
    }
  }
}
```

For **Tailscale** (managed), use an ACL policy with a custom DERP map. See [Tailscale's custom DERP docs](https://tailscale.com/kb/1118/custom-derp-servers).

### Clients connect but can't relay traffic

Check that both peers are connected to the same HD relay node. If using multiple relay nodes, they need mesh keys to forward between each other.

Verify with:

```bash
# On the relay, check active peers
curl -s http://localhost:9090/metrics | \
  grep hyper_derp_peers_active
```

### Self-signed certs and InsecureForTests

For testing with self-signed certificates, set `InsecureForTests: true` in the DERP map node config. This disables TLS certificate verification for DERP connections.

Do not use this in production.

## Logs

HD logs to stderr by default. Increase verbosity:

```bash
hyper-derp -c config.yaml --verbose
```

Or set `log_level: debug` in the config file.

Key log messages:

| Message | Meaning |
|---------|---------|
| `kTLS enabled` | Kernel TLS offload is active |
| `kTLS not available` | Falling back to userspace TLS |
| `worker N started` | Worker thread is up and accepting connections |
| `recv_paused` | Backpressure activated on a worker (send queue full) |
| `send_drop` | A frame was dropped because the destination queue was full |
| `slab_exhaust` | Frame pool ran out of pre-allocated buffers |

## Kernel Version Compatibility

| Feature | Minimum Kernel |
|---------|---------------|
| io_uring basic | 5.11 |
| Provided buffer rings | 5.19 |
| kTLS (TLS 1.3 AES-GCM) | 5.19 |
| DEFER_TASKRUN | 6.1 |
| SQPOLL | 5.11 (CAP_SYS_NICE) |
