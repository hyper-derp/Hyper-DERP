---
title: "Building"
description: "Build instructions, dependencies, and packaging."
weight: 2
---

## System Requirements

- **Linux kernel 5.19+** -- io_uring with provided buffers.
  Kernel 6.1+ recommended for production.
- **C++23 compiler** -- Clang 17+ or GCC 13+.
- **CMake 3.20+** with Ninja (preferred) or Make.

## Dependencies (Debian/Ubuntu)

```sh
sudo apt install \
  clang cmake ninja-build \
  liburing-dev libsodium-dev libspdlog-dev \
  libssl-dev libasio-dev \
  libgtest-dev libgmock-dev libcli11-dev
```

| Library | Minimum | Purpose |
|---------|--------:|---------|
| liburing | 2.3+ | io_uring API |
| libsodium | 1.0.18+ | NaCl box, Curve25519 |
| spdlog | 1.15+ | Structured logging |
| OpenSSL | 3.0+ | kTLS handshake |
| Asio | -- | Crow HTTP dependency |

Crow v1.3.0.0 is fetched automatically via CMake
`FetchContent`.

## Build from Source

```sh
# Release
cmake --preset default
cmake --build build -j

# Debug
cmake --preset debug
cmake --build build-debug -j
```

## ARM64 Cross-Compile

```sh
sudo apt install \
  gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
cmake \
  -DCMAKE_TOOLCHAIN_FILE=CMakeCrossCompile-aarch64.cmake \
  -B build-arm64
cmake --build build-arm64 -j
```

Targets AWS Graviton and Ampere (Oracle/Azure) instances.
io_uring is architecture-independent on kernel 6.1+.

## Packaging

```sh
cmake --build build --target package
sudo dpkg -i build/hyper-derp_*.deb
```

Installs the systemd unit (`hyper-derp.service`), example
config (`/etc/hyper-derp/hyper-derp.conf.example`), and
binaries to `/usr/bin/`.

## Detailed Reference

The full build guide with version tables and runtime
dependencies is in the repository at
[docs/building.md](https://github.com/hyper-derp/hyper-derp/blob/main/docs/building.md).
