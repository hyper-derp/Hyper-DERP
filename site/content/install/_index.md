---
title: "Install"
description: "Install Hyper-DERP from APT or build from source."
draft: true
---

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

### Build

```bash
git clone \
  https://github.com/hyper-derp/hyper-derp.git
cd hyper-derp
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### Install

```bash
sudo cmake --install build
```

### Run Tests

```bash
cd build && ctest --output-on-failure
```

## Next Steps

- [Configuration reference](/docs/configuration/)
- [Deployment and systemd setup](/docs/operations/)
- [Architecture overview](/docs/architecture/)
