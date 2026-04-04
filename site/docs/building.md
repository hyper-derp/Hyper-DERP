---
title: "Building"
description: >-
  Build from source, dependencies, CMake presets,
  cross-compile for ARM64.
sidebar_position: 5
---

## Dependencies

| Package | Min Version | Purpose |
|---------|-------------|---------|
| CMake | 3.25 | Build system |
| Ninja | 1.11 | Build backend |
| Clang | 17 | C++23 compiler |
| liburing | 2.4 | io_uring API |
| libsodium | 1.0.18 | Curve25519, XSalsa20 |
| libspdlog | 1.12 | Logging |
| OpenSSL | 3.0 | TLS, kTLS support |

On Debian/Ubuntu:

```bash
sudo apt install cmake ninja-build clang \
  liburing-dev libsodium-dev libspdlog-dev libssl-dev
```

## CMake Presets

```bash
# Release (default)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Debug (ASAN + assertions)
cmake -B build-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j$(nproc)
```

## Cross-Compile for ARM64

```bash
sudo apt install gcc-aarch64-linux-gnu \
  g++-aarch64-linux-gnu

cmake -B build-arm64 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
  -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++

cmake --build build-arm64 -j$(nproc)
```

## Running Tests

```bash
cd build && ctest --output-on-failure
```

Tests require a kernel with io_uring support (5.11+).
Some kTLS tests require 5.19+.
