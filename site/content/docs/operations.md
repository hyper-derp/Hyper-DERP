---
title: "Operations"
description: >-
  Deployment, systemd service, monitoring, kernel tuning.
weight: 4
draft: true
---

## systemd Service

```ini
[Unit]
Description=Hyper-DERP relay server
After=network-online.target
Wants=network-online.target

[Service]
Type=notify
ExecStart=/usr/bin/hyper-derp -c /etc/hyper-derp/config.toml
Restart=on-failure
RestartSec=5

# Security hardening
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
ReadOnlyPaths=/
ReadWritePaths=/etc/hyper-derp/certs
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
```

Install and enable:

```bash
sudo cp hyper-derp.service \
  /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now hyper-derp
```

## Memory Footprint

Base memory usage is low. Per-connection cost depends
on buffer ring configuration:

| Workers | Connections | Approx RSS |
|--------:|------------:|-----------:|
|       2 |       1,000 |      80 MB |
|       4 |       5,000 |     200 MB |
|       8 |      10,000 |     400 MB |

## Metrics

Prometheus-format metrics are exposed on the metrics
address (default `:9090`):

```
# Connection counts
hyper_derp_connections_active
hyper_derp_connections_total

# Throughput
hyper_derp_bytes_relayed_total
hyper_derp_frames_relayed_total

# Errors
hyper_derp_frames_dropped_total
hyper_derp_errors_total{type="..."}

# io_uring stats
hyper_derp_uring_sq_utilization
hyper_derp_uring_cq_overflow_total
```

## Kernel Tuning

Recommended sysctl settings for high-throughput
deployments:

```bash
# Socket buffers
sysctl -w net.core.rmem_max=4194304
sysctl -w net.core.wmem_max=4194304
sysctl -w net.core.rmem_default=2097152
sysctl -w net.core.wmem_default=2097152

# Connection backlog
sysctl -w net.core.somaxconn=4096
sysctl -w net.ipv4.tcp_max_syn_backlog=4096

# TCP tuning
sysctl -w net.ipv4.tcp_tw_reuse=1
sysctl -w net.ipv4.tcp_fin_timeout=15

# io_uring memlock limit (for provided buffers)
# Set in /etc/security/limits.conf or systemd unit:
# LimitMEMLOCK=infinity
```

## Monitoring

Pair the metrics endpoint with Prometheus and Grafana.
Key dashboards to set up:

- **Throughput**: `rate(hyper_derp_bytes_relayed_total[5m])`
- **Active connections**: `hyper_derp_connections_active`
- **Drop rate**: `rate(hyper_derp_frames_dropped_total[5m])`
- **io_uring saturation**:
  `hyper_derp_uring_sq_utilization`
