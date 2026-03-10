# Hyper-DERP

High-performance DERP (Designated Encrypted Relay for Packets) server.
Linux-only, io_uring-based, pure data-oriented design.

## Build

```bash
cmake --preset default    # or debug
cmake --build build -j
ctest --output-on-failure -j --test-dir build/tests
```

### Dependencies (system)

- liburing-dev
- clang (preferred compiler, auto-detected)
- ninja-build
- cmake >= 3.20

### Dependencies (system, continued)

- libspdlog-dev
- libgtest-dev, libgmock-dev
- libcli11-dev
- libsodium-dev

## Project Structure

- `include/hyper_derp/protocol.h` — DERP wire protocol constants, frame codec
- `include/hyper_derp/types.h` — DOD data types (Peer, Route, Worker, Ctx)
- `include/hyper_derp/data_plane.h` — io_uring data plane public API
- `include/hyper_derp/http.h` — HTTP upgrade request/response codec
- `include/hyper_derp/handshake.h` — DERP handshake (ServerKey/ClientInfo/ServerInfo)
- `include/hyper_derp/server.h` — Server lifecycle (accept loop, data plane integration)
- `src/protocol.cc` — Frame building functions
- `src/data_plane.cc` — io_uring sharded worker implementation
- `src/http.cc` — HTTP parser/builder
- `src/handshake.cc` — Key exchange, NaCl box crypto
- `src/server.cc` — TCP listener, HTTP upgrade, handshake, hand-off
- `src/main.cc` — Entry point (--port, --workers)

## Architecture

Pure data-oriented design: plain structs, free functions, no virtual dispatch.

```
TCP accept (port 3340)
  |
  v
HTTP upgrade (GET /derp → 101 Switching Protocols)
  |
  v
DERP handshake (ServerKey → ClientInfo → ServerInfo)
  |  (Curve25519 + NaCl box crypto via libsodium)
  v
DpAddPeer(ctx, fd, client_public_key)
  |
  v
Worker threads (sharded by peer key hash)
  |--- io_uring recv/send/send_zc
  |--- Per-worker hash table (Peer[])
  |--- Replicated routing table (Route[])
  |--- MPSC cross-shard transfer rings
```

### DERP Wire Format

Frame: `[1 byte type][4 bytes big-endian length][payload]`

Key frame types: ServerKey (0x01), ClientInfo (0x02), ServerInfo (0x03),
SendPacket (0x04), RecvPacket (0x05), KeepAlive (0x06),
Ping (0x12), Pong (0x13).

### Handshake Flow (matches Tailscale client expectations)

1. Client: `GET /derp` with `Upgrade: DERP`, `Connection: Upgrade`
2. Server: `HTTP/1.1 101` with `Derp-Version`, `Derp-Public-Key` headers
3. Server: ServerKey frame = `[8B magic "DERP🔑"][32B server pub key]`
4. Client: ClientInfo frame = `[32B client pub key][24B nonce][NaCl box(JSON)]`
5. Server: ServerInfo frame = `[24B nonce][NaCl box(JSON{version:2})]`
6. Authenticated — fd handed to data plane via DpAddPeer

### Data Plane

Adapted from tested C prototype. Each worker:
- Owns a disjoint peer set (hash of 32-byte Curve25519 key)
- Uses io_uring SEND_ZC for zero-copy sends
- Provided buffer rings for recv
- SPSC command ring from control plane
- MPSC ring + eventfd for cross-shard forwarding

## Code Conventions

- C++23, Google Style Guide
- 80-character line limit, 2-space indent
- Doxygen docstrings (`///`) on all public APIs
- Single newline between top-level definitions
- `CamelCase` functions, `kCamelCase` constants, `snake_case` members
- Namespace: `hyper_derp`

## Scripts

- `./scripts/format.sh` — clang-format all sources
- `./scripts/lint.sh` — cpplint
- `./scripts/autotest.sh` — watch + rebuild + test (requires entr)
