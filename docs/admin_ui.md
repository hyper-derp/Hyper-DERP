# Admin UI

## Status

Design. Not implemented. Specifies a minimal embedded web
UI for operational visibility into a running Hyper-DERP
instance, applicable to both `mode: derp` and
`mode: wireguard`.

## Goals

- Operator can answer at a glance: *is the relay alive,
  who is connected, how much traffic, any errors*.
- Zero external dependencies. Single binary still ships
  everything.
- No runtime state mutation in v0. Read-only.
- Safe by default. Binds to localhost only; operator opts
  in to exposure.

## Non-goals

- Not a general-purpose dashboard. Operators who want
  Grafana point it at the existing Prometheus metrics
  endpoint.
- Not a peer management console. Adding/removing peers
  stays in the config file in v0. Runtime mutation is a
  separate v1 project that touches the control plane.
- Not authentication/authorization infrastructure. Basic
  auth or upstream reverse proxy is the expected control.

## Architecture

One process, three surfaces on the existing admin HTTP
server (Crow, already present for `/metrics`):

| Path | Content |
|------|---------|
| `/` | Static SPA: HTML + vanilla JS, embedded in binary. |
| `/api/status` | JSON snapshot — relay info, worker stats, peer roster. |
| `/api/traffic` | JSON — per-peer pps / bps rolling window, for charts. |
| `/metrics` | Unchanged Prometheus endpoint. |

Static assets are compiled into the binary via
`xxd -i` or equivalent at build time and served from a
constexpr byte array. No filesystem lookup, no runtime
asset dependencies.

### Why vanilla JS, not React / Vue

Build toolchain avoidance. A 300–500 line single-page
app with `fetch()` + direct DOM writes covers the
feature set. Adding a bundler (webpack / vite / esbuild)
just to render a peer table is not worth the build-time
complexity for a project whose primary language is C++.

Estimated asset size: ~15 KB of HTML/JS/CSS minified,
gzipped to ~5 KB.

## UI pages

Single page. Tabs switch between sections; all served
from `/`.

### Overview

- Hostname, mode (`derp` / `wireguard`), version, uptime.
- Aggregate: total connected peers, total packets/sec in
  and out, total bytes/sec in and out, active workers.
- Error summary: top three reject reasons in the last 60 s
  (from the reject counters).

### Peers

Table, one row per peer in the roster:

| Column | Source |
|--------|--------|
| Peer ID | config |
| Label | config |
| Auth mode | config |
| Status | live: *connected* if endpoint known + packets seen in last 30 s, *idle* if known but quiet, *unknown* if never seen |
| Endpoint | `udp_endpoint_map[peer_id]`, redacted if not admin |
| Last seen | timestamp of most recent valid packet |
| Rx pps | rolling 5-second rate |
| Tx pps | rolling 5-second rate |
| Rejects (60 s) | sum of per-peer reject counters |

For DERP mode: same table but "Endpoint" is the socket
4-tuple, "Auth mode" is `tls`.

### Traffic

Simple line chart, last 5 minutes, 1-second resolution:

- Total pps in / out.
- Total bps in / out.

Data source: the admin HTTP server maintains a 300-slot
ring buffer sampled once per second from the worker
stats. The `/api/traffic` endpoint returns the full
buffer as a JSON array. Client draws the chart with
plain canvas calls — no charting library.

### Logs

Last N log lines (N=500) served from an in-memory ring
buffer populated by the logging system. Endpoint:
`/api/logs?since=<lsn>`. Long-poll, no WebSocket. If a
production operator wants structured logs they read the
stderr stream; the UI log tail is for quick triage.

## Security

### Bind address

Defaults to `127.0.0.1:8080`. Must be explicitly bound to
another interface via config:

```yaml
admin:
  bind: "127.0.0.1:8080"   # default, localhost only
  # bind: "0.0.0.0:8080"   # exposes everywhere — DON'T
  # bind: "10.0.0.10:8080" # mgmt interface only
```

The documented recommendation is: leave it on localhost,
use an SSH port-forward to reach it, or front it with a
reverse proxy that does TLS + auth.

### Basic auth (optional)

```yaml
admin:
  bind: "10.0.0.10:8080"
  auth:
    users:
      - name: "ops"
        password_hash: "$2y$12$..."  # bcrypt
```

If `auth` is set, every request (including static asset
fetches) requires HTTP Basic. No session cookies, no
logout flow, nothing to forget to invalidate. Wrong
password returns 401 after a ~100 ms delay to slow
guessing.

### CSRF

Not applicable in v0 — no state mutation endpoints. If/when
v1 adds POST endpoints, switch to token-in-header CSRF
protection.

### Data exposure

Two knobs:

```yaml
admin:
  redact_endpoints: true  # replace IPs with "x.x.x.x:port"
  show_logs: true         # false = /api/logs returns 404
```

Endpoint redaction matters when the UI is proxied to an
audience broader than the operator (e.g. exposed to
customer-facing dashboards).

## Implementation cost

| Component | LOC |
|-----------|----:|
| Admin HTTP routes (C++) | ~250 |
| Stats ring buffer + sampling loop | ~150 |
| In-memory log ring buffer | ~100 |
| Static SPA (HTML+JS+CSS) | ~500 |
| Build glue (`xxd -i` rule, embed) | ~50 |
| **Total** | **~1050** |

Delivered as two new source files: `src/admin_ui.cc` and
`src/admin_stats.cc`, plus a `ui/` directory for the raw
SPA assets embedded at build time.

## Phases

1. **Phase 0:** JSON `/api/status` endpoint returning the
   existing Prometheus metrics as structured JSON. No UI
   yet — `curl`-testable.
2. **Phase 1:** Static SPA for Overview + Peers tabs.
   Auto-refresh every 2 s.
3. **Phase 2:** Traffic chart + rolling stats ring buffer.
4. **Phase 3:** Logs endpoint + log tail tab.
5. **Phase 4:** Basic auth + redaction options.

Phases 0–2 cover 80 % of operator value. Phases 3–4 can
land post-release if schedule tightens.
