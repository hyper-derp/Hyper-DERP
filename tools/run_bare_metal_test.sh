#!/usr/bin/env bash
# Bare-metal breakdown test: Hyper-DERP vs Tailscale derper.
#
# Runs relay and clients on the same host using localhost TCP.
# No VM overhead — measures pure data-plane performance.
#
# CPU layout (i5-13600KF):
#   P-cores 0-5 (logical 0-11, HT, 5.1 GHz)
#   E-cores 6-13 (logical 12-19, 3.9 GHz)
#
# Pinning strategy:
#   Relay workers:  P-cores 2-3 (logical 4-7)
#   Test client:    E-cores 6-9 (logical 12-15)
#   Kernel/IRQ:     remaining cores
#
# Usage: sudo ./run_bare_metal_test.sh [output_dir]
#   Must run as root for CPU pinning + perf + sysctl.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build"
RELAY="${BUILD_DIR}/hyper-derp"
SCALE="${BUILD_DIR}/tools/derp-scale-test"
CLIENT="${BUILD_DIR}/tools/derp-test-client"
DERPER="/home/karl/go/bin/derper"

TS_PORT=3340
HD_PORT=3341
WORKERS=2

# Relay pinned to P-cores 2-3 (logical 4,5,6,7).
HD_CORES="4,5"
# Client pinned to E-cores (logical 12-15).
CLIENT_CORES="12,13,14,15"

# Scale test params.
PEERS=20
PAIRS=10
DURATION=10
SIZE=1400

# Rate sweep — go higher on bare metal.
RATES="100 500 1000 2000 5000 10000 20000 50000"

# Latency test.
PING_COUNT=5000
PING_WARMUP=500

OUTPUT_DIR="${1:-${PROJECT_DIR}/bench_results/bare_metal-$(date +%Y%m%d)}"
mkdir -p "$OUTPUT_DIR"

log() { echo "[$(date +%H:%M:%S)] $*"; }

PIDS=()
cleanup() {
  for pid in "${PIDS[@]}"; do
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  done
  # Restore governor.
  for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo powersave > "$cpu" 2>/dev/null || true
  done
}
trap cleanup EXIT

wait_port() {
  local port=$1 timeout=${2:-10}
  local deadline=$((SECONDS + timeout))
  while ! timeout 1 bash -c \
      "echo >/dev/tcp/127.0.0.1/$port" 2>/dev/null; do
    if ((SECONDS >= deadline)); then
      log "ERROR: port $port not ready after ${timeout}s"
      return 1
    fi
    sleep 0.1
  done
}

# -- System tuning (requires root) --

tune_system() {
  log "Tuning system for benchmarking..."

  # Performance governor on all cores.
  for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo performance > "$cpu" 2>/dev/null || true
  done

  # Disable NMI watchdog (reduces jitter).
  echo 0 > /proc/sys/kernel/nmi_watchdog 2>/dev/null || true

  # Increase TCP buffers.
  sysctl -w net.core.wmem_max=16777216 >/dev/null
  sysctl -w net.core.rmem_max=16777216 >/dev/null
  sysctl -w net.core.wmem_default=262144 >/dev/null
  sysctl -w net.core.rmem_default=262144 >/dev/null
  sysctl -w net.ipv4.tcp_wmem="4096 262144 16777216" >/dev/null
  sysctl -w net.ipv4.tcp_rmem="4096 262144 16777216" >/dev/null

  # Increase loopback MTU for less fragmentation.
  ip link set lo mtu 65536 2>/dev/null || true

  # Increase somaxconn for burst connects.
  sysctl -w net.core.somaxconn=4096 >/dev/null

  log "  governor=performance, NMI off, TCP buffers=16MB"
}

# -- Collect system info --

collect_sysinfo() {
  {
    echo "date: $(date -Iseconds)"
    echo "kernel: $(uname -r)"
    echo "cpu: $(lscpu | grep 'Model name' | sed 's/.*: *//')"
    echo "cores: $(nproc)"
    echo "governor: $(cat /sys/devices/system/cpu/cpu4/cpufreq/scaling_governor)"
    echo "relay_cores: $HD_CORES"
    echo "client_cores: $CLIENT_CORES"
    echo "workers: $WORKERS"
    echo "peers: $PEERS"
    echo "pairs: $PAIRS"
    echo "duration: ${DURATION}s"
    echo "size: ${SIZE}B"
    echo "rates: $RATES"
    echo "tcp_wmem: $(cat /proc/sys/net/ipv4/tcp_wmem)"
    echo "wmem_max: $(cat /proc/sys/net/core/wmem_max)"
    echo "lo_mtu: $(ip link show lo | grep mtu | awk '{print $5}')"
  } > "${OUTPUT_DIR}/system_info.txt"
}

# -- Scale test runner --

run_scale() {
  local label=$1 port=$2 rate=$3
  local rate_flag="--rate-mbps $rate"

  log "  scale: ${rate:-unlimited} Mbps ..."

  local outfile="${OUTPUT_DIR}/${label}.json"
  local raw
  raw=$(taskset -c "$CLIENT_CORES" \
    "$SCALE" --host 127.0.0.1 --port "$port" \
    --peers "$PEERS" --active-pairs "$PAIRS" \
    --duration "$DURATION" --msg-size "$SIZE" \
    $rate_flag --json 2>/dev/null) || true

  # Extract JSON block from output.
  echo "$raw" | sed -n '/{/,/}/p' > "$outfile"

  # Print summary.
  python3 -c "
import json, sys
try:
    d = json.load(open('$outfile'))
    print(f'    {d[\"throughput_mbps\"]:.1f} Mbps, '
          f'{d[\"message_loss_pct\"]:.2f}% loss, '
          f'{d.get(\"send_errors\", 0)} errs')
except:
    print('    PARSE ERROR')
" 2>/dev/null
  sleep 2
}

# -- Latency test runner --

run_latency() {
  local label=$1 port=$2
  log "  latency 1400B ..."

  local echo_err
  echo_err=$(mktemp)

  taskset -c "$CLIENT_CORES" \
    "$CLIENT" --host 127.0.0.1 --port "$port" \
    --mode echo \
    --count $((PING_COUNT + PING_WARMUP + 500)) \
    --timeout 60000 \
    2>"$echo_err" &
  local ECHO_PID=$!
  PIDS+=("$ECHO_PID")
  sleep 2

  local echo_key
  echo_key=$(grep '^pubkey:' "$echo_err" | awk '{print $2}')
  rm -f "$echo_err"

  if [[ -z "$echo_key" ]]; then
    log "    ERROR: no echo key"
    kill "$ECHO_PID" 2>/dev/null || true
    wait "$ECHO_PID" 2>/dev/null || true
    return 1
  fi

  local raw_file
  raw_file=$(mktemp)

  taskset -c "$CLIENT_CORES" \
    "$CLIENT" --host 127.0.0.1 --port "$port" \
    --mode ping --count "$PING_COUNT" --size "$SIZE" \
    --dst-key "$echo_key" --warmup "$PING_WARMUP" \
    --json --raw-latency \
    --label "$label" --timeout 60000 \
    2>/dev/null > "$raw_file" || true

  kill "$ECHO_PID" 2>/dev/null || true
  wait "$ECHO_PID" 2>/dev/null || true

  # Extract JSON.
  python3 -c "
import json
text = open('$raw_file').read()
start = text.index('{')
depth = 0
for i in range(start, len(text)):
    if text[i] == '{': depth += 1
    elif text[i] == '}': depth -= 1
    if depth == 0:
        data = json.loads(text[start:i+1])
        with open('${OUTPUT_DIR}/${label}.json', 'w') as f:
            json.dump(data, f, indent=2)
        l = data['latency_ns']
        print(f'    p50={l[\"p50\"]/1000:.0f}us '
              f'p90={l[\"p90\"]/1000:.0f}us '
              f'p99={l[\"p99\"]/1000:.0f}us '
              f'pps={data[\"throughput_pps\"]:.0f}')
        break
" 2>/dev/null
  rm -f "$raw_file"
}

# -- Loaded latency: measure latency WHILE throughput is running --

run_loaded_latency() {
  local label=$1 port=$2 bg_rate=$3
  log "  loaded latency @ ${bg_rate} Mbps background ..."

  # Start background traffic.
  taskset -c 14,15 \
    "$SCALE" --host 127.0.0.1 --port "$port" \
    --peers 10 --active-pairs 5 \
    --duration 15 --msg-size "$SIZE" \
    --rate-mbps "$bg_rate" --json \
    2>/dev/null >/dev/null &
  local BG_PID=$!
  PIDS+=("$BG_PID")
  sleep 3  # Let traffic stabilize.

  # Measure latency on separate peers.
  local echo_err
  echo_err=$(mktemp)

  taskset -c 12 \
    "$CLIENT" --host 127.0.0.1 --port "$port" \
    --mode echo \
    --count 3000 --timeout 30000 \
    2>"$echo_err" &
  local ECHO_PID=$!
  PIDS+=("$ECHO_PID")
  sleep 2

  local echo_key
  echo_key=$(grep '^pubkey:' "$echo_err" | awk '{print $2}')
  rm -f "$echo_err"

  if [[ -z "$echo_key" ]]; then
    log "    ERROR: no echo key"
    kill "$ECHO_PID" "$BG_PID" 2>/dev/null || true
    wait "$ECHO_PID" "$BG_PID" 2>/dev/null || true
    return 1
  fi

  local raw_file
  raw_file=$(mktemp)

  taskset -c 13 \
    "$CLIENT" --host 127.0.0.1 --port "$port" \
    --mode ping --count 2000 --size "$SIZE" \
    --dst-key "$echo_key" --warmup 200 \
    --json --raw-latency \
    --label "$label" --timeout 30000 \
    2>/dev/null > "$raw_file" || true

  kill "$ECHO_PID" 2>/dev/null || true
  wait "$ECHO_PID" 2>/dev/null || true

  # Wait for background traffic to finish.
  wait "$BG_PID" 2>/dev/null || true

  python3 -c "
import json
text = open('$raw_file').read()
start = text.index('{')
depth = 0
for i in range(start, len(text)):
    if text[i] == '{': depth += 1
    elif text[i] == '}': depth -= 1
    if depth == 0:
        data = json.loads(text[start:i+1])
        with open('${OUTPUT_DIR}/${label}.json', 'w') as f:
            json.dump(data, f, indent=2)
        l = data['latency_ns']
        print(f'    p50={l[\"p50\"]/1000:.0f}us '
              f'p90={l[\"p90\"]/1000:.0f}us '
              f'p99={l[\"p99\"]/1000:.0f}us '
              f'pps={data[\"throughput_pps\"]:.0f}')
        break
" 2>/dev/null
  rm -f "$raw_file"
}

# -- Perf stat capture --

run_perf_stat() {
  local label=$1 port=$2 rate=$3 pid=$4
  log "  perf stat @ ${rate} Mbps ..."

  # Start perf stat on relay PID.
  perf stat -e task-clock,cycles,instructions,cache-misses,\
cache-references,context-switches,cpu-migrations \
    -p "$pid" \
    -o "${OUTPUT_DIR}/${label}_perf.txt" -- \
    sleep "$DURATION" 2>/dev/null &
  local PERF_PID=$!

  # Run traffic.
  taskset -c "$CLIENT_CORES" \
    "$SCALE" --host 127.0.0.1 --port "$port" \
    --peers "$PEERS" --active-pairs "$PAIRS" \
    --duration "$DURATION" --msg-size "$SIZE" \
    --rate-mbps "$rate" --json \
    2>/dev/null | sed -n '/{/,/}/p' \
    > "${OUTPUT_DIR}/${label}.json"

  wait "$PERF_PID" 2>/dev/null || true

  # Print perf summary.
  grep -E 'instructions|context-switch' \
    "${OUTPUT_DIR}/${label}_perf.txt" 2>/dev/null || true
  sleep 2
}

# =====================================================================

test_ts() {
  log ""
  log "============ Tailscale derper (bare metal) ============"

  "$DERPER" -dev 2>"${OUTPUT_DIR}/derper.log" &
  local TS_PID=$!
  PIDS+=("$TS_PID")

  if ! wait_port "$TS_PORT" 5; then
    log "ERROR: derper failed to start"
    return 1
  fi
  log "derper started (pid=$TS_PID)"

  # Throughput sweep.
  for rate in $RATES; do
    run_scale "ts_${rate}mbps" "$TS_PORT" "$rate"
  done

  # Idle latency.
  run_latency "ts_lat_1400B" "$TS_PORT"

  # Loaded latency.
  for bg in 500 2000; do
    run_loaded_latency "ts_loaded_lat_${bg}mbps" \
      "$TS_PORT" "$bg"
    sleep 2
  done

  # Perf counters at saturation.
  run_perf_stat "ts_perf_5000" "$TS_PORT" 5000 "$TS_PID"

  kill "$TS_PID" 2>/dev/null || true
  wait "$TS_PID" 2>/dev/null || true
  sleep 2
}

test_hd() {
  log ""
  log "============ Hyper-DERP (bare metal) ============"

  "$RELAY" --port "$HD_PORT" --workers "$WORKERS" \
    --pin-workers "$HD_CORES" \
    2>"${OUTPUT_DIR}/hyper-derp.log" &
  local HD_PID=$!
  PIDS+=("$HD_PID")

  if ! wait_port "$HD_PORT" 5; then
    log "ERROR: hyper-derp failed to start"
    return 1
  fi
  log "hyper-derp started (pid=$HD_PID)"

  # Throughput sweep.
  for rate in $RATES; do
    run_scale "hd_${rate}mbps" "$HD_PORT" "$rate"
  done

  # Idle latency.
  run_latency "hd_lat_1400B" "$HD_PORT"

  # Loaded latency.
  for bg in 500 2000; do
    run_loaded_latency "hd_loaded_lat_${bg}mbps" \
      "$HD_PORT" "$bg"
    sleep 2
  done

  # Perf counters at saturation.
  run_perf_stat "hd_perf_5000" "$HD_PORT" 5000 "$HD_PID"

  kill "$HD_PID" 2>/dev/null || true
  wait "$HD_PID" 2>/dev/null || true
  sleep 2
}

main() {
  if [[ "$(id -u)" -ne 0 ]]; then
    log "ERROR: must run as root (for CPU pinning + perf)"
    exit 1
  fi

  for bin in "$RELAY" "$SCALE" "$CLIENT" "$DERPER"; do
    if [[ ! -x "$bin" ]]; then
      log "ERROR: missing: $bin"
      exit 1
    fi
  done

  log "Bare-metal breakdown test"
  log "  CPU: $(lscpu | grep 'Model name' | sed 's/.*: *//')"
  log "  Relay cores: $HD_CORES (P-cores)"
  log "  Client cores: $CLIENT_CORES (E-cores)"
  log "  Rates: $RATES Mbps"
  log "  Duration: ${DURATION}s per point"
  log "  Output: $OUTPUT_DIR"

  tune_system
  collect_sysinfo

  # test_ts  # Already collected.
  test_hd

  log ""
  log "=========================================="
  log "All tests complete: $OUTPUT_DIR"
  log ""

  # Quick summary table.
  python3 -c "
import json, os, glob

rates = [100, 500, 1000, 2000, 5000, 10000, 20000, 50000, 0]
dir = '$OUTPUT_DIR'

print(f'{\"Rate\":>10} | {\"TS Mbps\":>10} {\"TS Loss\":>10} | {\"HD Mbps\":>10} {\"HD Loss\":>10}')
print('-' * 65)
for rate in rates:
    label = f'{rate}mbps' if rate > 0 else 'unlimitedmbps'
    ts = hd = None
    try:
        ts = json.load(open(os.path.join(dir, f'ts_{label}.json')))
    except: pass
    try:
        hd = json.load(open(os.path.join(dir, f'hd_{label}.json')))
    except: pass
    rl = 'unlimited' if rate == 0 else str(rate)
    ts_tp = f'{ts[\"throughput_mbps\"]:.1f}' if ts else '-'
    ts_loss = f'{ts[\"message_loss_pct\"]:.2f}%' if ts else '-'
    hd_tp = f'{hd[\"throughput_mbps\"]:.1f}' if hd else '-'
    hd_loss = f'{hd[\"message_loss_pct\"]:.2f}%' if hd else '-'
    print(f'{rl:>10} | {ts_tp:>10} {ts_loss:>10} | {hd_tp:>10} {hd_loss:>10}')
" 2>/dev/null || true
}

main
