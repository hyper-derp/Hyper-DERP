# Packaging — Docker Image

## Status

Design. Not implemented. Specifies the Docker image for
the Hyper-DERP v1 release. Applies to both `mode: derp`
and `mode: wireguard`.

## Goals

- One image that runs the relay with sensible defaults.
- Multi-stage build: fat builder, slim runtime.
- Runs as non-root user inside the container.
- Host-OS requirements documented and minimal.
- Published to a container registry alongside the GitHub
  release.

## Non-goals

- Not a Kubernetes operator. Users who want k8s deploy
  write their own manifests around this image.
- Not a systemd unit / Debian package. Those are separate
  packaging tracks.

## Base image

**Builder:** `debian:13-slim` (matches the project's
declared build target: `6.12.73+deb13-cloud-amd64`).
Installs `build-essential`, `cmake`, `ninja-build`,
`libssl-dev`, `liburing-dev`, `pkg-config`. About 500 MB,
discarded.

**Runtime:** `debian:13-slim`. Adds only:

- `libssl3` — kTLS + OpenSSL.
- `liburing2` — io_uring shim.
- `ca-certificates` — if operator mounts TLS certs.

Expected runtime image size: ~90 MB. Stripped HD binary
~4 MB; most of the image is the base layer.

*Not Alpine.* Alpine's musl has historically caused
io_uring edge-case differences and lacks kTLS tooling.
Debian slim is worth the ~50 MB tradeoff.

## Dockerfile sketch

```dockerfile
# syntax=docker/dockerfile:1.7

FROM debian:13-slim AS builder
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build \
      libssl-dev liburing-dev pkg-config ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DHD_WG_MODE=ON \
      -DHD_ADMIN_UI=ON \
    && cmake --build build --target hyper-derp \
    && strip build/hyper-derp

FROM debian:13-slim AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
      libssl3 liburing2 ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --no-create-home --shell /usr/sbin/nologin hyperderp \
    && mkdir -p /etc/hyper-derp /var/lib/hyper-derp \
    && chown hyperderp:hyperderp /var/lib/hyper-derp

COPY --from=builder /src/build/hyper-derp /usr/local/bin/hyper-derp
COPY dist/hyper-derp.yaml /etc/hyper-derp/hyper-derp.yaml

USER hyperderp
EXPOSE 3340/tcp 51820/udp 8080/tcp
HEALTHCHECK --interval=30s --timeout=5s --retries=3 \
  CMD wget -qO- http://127.0.0.1:8080/api/status >/dev/null || exit 1
ENTRYPOINT ["/usr/local/bin/hyper-derp", "--config", "/etc/hyper-derp/hyper-derp.yaml"]
```

## Host OS requirements

The image cannot provide these; the operator must:

1. **kTLS kernel module** (`mode: derp` only).
   `modprobe tls` on the host. Containers inherit the
   host's module state. Document this in `OPERATIONS.md`.

2. **Kernel ≥ 6.0** for full io_uring feature support.
   `uname -r` on the host; the image will run on older
   kernels with reduced features (no multishot, no
   SEND_ZC) but performance suffers.

3. **Sufficient `net.core.rmem_max` / `wmem_max`** for
   UDP mode at high rate. Documented recommendation:
   `sysctl -w net.core.rmem_max=16777216`.

4. **BPF cap or NET_ADMIN** only if the operator enables
   the future eBPF fast path. Not needed for v0.

## Container capabilities

Runs with default Docker capabilities (no `--privileged`,
no `--cap-add`). Does **not** need:

- `NET_ADMIN` (no interface manipulation).
- `SYS_NICE` (no RT scheduling).
- `IPC_LOCK` (slab allocator uses plain `mmap`).

Needs only the default `NET_BIND_SERVICE` to bind below
1024, which is irrelevant for the default ports
(3340, 51820, 8080).

## Volumes and configuration

| Container path | Purpose | Typical mount |
|----------------|---------|---------------|
| `/etc/hyper-derp/hyper-derp.yaml` | Main config | Host config dir |
| `/etc/hyper-derp/certs/` | TLS cert + key (DERP mode) | Host secret store / k8s secret |
| `/etc/hyper-derp/peers/` | Per-peer HMAC secret files (WG mode) | Host secret store |
| `/var/lib/hyper-derp/` | Runtime state (replay nonces, etc.) | Named volume |

All paths configurable via the YAML file; the paths above
are defaults.

## Example run

```bash
docker run -d --name hyper-derp \
  --restart unless-stopped \
  -p 3340:3340 \
  -p 51820:51820/udp \
  -p 127.0.0.1:8080:8080 \
  -v /etc/hyper-derp:/etc/hyper-derp:ro \
  -v hyper-derp-state:/var/lib/hyper-derp \
  ghcr.io/hyper-derp/hyper-derp:v1.0.0
```

Note the admin UI port `8080` bound to `127.0.0.1` on the
host — matches the admin UI security default.

## docker-compose.yaml

Shipped in `dist/docker-compose.yaml` for operators who
prefer compose:

```yaml
services:
  hyper-derp:
    image: ghcr.io/hyper-derp/hyper-derp:v1.0.0
    restart: unless-stopped
    ports:
      - "3340:3340"
      - "51820:51820/udp"
      - "127.0.0.1:8080:8080"
    volumes:
      - /etc/hyper-derp:/etc/hyper-derp:ro
      - hyper-derp-state:/var/lib/hyper-derp
    sysctls:
      - net.core.rmem_max=16777216
      - net.core.wmem_max=16777216

volumes:
  hyper-derp-state:
```

## Image build / publish

- GitHub Actions workflow on tag push (`v*.*.*`):
  1. `docker buildx` multi-arch: `amd64` + `arm64`.
     Per the project's ARM targeting (Graviton / Ampere),
     `arm64` is first-class, not an afterthought.
  2. Push to `ghcr.io/hyper-derp/hyper-derp:<tag>` and
     `:latest`.
  3. Also publish a `:<tag>-debug` tag built with
     `-DCMAKE_BUILD_TYPE=RelWithDebInfo` and unstripped
     binary, for crash investigation.
- Image is signed with cosign; signature published
  alongside the image.
- SBOM generated via `syft` and attached to the GitHub
  release.

## What this doesn't solve

- **kTLS on container platforms that sandbox `setsockopt`**
  (some PaaS, some strict security policies). If the host
  rejects the `SOL_TLS` setsockopt, HD falls back to
  userspace TLS via OpenSSL — slower but correct. Document
  the detection path in `OPERATIONS.md`.
- **IPv6 by default.** Requires `--sysctl
  net.ipv6.conf.all.disable_ipv6=0` and the host's
  Docker daemon having IPv6 enabled. Operator concern,
  documented.

## Work estimate

| Item | Hours |
|------|------:|
| Dockerfile | 1–2 |
| CI build + publish workflow | 3–4 |
| Multi-arch cross-compile verification | 2–3 |
| Documentation updates (`OPERATIONS.md`, `README.md`) | 1–2 |
| **Total** | **7–11 h** |

Small compared to WG mode and admin UI. The main risk is
multi-arch kTLS behavior — worth a smoke test on each
target arch before tagging.
