---
title: Installation
description: Install Hyper-DERP from APT or build from source.
sidebar_position: 1
---

## Requirements

- **OS**: Linux (Debian 13+, Ubuntu 24.04+)
- **Kernel**: 5.11+ for io_uring, 5.19+ for kTLS
- **Arch**: amd64 or arm64

## APT Repository (Debian/Ubuntu)

Packages are available for amd64 and arm64.

```bash
curl -fsSL https://hyper-derp.dev/repo/key.gpg | \
  sudo gpg --dearmor \
    -o /usr/share/keyrings/hyper-derp.gpg

echo "deb [signed-by=/usr/share/keyrings/hyper-derp.gpg] \
  https://hyper-derp.dev/repo stable main" | \
  sudo tee /etc/apt/sources.list.d/hyper-derp.list

sudo apt update && sudo apt install hyper-derp
```

Verify:

```bash
hyper-derp --version
```

## Build from Source

### Dependencies

```bash
sudo apt install cmake ninja-build clang \
  liburing-dev libsodium-dev libspdlog-dev libssl-dev
```

See [Building](/docs/building/) for the full dependency table, CMake presets, cross-compilation, and running tests.

### Build and Install

```bash
git clone \
  https://github.com/hyper-derp/Hyper-DERP.git
cd Hyper-DERP
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
sudo cmake --install build
```

## Quick Start

After installing, the minimum to get HD running:

### 1. Generate TLS Certificates

HD needs a TLS certificate for the DERP protocol. For testing, use a self-signed cert:

```bash
sudo mkdir -p /etc/hyper-derp
sudo openssl req -x509 -newkey ec \
  -pkeyopt ec_paramgen_curve:prime256v1 \
  -keyout /etc/hyper-derp/key.pem \
  -out /etc/hyper-derp/cert.pem \
  -days 365 -nodes \
  -subj "/CN=derp.example.com"
```

For production, use certificates from Let's Encrypt or your CA.

### 2. Create a Config File

```bash
sudo tee /etc/hyper-derp/config.yaml << 'EOF'
port: 3340
tls_cert: /etc/hyper-derp/cert.pem
tls_key: /etc/hyper-derp/key.pem
log_level: info
EOF
```

### 3. Run

```bash
hyper-derp -c /etc/hyper-derp/config.yaml
```

HD will start listening on port 3340 with kTLS enabled (if the kernel supports it). Worker count defaults to the number of available CPUs.

### 4. Verify

From another machine, check the DERP endpoint:

```bash
curl -k https://your-server:3340/derp/probe
```

A healthy relay responds with `ok`.

## Next Steps

- [Configuration reference](/docs/configuration/) -- all CLI flags, config file options, worker tuning
- [Operations](/docs/operations/) -- systemd service, monitoring, kernel tuning
- [Troubleshooting](/docs/troubleshooting/) -- common issues and fixes
