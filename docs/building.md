# Building

## System Requirements

- **Linux kernel 5.19+** -- io_uring with provided buffers.
  Kernel 6.0+ recommended for `SINGLE_ISSUER`,
  `DEFER_TASKRUN`, and multishot recv.
- **Kernel 6.1+** recommended for production (io_uring
  stability improvements, kTLS fixes).
- **C++23 compiler** -- Clang 17+ or GCC 13+. The build
  system auto-detects Clang if available
  (`CMakeLists.txt:6`).
- **CMake 3.20+** with Ninja (preferred) or Make.

## Dependencies

### System Packages (Debian/Ubuntu)

```sh
sudo apt install \
  clang cmake ninja-build \
  liburing-dev libsodium-dev libspdlog-dev \
  libssl-dev libasio-dev \
  libgtest-dev libgmock-dev libcli11-dev
```

### Dependency Versions

| Library | Minimum | Debian Package | Purpose |
|---------|--------:|----------------|---------|
| liburing | 2.3+ | `liburing-dev` | io_uring API |
| libsodium | 1.0.18+ | `libsodium-dev` | NaCl box, Curve25519 |
| spdlog | 1.15+ | `libspdlog-dev` | Structured logging |
| OpenSSL | 3.0+ | `libssl-dev` | kTLS handshake |
| Asio | -- | `libasio-dev` | Crow HTTP dependency |
| GTest | -- | `libgtest-dev` | Unit tests |
| GMock | -- | `libgmock-dev` | Test mocks |
| CLI11 | -- | `libcli11-dev` | CLI parsing (tools) |

### Fetched at Build Time

| Library | Version | Source |
|---------|---------|--------|
| Crow | v1.3.0.0 | `cmake/3rd_party_libs/fetch.cmake:22` |

Crow is fetched via `FetchContent` from GitHub. All other
dependencies are expected as system packages.

### Runtime Dependencies (Debian Package)

The `.deb` package declares these runtime dependencies
(`CMakeLists.txt:161`):

```
liburing2 (>= 2.9)
libsodium23 (>= 1.0.18)
libspdlog1.15 (>= 1:1.15)
```

## Build from Source

### Release Build

```sh
cmake --preset default
cmake --build build -j
```

This uses the `default` preset (`CMakePresets.json`):
Ninja generator, Release mode, compile_commands.json
enabled. The binary is at `build/hyper-derp`.

### Debug Build

```sh
cmake --preset debug
cmake --build build-debug -j
```

Debug mode adds `-O0 -g -fno-omit-frame-pointer` and
`-glldb` if LLDB is detected (`CMakeLists.txt:80`).

### Manual Configuration

```sh
mkdir build && cd build
cmake .. -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build . -j
```

### Linker Selection

The build system auto-detects the fastest available linker
in order: mold, lld, gold, default ld
(`CMakeLists.txt:41`). No manual configuration needed.

## CMake Presets

| Preset | Type | Build Dir | Generator |
|--------|------|-----------|-----------|
| `default` | Release | `build/` | Ninja |
| `debug` | Debug | `build-debug/` | Ninja |

Both presets enable `CMAKE_EXPORT_COMPILE_COMMANDS` for
IDE integration and clang-tidy.

## ARM64 Cross-Compile

Create a toolchain file `CMakeCrossCompile-aarch64.cmake`:

```cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
```

Install cross-compile dependencies:

```sh
sudo apt install \
  gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
  liburing-dev:arm64 libsodium-dev:arm64 \
  libspdlog-dev:arm64 libssl-dev:arm64 \
  libasio-dev
```

Build:

```sh
cmake -B build-arm64 \
  -DCMAKE_TOOLCHAIN_FILE=CMakeCrossCompile-aarch64.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -G Ninja
cmake --build build-arm64 -j
```

io_uring is architecture-independent in kernel 6.1+.
Target platforms: AWS Graviton, Oracle Ampere, Azure Arm.

## Fuzz Targets

Fuzz targets require Clang with libFuzzer:

```sh
cmake --preset debug -DENABLE_FUZZING=ON
cmake --build build-debug -j
```

Fuzz targets are in the `fuzz/` directory and are only
built when `ENABLE_FUZZING=ON` (`CMakeLists.txt:129`).

## Debian Packaging

Build a `.deb` package with CPack:

```sh
cmake --preset default
cmake --build build -j
cmake --build build --target package
```

This produces `build/hyper-derp_<version>_<arch>.deb`.

Install:

```sh
sudo dpkg -i build/hyper-derp_*.deb
```

### Package Contents

| Path | Content |
|------|---------|
| `/usr/bin/hyper-derp` | Relay server binary |
| `/usr/bin/derp-test-client` | Test client tool |
| `/usr/bin/derp-scale-test` | Scale testing tool |
| `/usr/bin/derp-tun-proxy` | Tunnel proxy tool |
| `/lib/systemd/system/hyper-derp.service` | systemd unit |
| `/etc/hyper-derp/hyper-derp.conf.example` | Config template |

Install targets are defined at `CMakeLists.txt:136`.

### Package Metadata

| Field | Value |
|-------|-------|
| Name | `hyper-derp` |
| Section | `net` |
| Priority | `optional` |
| Homepage | `github.com/hyper-derp/hyper-derp` |

Post-install scripts handle systemd daemon-reload and
service enablement. Scripts are in `dist/postinst`,
`dist/prerm`, `dist/postrm` (`CMakeLists.txt:174`).

## Installed Files

The `hyper-derp.service` systemd unit
(`dist/hyper-derp.service`) reads options from
`/etc/hyper-derp/hyper-derp.conf` via `EnvironmentFile`.
The unit includes security hardening: `DynamicUser`,
`ProtectSystem=strict`, `NoNewPrivileges`, restricted
system call filter, and `CAP_SYS_NICE` for SQPOLL mode.

## Troubleshooting

### Missing liburing headers

```
fatal error: liburing.h: No such file or directory
```

Install `liburing-dev` (Debian/Ubuntu) or `liburing-devel`
(Fedora/RHEL).

### kTLS module not loaded

If kTLS handshake fails at runtime:

```sh
sudo modprobe tls
lsmod | grep tls
```

The TLS kernel module must be loaded before starting
hyper-derp with `--tls-cert`/`--tls-key`.

### Cross-compile: missing arm64 libraries

Enable multi-arch and install arm64 packages:

```sh
sudo dpkg --add-architecture arm64
sudo apt update
sudo apt install liburing-dev:arm64 libsodium-dev:arm64
```
