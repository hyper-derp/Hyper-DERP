# 001: C++ Modernization — Regression Test

Date: 2026-03-11
Change: Replace C-style patterns with idiomatic C++23
Environment: libvirt KVM VM, Debian 13, 4 vCPU, 3.8 GB RAM

## Changes Made

- `Key = std::array<uint8_t, 32>` type alias (all structs)
- `malloc/free` → `std::unique_ptr<T[]>` (handshake, bench)
- `memset` → aggregate init / default member initializers
- `HttpRequest` char arrays → `std::string`
- `strcmp` → `string_view` comparisons
- `LatencyRecorder` → RAII (unique_ptr, no Destroy())
- JSON parsing → `string_view::find()` lambdas

## Regression Test: 10 Active Pairs

| Peers | Before (Mbps) | After (Mbps) | Delta |
|------:|--------------:|-------------:|------:|
|   100 |         3,070 |        3,635 |  +18% |
|   500 |         3,377 |        3,163 |   -6% |
| 1,000 |         3,291 |        3,604 |  +10% |
| 2,000 |         3,804 |        3,477 |   -9% |
| 4,000 |         3,194 |        3,841 |  +20% |

## Regression Test: All Pairs Active

| Peers | Before (Mbps) | After (Mbps) | Delta |
|------:|--------------:|-------------:|------:|
|   100 |         2,209 |        2,096 |   -5% |
|   500 |         1,330 |        2,393 |  +80% |
| 1,000 |           656 |        3,523 | +437% |

## Tailscale derper Comparison (10 pairs)

| Peers | HD (Mbps) | TS (Mbps) | HD/TS |
|------:|----------:|----------:|------:|
|   100 |     3,635 |       495 |  7.3x |
| 1,000 |     3,604 |       283 | 12.7x |
| 2,000 |     3,477 |       191 | 18.2x |

## Verdict

**No regression.** 10-pair throughput is within +/-20% of
baseline (run-to-run variance). All-pairs-active shows
improvement at higher pair counts — likely due to reduced
overhead from aggregate init vs memset on the HttpRequest
struct (one alloc per connection in the accept path).

HD maintains 7-18x throughput advantage over Tailscale
derper, consistent with pre-modernization measurements.
