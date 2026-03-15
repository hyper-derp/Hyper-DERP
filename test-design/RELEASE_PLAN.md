# Hyper-DERP — Release Plan

## Current State (2026-03-15)

### Complete
- C++23 io_uring relay with SPSC cross-shard, P3 bitmask, kTLS
- GCP Phase A (plain TCP) full sweep: 2/4/8/16 vCPU, 25 runs
- GCP Phase B (kTLS) full sweep: 2/4/8/16 vCPU, 25 runs
- AWS Phase B (kTLS): 8/16 vCPU on c7i
- Worker scaling analysis (2/4/6/8 workers, TCP + kTLS)
- Tunnel tests (Phases 0-4, Headscale, real Tailscale clients)
- Optimization: P3 bitmask (+13.5% kTLS throughput)
- Reports with plots for all datasets
- Test designs documented

### Key Results
- GCP 2 vCPU kTLS: 11.8x throughput ratio over TS
- GCP 8 vCPU kTLS: 2.0x throughput ratio
- Tunnel: 1.7x throughput, 48x fewer retransmits (5 min sustained)
- AWS 8 vCPU: 2.0-2.2x — cross-cloud confirmed
- kTLS eliminates worker scaling regression

## Phase 1: Bare Metal (Week of 2026-03-17)

### Hardware Setup
- Assemble: 2x Dell T5810 + CX4 LX + DDR4 + DAC cables
- Firmware: flash CX4 LX to latest (mstflint)
- Network: dual-port SFP28, two subnets, iperf3 validation
- OS: Debian 13, kernel 6.12+, modprobe tls
- Setup doc: `bare-metal/SETUP.md` (done)

### Benchmarks
- Same Phase B methodology (kTLS sweep)
- Single port first (25 Gbps), then dual-port (50 Gbps aggregate)
- Worker count: 2, 3, 4 on 6C/12T Haswell
- Profile with perf: cache misses, IPC, cycles per packet
- P6 investigation: tcpdump TCP segment analysis

### Expected Outcome
- HD saturates 25 Gbps or hits Haswell CPU ceiling (~15 Gbps)
- TS collapses at much lower rate
- "2014 workstation with $50 NICs crushes modern cloud VMs"

## Phase 2: Code Quality (Parallel with Phase 1)

### CLI Observer Tool
- `hd-watch`: live dashboard for relay monitoring over SSH
- Connects to --metrics-port, polls /debug/workers + /metrics
- Shows: per-worker throughput, queue depths, xfer_drops,
  connections, CPU, memory
- Options:
  - Curses TUI (nice but more code)
  - Plain text refresh via `watch` + shell script (quick)
  - Simple C++ binary using the same HTTP client (consistent)
- **Recommendation**: start with shell script wrapper, upgrade
  to curses TUI if it proves useful

### Repo Cleanup
- Remove dead experimental code and branches
- Clean up build system (CMake presets for release/debug/bench)
- Verify all source files pass cpplint and clang-tidy
- Remove any hardcoded IPs, paths, or credentials
- Verify MIT LICENSE file is present and correct

### Documentation
- **README.md**: architecture overview, build instructions,
  quick start, headline benchmark numbers
- **ARCHITECTURE.md**: three-layer design, SPSC rings, kTLS,
  P3 bitmask — the technical deep-dive
- **BENCHMARKS.md**: methodology summary, results tables,
  links to full reports. Not the full reports themselves —
  a curated overview with the key numbers.
- **DEPLOYING.md**: how to run HD as a DERP relay, config
  flags, worker count guidance, kTLS prerequisites, monitoring

### Cross-Compilation
- ARM64 target: `aarch64-linux-gnu-g++`
- Audit for x86-specific code:
  - `__builtin_ctz` — fine on ARM (maps to rbit+clz)
  - Check for SSE/AVX intrinsics (unlikely but verify)
  - `alignas(64)` — ARM64 cache lines are 64B (128B on
    some chips, but 64B alignment is still correct)
- Test build, run on Graviton spot instance
- Add ARM64 to CI if you set up CI

## Phase 3: Optimizations (After bare metal data)

### P1: Backpressure Hysteresis
- Widen pause/resume threshold gap in data_plane.cc
- Only matters for plain TCP or if bare metal pushes past
  kTLS ceiling
- 30 min code, 1 hr validation

### P6: TCP Segment Packing
- Profile with tcpdump on bare metal under load
- Check segment sizes, pacing, MSG_MORE effectiveness
- Only optimize if profiling shows room
- See OPTIMIZATION_PLAN.md for details

### P5: Adaptive Ring Sizing (if needed)
- Only if bare metal with many workers shows xfer_drops
  even under kTLS

## Phase 4: Publication (2-3 months out)

### Blog Post
- Lead with tunnel test results (user-facing story)
- Back up with synthetic benchmarks (engineering detail)
- Include bare metal numbers
- Cross-cloud comparison (GCP + AWS)
- Architecture section: why io_uring beats goroutines
- Honest reporting: where TS wins, limitations, caveats
- Draft in `~/dev/HD-Report/README.md` (started)

### Target Audiences
1. **Hacker News / systems community**: architecture,
   io_uring vs Go, benchmark methodology
2. **Tailscale / Headscale users**: drop-in replacement,
   performance comparison
3. **Enterprise / relay operators**: cost story, scaling
4. **Potential employers**: portfolio piece

### Outreach (after blog post is published)
- Post to Hacker News
- Share with Tailscale (CEO on Twitter)
- Share with fly.io (they run DERP relays)
- Post to Headscale community (Discord, GitHub)
- r/golang, r/networking, r/selfhosted

### Repo Public Release
- Final review of all code for secrets/credentials
- Clean git history (or squash to clean start)
- GitHub repo with:
  - MIT LICENSE
  - README with build + benchmark summary
  - Architecture docs
  - Benchmark data (or link to separate data repo)
  - CI: build + lint (GitHub Actions)
- Tag v0.1.0

## Phase 5: Future (Post-Release)

### Generic Relay Mode
- Separate binary: `hyper-relay`
- PSK/mTLS auth, minimal framing (no DERP protocol)
- Same io_uring data plane, different protocol frontend
- For OTC.Relay and custom deployments

### Graviton Benchmarks
- ARM64 build on AWS c7g instances
- Same methodology as x86 sweep
- "20% cheaper instances, same performance advantage"

### Peer Scaling Tests
- 20 → 100 → 500 peers
- Tests hash distribution, control plane overhead,
  connection management at scale

### Container Swarm Test
- GKE pods as Tailscale clients, GCE relay
- Dozens of concurrent clients
- "How many peers can one relay serve?"
- Budget-dependent

## Timeline

```
Week of Mar 17:  Bare metal setup + benchmarks
                 Repo cleanup + documentation (parallel)
                 CLI observer (quick version)

Week of Mar 24:  Bare metal data analysis
                 Cross-compilation + Graviton test build
                 P1/P6 if profiling warrants

April:           Blog post drafting
                 Final repo cleanup
                 Peer review of methodology

May:             Blog post published
                 HN submission
                 Outreach
                 v0.1.0 tag

June+:           Generic relay mode
                 Graviton benchmarks
                 Peer scaling / container swarm
```
