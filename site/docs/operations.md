---
title: "Operations"
description: >-
  Deployment, systemd service, monitoring, kernel tuning.
sidebar_position: 4
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
# Throughput
hyper_derp_recv_bytes_total
hyper_derp_send_bytes_total

# Drops and errors
hyper_derp_send_drops_total
hyper_derp_xfer_drops_total
hyper_derp_slab_exhausts_total
hyper_derp_send_errors_total
hyper_derp_recv_enobufs_total

# Allocation
hyper_derp_frame_pool_hits_total
hyper_derp_frame_pool_misses_total

# Gauges
hyper_derp_peers_active
hyper_derp_workers
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

- **Throughput**: `rate(hyper_derp_send_bytes_total[5m])`
- **Active peers**: `hyper_derp_peers_active`
- **Send drops**: `rate(hyper_derp_send_drops_total[5m])`
- **Cross-shard drops**:
  `rate(hyper_derp_xfer_drops_total[5m])`
