# Security Policy

## Reporting a Vulnerability

If you discover a security vulnerability in Hyper-DERP,
please report it responsibly.

**Email:** karl@localhost
**Subject:** [SECURITY] Hyper-DERP vulnerability report

Please include:
- Description of the vulnerability
- Steps to reproduce
- Potential impact
- Suggested fix (if any)

We will acknowledge receipt within 48 hours and provide an
initial assessment within 7 days.

## Security Design

### Cryptography
- Curve25519 key exchange via libsodium
- NaCl box authenticated encryption for handshake
- Timing-safe key comparison (sodium_memcmp) in all
  lookup paths

### Network Exposure
- DERP protocol port (default 3340): accepts TCP connections,
  performs HTTP upgrade + NaCl handshake before admitting peers
- Metrics port (optional): HTTP(S) server for Prometheus
  scraping. Debug endpoints (/debug/peers) expose peer public
  keys and require explicit --debug-endpoints flag.

### Process Hardening
- Systemd unit runs with DynamicUser, ProtectSystem=strict,
  NoNewPrivileges, MemoryDenyWriteExecute, and restricted
  syscall filter
- No privilege escalation required after bind
- SIGPIPE ignored; SIGINT/SIGTERM handled via atomic flag

### Input Validation
- HTTP request size capped at 4 KB
- DERP frame payload capped at 64 KB
- All CLI arguments bounds-checked
- Connection rate limiting (--max-accept-rate)
