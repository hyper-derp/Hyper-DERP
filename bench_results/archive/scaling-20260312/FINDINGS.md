# Scaling Investigation Findings (2026-03-12)

## Root causes found

### 1. Test client sender deadlock
`WriteAll()` in `client.cc` retries EAGAIN in a tight loop. With
`SO_SNDTIMEO=1s`, the sender gets trapped inside `ClientSendPacket`
and never checks its deadline. At 250+ pairs, many senders block
indefinitely. This caused tests to run 100+ seconds instead of ~7
seconds.

### 2. Receiver drain tail
Old drain logic waited for 50 consecutive 100ms idle cycles (5s of
silence). But the relay slowly trickles queued data during drain,
resetting the idle counter each time. With deep relay queues
(kMaxSendQueueDepth=16384), drain took 100+ seconds. Since
throughput = `bytes_recv / traffic_secs`, the inflated denominator
destroyed the measured throughput (e.g. 452 Mbps instead of ~3000).

### 3. Zombie process contamination
`run_scaling_sweep.sh` doesn't reliably kill relay processes between
runs. Found ~30 zombies consuming 23GB RAM on the same pinned cores.
ALL earlier benchmark data from this session was invalid.

### 4. SEND_ZC "4-5x regression" was wrong
The conclusion saved in memory was based on contaminated data. Clean
A/B tests show ZC vs no-ZC difference is small and may actually
favor no-ZC at 100 pairs.

## Fixes applied (uncommitted)

| File | Change |
|------|--------|
| `src/data_plane.cc` | `sodium_memcmp` → `memcmp` (4 places), removed `sodium.h` |
| `tools/derp_scale_test.cc` | Sender: inline write with 100ms timeout, breaks on EAGAIN. Receiver: 2s hard drain deadline |
| `include/hyper_derp/types.h` | `kSendPressureHigh` 4096→32768, `kSendPressureLow` 1024→8192, `kMaxSendQueueDepth` 16384→2048 |

## Results (noisy — PoE2 was running)

The 250-500p collapse is fixed. HD went from 452 Mbps to 3813 Mbps
at 500 pairs. But all data is unreliable due to the game running.
The backpressure threshold tuning needs a clean environment — at
`kSendPressureHigh=32768`, 5-pair tests show 80% loss because
per-peer limits fill before the global threshold triggers.

## Next steps (clean environment needed)

1. Close all apps, let CPU cool, run clean sweep
2. Try `kSendPressureHigh=8192` as a middle ground
3. Clean A/B test of SEND_ZC with the fixed test client
4. Consider adaptive pressure threshold based on active peer count
