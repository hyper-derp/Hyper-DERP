# HD SDK

A client library for building applications that use an HD
relay as their message transport. Layered so tools only link
what they use; everything above `hd_sdk` is an optional
extension.

Source layout:

| Path | Library | Role |
|------|---------|------|
| `sdk/include/hd/sdk/` + `sdk/src/` | `hd_sdk` | Core: `Client`, `Tunnel`, config, frame pool |
| `sdk/include/hd/wg/` + `sdk/libs/hd_wg/` | `hd_wg` | WireGuard integration (netlink, proxy, state machine) |
| `sdk/include/hd/ice/` + `sdk/libs/hd_ice/` | `hd_ice` | ICE NAT traversal agent |
| `sdk/include/hd/bridge/` + `sdk/libs/hd_bridge/` | `hd_bridge` | TCP/unix-socket ↔ tunnel bridging |
| `sdk/include/hd/fleet/` + `sdk/libs/hd_fleet/` | `hd_fleet` | Fleet topology view |
| `sdk/include/hd/policy/policy.h` | header-only | Routing-intent evaluation |
| `sdk/bindings/c/` | `hd_sdk_c` | C ABI wrapper |
| `sdk/tools/hdcat/` | binary `hdcat` | netcat-like over HD tunnels |
| `sdk/examples/` | | Minimal client |

## Core API (`hd::sdk`)

Move-only handles with pimpl. Thread-safe callback
registration. The I/O thread is either owned by the SDK
(`EventThread::Own`) or driven externally via `Poll()` +
`EventFd()` for integration with an existing event loop.

```cpp
#include "hd/sdk/client.h"

hd::sdk::ClientConfig cfg;
cfg.relay_host = "10.50.0.2";
cfg.relay_port = 3341;
cfg.relay_key_hex = "aabbcc...";  // HMAC secret
cfg.event_thread = hd::sdk::EventThread::Own;

auto client = hd::sdk::Client::Create(cfg).value();

client.SetPeerCallback(
    [](const hd::sdk::PeerInfo& peer, bool connected) {
      // peer.name, peer.id, peer.public_key
    });

client.Start();

auto tunnel = client.Open("peer-name").value();
tunnel.SetDataCallback(
    [](std::span<const uint8_t> data) { /* handle */ });
tunnel.Send(std::span<const uint8_t>(buf, len));
```

Key types:

| Type | Header | Notes |
|------|--------|-------|
| `Client` | `hd/sdk/client.h` | Connection + peer registry + tunnel factory |
| `Tunnel` | `hd/sdk/tunnel.h` | Per-peer channel, `Send` / `SendOwned` / `SetDataCallback` |
| `PeerInfo` | `hd/sdk/peer_info.h` | Observed peer: name, id, public key |
| `FrameBuffer` | `hd/sdk/frame_pool.h` | Zero-copy pooled buffer; reuse on drop |
| `ClientConfig` | `hd/sdk/config.h` | Relay target, keys, event-loop mode |
| `Result<T>` | `hd/sdk/error.h` | `std::expected<T, Error>` |

## Extensions

Each extension is an independent static lib that links
`hd_sdk`. Tools pick what they need.

### `hd_wg` — WireGuard integration

Drives `wireguard.ko` via generic netlink (libmnl). Runs a
UDP proxy on `127.0.0.1:<port>` that bridges WG UDP to HD
MeshData, so WG traffic can fall back to HD when direct
UDP fails. See [hdwg.md](../hdwg.md) for the end-to-end flow.

Public surface in `hd/wg/`:

- `wg_netlink.h` — `WgNlSetDevice`, `WgNlSetPeer`,
  `WgNlRemovePeer`, `WgNlGetPeerStats` (rx/tx + handshake)
- `wg_proxy.h` — `WgProxy` state, `WgProxyHandleUdp`,
  `WgProxyHandleHd`, WGEX serialization helpers
- `wg_peer.h` — `WgPeer`, `WgPeerState` enum, peer table
- `wg_config.h` — YAML config loader

Link: `target_link_libraries(X PRIVATE hd_wg)` — transitively
pulls in `hd_sdk`, `hd_ice`, and libmnl.

### `hd_ice` — NAT traversal

STUN binding requests, candidate gathering, connectivity
checks. Used by `hd_wg` for direct-path discovery; usable
standalone for any tunnel setup.

### `hd_bridge` — TCP/unix ↔ tunnel

Bidirectional byte pipe between a local socket and an HD
tunnel. `hdctl`'s bridge runner builds on this.

### `hd_fleet` — topology view

Observes `FleetData` / route announcements to maintain a
picture of the reachable relay graph.

### `hd_policy` — routing intent

Header-only evaluator for declarative policies (peer name
globs, tunnel IP ranges, allow/deny).

## C ABI (`hd_sdk_c`)

Opaque pointers + C callback function pointers. Intended
for FFI from Go, Python, etc. See `sdk/bindings/c/hd_sdk_c.h`.

## Examples

- `sdk/examples/simple_tunnel.cc` — connect, open tunnel,
  send bytes, print replies.
- `sdk/tools/hdcat/hdcat.cc` — netcat-equivalent. Run
  `hdcat --help` for the flag set, or see the YAML config
  examples under `sdk/tools/hdcat/` for connect/listen,
  TCP / UDP / unix-socket, and wildcard peer patterns.

## Build

```sh
cmake --build build --target hd_sdk      # core
cmake --build build --target hd_wg       # WG extension
cmake --build build --target hdcat       # tool
cmake --build build --target hd_sdk_c    # C wrapper
```

The tree is structured so `add_subdirectory(sdk)` in a
downstream CMake project gives access to all targets.
