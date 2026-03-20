---
title: "Install"
description: "Installation instructions for Hyper-DERP."
---

## APT Repository

```sh
# Add GPG key
curl -fsSL https://hyper-derp.dev/repo/key.gpg | \
  sudo gpg --dearmor \
  -o /usr/share/keyrings/hyper-derp.gpg

# Add repository
echo "deb [signed-by=/usr/share/keyrings/hyper-derp.gpg] \
  https://hyper-derp.dev/repo stable main" | \
  sudo tee /etc/apt/sources.list.d/hyper-derp.list

# Install
sudo apt update && sudo apt install hyper-derp
```

Packages are available for amd64 and arm64.

## Build from Source

```sh
# Install dependencies (Debian/Ubuntu)
sudo apt install \
  clang cmake ninja-build \
  liburing-dev libsodium-dev libspdlog-dev \
  libssl-dev libasio-dev \
  libgtest-dev libgmock-dev libcli11-dev

# Build
cmake --preset default
cmake --build build -j
```

Requires Linux with io_uring support (kernel 6.1+
recommended). See the [build docs](/docs/building/) for
details and ARM64 cross-compilation.

## Systemd Configuration

The `.deb` package installs a systemd unit. Configure it via
the environment file:

```sh
sudo editor /etc/hyper-derp/hyper-derp.conf
```

Example configuration:

```
HYPER_DERP_PORT=443
HYPER_DERP_TLS_CERT=/etc/hyper-derp/cert.pem
HYPER_DERP_TLS_KEY=/etc/hyper-derp/key.pem
HYPER_DERP_WORKERS=0
HYPER_DERP_METRICS_PORT=9090
```

Start the service:

```sh
sudo systemctl enable --now hyper-derp
```

## kTLS Prerequisites

```sh
sudo modprobe tls
echo tls | sudo tee /etc/modules-load.d/tls.conf
```

Without the `tls` kernel module loaded, HD falls back to
userspace TLS silently.

## Production Tuning

See [OPERATIONS.md](https://github.com/hyper-derp/hyper-derp/blob/main/OPERATIONS.md)
for sysctl settings, CPU pinning, file descriptor limits,
and Prometheus alerting.
