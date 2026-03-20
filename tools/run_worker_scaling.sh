#!/bin/bash
# Worker scaling benchmark — investigates worker count
# impact on throughput and xfer_drops on 16 vCPU.
# Implements test-design/WORKER_SCALING_DESIGN.md.
# Runs on the bench-client VM.
#
# Usage:
#   run_worker_scaling.sh [--skip-test2] [--skip-test3]
#                         [--skip-ktls]
set -uo pipefail

# ---- Flags ---------------------------------------------------

SKIP_TEST2=0
SKIP_TEST3=0
SKIP_KTLS=0

while [[ $# -gt 0 ]]; do
  case $1 in
    --skip-test2) SKIP_TEST2=1; shift ;;
    --skip-test3) SKIP_TEST3=1; shift ;;
    --skip-ktls)  SKIP_KTLS=1; shift ;;
    *)            echo "unknown flag: $1"; exit 1 ;;
  esac
done

# ---- Network / relay -----------------------------------------

RELAY=${RELAY:?Set RELAY env var}
RELAY_USER=${RELAY_USER:-worker}
RELAY_KEY=${RELAY_KEY:?Set RELAY_KEY env var}
HD_PORT=3341
TS_PORT=3340
HD_METRICS_PORT=9090
SIZE=1400

# ---- Test 1: Worker count sweep ------------------------------

WORKER_COUNTS="2 4 6 8"
TEST1_RATES="5000 7500 10000 15000 20000"
KTLS_WORKERS="4 8"
RUNS=10

# ---- Test 2: 20G collapse -----------------------------------

COLLAPSE_WORKERS="4 8"
COLLAPSE_RATES="15000 17500 20000 22500 25000"

# ---- Rate sweep params ---------------------------------------

RATE_PEERS=20
RATE_PAIRS=10
RATE_DURATION=15

# ---- Output --------------------------------------------------

OUT=/tmp/bench_worker_scaling
rm -rf "$OUT"
mkdir -p "$OUT"/{rate,cpu,workers,perf,diag}

# ---- Helpers -------------------------------------------------

log() { echo "[$(date +%H:%M:%S)] $*"; }

relay_cmd() {
  timeout 30 ssh -i "$RELAY_KEY" \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    -o ConnectTimeout=10 \
    -o LogLevel=ERROR \
    "${RELAY_USER}@${RELAY}" "$1" 2>/dev/null || true
}

relay_out() {
  timeout 30 ssh -i "$RELAY_KEY" \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    -o ConnectTimeout=10 \
    -o LogLevel=ERROR \
    "${RELAY_USER}@${RELAY}" "$1" 2>/dev/null
}

relay_scp() {
  scp -i "$RELAY_KEY" \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    -o LogLevel=ERROR \
    "${RELAY_USER}@${RELAY}:$1" "$2" 2>/dev/null || true
}

# ---- Server management --------------------------------------

start_hd() {
  local workers=$1
  local tls_flags="${2:-}"
  log "  [relay] starting HD (workers=$workers)..."
  relay_cmd "sudo pkill -9 hyper-derp 2>/dev/null; sleep 1"
  relay_cmd "setsid hyper-derp --port ${HD_PORT} \
    --workers $workers \
    --metrics-port $HD_METRICS_PORT \
    --debug-endpoints \
    $tls_flags \
    </dev/null >/tmp/hd.log 2>&1 &"
  sleep 4
  local ok
  ok=$(relay_out "ss -tlnp | grep -c ${HD_PORT}" || echo 0)
  if [ "${ok:-0}" -lt 1 ]; then
    log "  WARNING: HD not listening, retrying..."
    relay_cmd "setsid hyper-derp --port ${HD_PORT} \
      --workers $workers \
      --metrics-port $HD_METRICS_PORT \
      --debug-endpoints \
      $tls_flags \
      </dev/null >/tmp/hd.log 2>&1 &"
    sleep 4
  fi
}

stop_hd() {
  relay_cmd "sudo pkill -9 hyper-derp 2>/dev/null"
  sleep 2
}

start_ts() {
  log "  [relay] starting TS derper..."
  relay_cmd "sudo pkill -9 derper 2>/dev/null; sleep 1"
  relay_cmd "setsid derper -dev -a :${TS_PORT} \
    </dev/null >/tmp/ts.log 2>&1 &"
  sleep 4
  local ok
  ok=$(relay_out "ss -tlnp | grep -c ${TS_PORT}" || echo 0)
  if [ "${ok:-0}" -lt 1 ]; then
    log "  WARNING: TS not listening, retrying..."
    relay_cmd "setsid derper -dev -a :${TS_PORT} \
      </dev/null >/tmp/ts.log 2>&1 &"
    sleep 4
  fi
}

stop_ts() {
  relay_cmd "sudo pkill -9 derper 2>/dev/null"
  sleep 2
}

drop_caches() {
  relay_cmd "sync; echo 3 | sudo tee \
    /proc/sys/vm/drop_caches >/dev/null"
  sleep 2
}

# ---- CPU monitoring ------------------------------------------

start_monitor() {
  local server_bin=$1
  local phase_label=$2
  relay_cmd "mkdir -p /tmp/cpu"
  relay_cmd "nohup bash -c 'PID=\$(pgrep -x $server_bin); \
    [ -n \"\$PID\" ] && pidstat -p \$PID -r -u 1 \
    > /tmp/cpu/${phase_label}_pidstat.txt 2>&1' &"
  relay_cmd "nohup mpstat -P ALL 1 \
    > /tmp/cpu/${phase_label}_mpstat.txt 2>&1 &"
  relay_cmd "nohup bash -c 'PID=\$(pgrep -x $server_bin); \
    while kill -0 \$PID 2>/dev/null; do \
      echo \"\$(date +%s) \$(ps -o rss= -p \$PID)\"; \
      sleep 1; \
    done > /tmp/cpu/${phase_label}_rss.txt 2>&1' &"
}

stop_monitor() {
  relay_cmd "pkill -f pidstat 2>/dev/null; \
    pkill -f mpstat 2>/dev/null"
  sleep 1
}

download_cpu() {
  local phase_label=$1
  for suffix in pidstat mpstat rss; do
    relay_scp "/tmp/cpu/${phase_label}_${suffix}.txt" \
      "$OUT/cpu/"
  done
}

# ---- HD worker stats -----------------------------------------

hd_worker_stats() {
  local label=$1
  relay_out "curl -s http://localhost:${HD_METRICS_PORT}\
/debug/workers" > "$OUT/workers/${label}.json" || true
}

# Poll /debug/workers at 1s intervals. Runs in background.
# Usage: poll_worker_stats <label> <duration> &
poll_worker_stats() {
  local label=$1 duration=$2
  for t in $(seq 0 "$duration"); do
    relay_out "curl -s http://localhost:${HD_METRICS_PORT}\
/debug/workers" \
      > "$OUT/workers/${label}_t${t}.json" 2>/dev/null \
      || true
    sleep 1
  done
}

# ---- perf stat on relay (one run) ----------------------------

run_perf_stat() {
  local label=$1 duration=${2:-15}
  relay_cmd "PID=\$(pgrep -x hyper-derp); \
    [ -n \"\$PID\" ] && sudo perf stat \
    -e cache-misses,cache-references,instructions,\
cycles,context-switches,cpu-migrations \
    -p \$PID -o /tmp/perf_${label}.txt \
    -- sleep $duration 2>&1 &"
}

download_perf() {
  local label=$1
  relay_scp "/tmp/perf_${label}.txt" "$OUT/perf/"
}

# ---- Rate run ------------------------------------------------

run_rate() {
  local label=$1 port=$2 rate=$3
  local tls_flag="${4:-}"
  local outfile="$OUT/rate/${label}.json"
  local tmpfile="${outfile}.tmp"

  local extra=""
  if [ "$tls_flag" = "--tls" ]; then
    extra="--tls --insecure"
  fi

  derp-scale-test \
    --host "$RELAY" --port "$port" \
    --peers "$RATE_PEERS" --active-pairs "$RATE_PAIRS" \
    --duration "$RATE_DURATION" --msg-size "$SIZE" \
    --rate-mbps "$rate" \
    $extra \
    --json 2>/dev/null > "$tmpfile" || true

  sed -n '/{/,/}/p' "$tmpfile" > "$outfile"
  rm -f "$tmpfile"

  python3 -c "
import json
try:
  d = json.load(open('$outfile'))
  print(f'  {d[\"throughput_mbps\"]:.0f} Mbps, '
        f'{d[\"message_loss_pct\"]:.1f}% loss')
except: print('  FAILED')
" 2>/dev/null
  sleep 2
}

# ---- System info ---------------------------------------------

collect_system_info() {
  log "Collecting system info..."
  relay_out "lscpu" > "$OUT/relay_lscpu.txt" 2>/dev/null
  relay_out "sysctl net.core.rmem_max \
    net.core.wmem_max \
    net.core.somaxconn \
    net.ipv4.tcp_wmem \
    net.ipv4.tcp_rmem 2>/dev/null" \
    > "$OUT/relay_sysctl.txt" 2>/dev/null
  relay_out "hyper-derp --version 2>&1 || true" \
    > "$OUT/hd_version.txt" 2>/dev/null
  relay_out "go version -m \$(which derper) 2>&1 || true" \
    > "$OUT/ts_version.txt" 2>/dev/null
  echo "test: worker_scaling" > "$OUT/system_info.txt"
  echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)" \
    >> "$OUT/system_info.txt"
  echo "relay: $RELAY" >> "$OUT/system_info.txt"
  echo "spsc_ring_size: 16384" >> "$OUT/system_info.txt"
}

# ---- Validation ----------------------------------------------

validate_results() {
  log "=== Validation ==="
  python3 -c "
import json, glob, statistics
from pathlib import Path

out = Path('$OUT')
errors = []

rate_files = list((out / 'rate').glob('*.json'))
valid_rate = [f for f in rate_files
              if f.stat().st_size > 10]
print(f'Rate files: {len(valid_rate)} valid')

# Check for empty/truncated files.
for f in rate_files:
    if f.stat().st_size < 10:
        errors.append(f'Empty/truncated: {f.name}')

# CV check at each worker count + rate combo.
import re
combos = {}
for f in valid_rate:
    m = re.match(
        r'(hd|ktls|collapse|ts)_(\d+w_)?(\d+)_r\d+\.json',
        f.name)
    if not m:
        continue
    key = f'{m.group(1)}_{m.group(2) or \"\"}{m.group(3)}'
    combos.setdefault(key, []).append(f)

for key, files in sorted(combos.items()):
    tps = []
    for f in files:
        try:
            d = json.loads(f.read_text())
            tps.append(d['throughput_mbps'])
        except:
            pass
    if len(tps) > 2:
        mean = statistics.mean(tps)
        if mean > 0:
            cv = statistics.stdev(tps) / mean * 100
            if cv > 20:
                errors.append(f'{key}: CV={cv:.1f}%')

# Worker stats file count.
ws_files = list((out / 'workers').glob('*.json'))
valid_ws = [f for f in ws_files if f.stat().st_size > 10]
print(f'Worker stat files: {len(valid_ws)}')

# Perf files.
perf_files = list((out / 'perf').glob('*.txt'))
print(f'Perf stat files: {len(perf_files)}')

if errors:
    print(f'\n{len(errors)} issues:')
    for e in errors:
        print(f'  - {e}')
else:
    print('All checks passed.')
" 2>/dev/null
}

# ==============================================================
# MAIN
# ==============================================================

log "========================================="
log "  WORKER SCALING INVESTIGATION"
log "  Workers: $WORKER_COUNTS"
log "  Rates:   $TEST1_RATES"
log "========================================="
log ""

collect_system_info

# Ensure sysstat is installed on relay.
relay_cmd "which pidstat >/dev/null 2>&1 || \
  sudo apt-get install -y -qq sysstat >/dev/null 2>&1"
relay_cmd "sudo rm -rf /tmp/cpu; mkdir -p /tmp/cpu"

# ---- Test 1a: Plain TCP worker sweep -------------------------

log "=== Test 1a: Plain TCP Worker Sweep ==="

for w in $WORKER_COUNTS; do
  log ""
  log "--- ${w} workers (plain TCP) ---"
  drop_caches
  start_hd "$w"
  hd_worker_stats "t1_${w}w_before"
  start_monitor "hyper-derp" "t1_${w}w"

  for rate in $TEST1_RATES; do
    hd_worker_stats "t1_${w}w_${rate}_before"
    for run in $(seq 1 $RUNS); do
      padded=$(printf '%02d' "$run")
      log "  [$run/$RUNS] HD ${w}w @${rate}:"
      run_rate "hd_${w}w_${rate}_r${padded}" \
        "$HD_PORT" "$rate"
    done
    hd_worker_stats "t1_${w}w_${rate}_after"
  done

  stop_monitor
  download_cpu "t1_${w}w"
  hd_worker_stats "t1_${w}w_after"

  # perf stat: one 15s run at 15G for this worker count.
  log "  perf stat: ${w}w @15G..."
  run_perf_stat "t1_${w}w_15g" 15
  run_rate "hd_${w}w_15000_perf" "$HD_PORT" 15000
  sleep 2
  download_perf "t1_${w}w_15g"

  stop_hd
done

# ---- Test 1b: TS control runs --------------------------------

log ""
log "=== Test 1b: TS Control ==="
drop_caches
start_ts
start_monitor "derper" "t1_ts"

for rate in $TEST1_RATES; do
  for run in $(seq 1 $RUNS); do
    padded=$(printf '%02d' "$run")
    log "  [$run/$RUNS] TS @${rate}:"
    run_rate "ts_${rate}_r${padded}" "$TS_PORT" "$rate"
  done
done

stop_monitor
download_cpu "t1_ts"
stop_ts

# ---- Test 1c: kTLS for 4w and 8w ----------------------------

if [ "$SKIP_KTLS" -eq 0 ]; then
  log ""
  log "=== Test 1c: kTLS Worker Sweep ==="

  # Ensure kTLS kernel module is loaded on both VMs.
  relay_cmd "sudo modprobe tls 2>/dev/null || true"
  sudo modprobe tls 2>/dev/null || true

  for w in $KTLS_WORKERS; do
    log ""
    log "--- ${w} workers (kTLS) ---"
    drop_caches
    start_hd "$w" \
      "--tls-cert /tmp/cert.pem --tls-key /tmp/key.pem"
    hd_worker_stats "t1_ktls_${w}w_before"
    start_monitor "hyper-derp" "t1_ktls_${w}w"

    for rate in $TEST1_RATES; do
      hd_worker_stats "t1_ktls_${w}w_${rate}_before"
      for run in $(seq 1 $RUNS); do
        padded=$(printf '%02d' "$run")
        log "  [$run/$RUNS] kTLS ${w}w @${rate}:"
        run_rate "ktls_${w}w_${rate}_r${padded}" \
          "$HD_PORT" "$rate" "--tls"
      done
      hd_worker_stats "t1_ktls_${w}w_${rate}_after"
    done

    stop_monitor
    download_cpu "t1_ktls_${w}w"
    hd_worker_stats "t1_ktls_${w}w_after"
    stop_hd
  done
fi

# ---- Test 2: 20G Collapse Investigation ---------------------

if [ "$SKIP_TEST2" -eq 0 ]; then
  log ""
  log "=== Test 2a: Collapse Investigation (plain TCP) ==="

  for w in $COLLAPSE_WORKERS; do
    log ""
    log "--- ${w} workers (collapse, plain TCP) ---"
    drop_caches
    start_hd "$w"
    hd_worker_stats "t2_${w}w_before"
    start_monitor "hyper-derp" "t2_${w}w"

    for rate in $COLLAPSE_RATES; do
      hd_worker_stats "t2_${w}w_${rate}_before"
      for run in $(seq 1 $RUNS); do
        padded=$(printf '%02d' "$run")
        log "  [$run/$RUNS] collapse ${w}w @${rate}:"
        # Poll worker stats during the run.
        poll_worker_stats \
          "t2_${w}w_${rate}_r${padded}" \
          "$RATE_DURATION" &
        POLL_PID=$!
        run_rate "collapse_${w}w_${rate}_r${padded}" \
          "$HD_PORT" "$rate"
        kill "$POLL_PID" 2>/dev/null || true
        wait "$POLL_PID" 2>/dev/null || true
      done
      hd_worker_stats "t2_${w}w_${rate}_after"
    done

    stop_monitor
    download_cpu "t2_${w}w"
    hd_worker_stats "t2_${w}w_after"
    stop_hd
  done

  if [ "$SKIP_KTLS" -eq 0 ]; then
    log ""
    log "=== Test 2b: Collapse (kTLS, 8 workers) ==="
    relay_cmd "sudo modprobe tls 2>/dev/null || true"
    sudo modprobe tls 2>/dev/null || true
    drop_caches
    start_hd 8 \
      "--tls-cert /tmp/cert.pem --tls-key /tmp/key.pem"
    hd_worker_stats "t2_ktls_8w_before"
    start_monitor "hyper-derp" "t2_ktls_8w"

    for rate in $COLLAPSE_RATES; do
      hd_worker_stats "t2_ktls_8w_${rate}_before"
      for run in $(seq 1 $RUNS); do
        padded=$(printf '%02d' "$run")
        log "  [$run/$RUNS] collapse kTLS 8w @${rate}:"
        poll_worker_stats \
          "t2_ktls_8w_${rate}_r${padded}" \
          "$RATE_DURATION" &
        POLL_PID=$!
        run_rate "collapse_ktls_8w_${rate}_r${padded}" \
          "$HD_PORT" "$rate" "--tls"
        kill "$POLL_PID" 2>/dev/null || true
        wait "$POLL_PID" 2>/dev/null || true
      done
      hd_worker_stats "t2_ktls_8w_${rate}_after"
    done

    stop_monitor
    download_cpu "t2_ktls_8w"
    hd_worker_stats "t2_ktls_8w_after"
    stop_hd
  fi
fi

# ---- Test 3: Diagnostics ------------------------------------

if [ "$SKIP_TEST3" -eq 0 ]; then
  log ""
  log "=== Test 3: Diagnostics ==="

  # 3a: perf trace eventfd (4w vs 8w at 15G).
  for w in 4 8; do
    log "  perf trace: ${w}w @15G..."
    start_hd "$w"
    sleep 2
    relay_cmd "PID=\$(pgrep -x hyper-derp); \
      [ -n \"\$PID\" ] && sudo timeout 8 perf trace \
      -e write,read -p \$PID -- sleep 5 \
      > /tmp/perf_trace_${w}w.txt 2>&1 &"
    run_rate "diag_eventfd_${w}w" "$HD_PORT" 15000
    sleep 7
    relay_scp "/tmp/perf_trace_${w}w.txt" "$OUT/diag/"
    stop_hd
  done

  # 3b: perf stat cache (4w vs 8w at 15G).
  for w in 4 8; do
    log "  perf stat cache: ${w}w @15G..."
    start_hd "$w"
    sleep 2
    relay_cmd "PID=\$(pgrep -x hyper-derp); \
      [ -n \"\$PID\" ] && sudo perf stat \
      -e cache-misses,cache-references,\
L1-dcache-load-misses,LLC-load-misses,LLC-store-misses \
      -p \$PID -o /tmp/perf_cache_${w}w.txt \
      -- sleep 10 2>&1 &"
    run_rate "diag_cache_${w}w" "$HD_PORT" 15000
    sleep 12
    relay_scp "/tmp/perf_cache_${w}w.txt" "$OUT/diag/"
    stop_hd
  done

  # 3c: Hash distribution test (40 peers, 8 workers).
  log "  hash test: 40 peers, 8w @15G..."
  start_hd 8
  for run in 1 2 3 4 5; do
    log "    [$run/5] 40-peer 8w @15G:"
    outfile="$OUT/rate/diag_40peer_8w_r${run}.json"
    tmpfile="${outfile}.tmp"
    derp-scale-test \
      --host "$RELAY" --port "$HD_PORT" \
      --peers 40 --active-pairs 20 \
      --duration "$RATE_DURATION" --msg-size "$SIZE" \
      --rate-mbps 15000 \
      --json 2>/dev/null > "$tmpfile" || true
    sed -n '/{/,/}/p' "$tmpfile" > "$outfile"
    rm -f "$tmpfile"
    python3 -c "
import json
try:
  d = json.load(open('$outfile'))
  print(f'    {d[\"throughput_mbps\"]:.0f} Mbps, '
        f'{d[\"message_loss_pct\"]:.1f}% loss')
except: print('    FAILED')
" 2>/dev/null
    hd_worker_stats "diag_40peer_8w_r${run}"
    sleep 2
  done
  stop_hd
fi

# ---- Validation ----------------------------------------------

log ""
validate_results

# ---- Done ----------------------------------------------------

rate_count=$(find "$OUT/rate" -name "*.json" \
  -size +0c | wc -l)
ws_count=$(find "$OUT/workers" -name "*.json" \
  -size +0c | wc -l)
echo "complete $(date -u +%Y-%m-%dT%H:%M:%S%z)" \
  > "$OUT/DONE"
log ""
log "=== Worker scaling complete ==="
log "Rate files:   $rate_count"
log "Worker stats: $ws_count"
log "Output:       $OUT"
