#!/bin/bash
# Gap-fill rerun for worker scaling test.
# Fills in: Test 1c kTLS, contaminated Test 2a 4w data,
# and Test 3 hash test.
# Runs on the bench-client VM.
set -uo pipefail

# ---- Network / relay -----------------------------------------

RELAY="${RELAY:?Set RELAY env var (relay IP)}"
RELAY_USER="${RELAY_USER:-worker}"
RELAY_KEY="${RELAY_KEY:?Set RELAY_KEY env var (path to SSH key)}"
HD_PORT=3341
HD_METRICS_PORT=9090
SIZE=1400

# ---- Test params ---------------------------------------------

KTLS_WORKERS="4 8"
TEST1_RATES="5000 7500 10000 15000 20000"
RUNS=10
RATE_PEERS=20
RATE_PAIRS=10
RATE_DURATION=15

# ---- Output --------------------------------------------------

OUT=/tmp/bench_worker_scaling
mkdir -p "$OUT"/{rate,cpu,workers}

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

drop_caches() {
  relay_cmd "sync; echo 3 | sudo tee \
    /proc/sys/vm/drop_caches >/dev/null"
  sleep 2
}

start_monitor() {
  local server_bin=$1
  local phase_label=$2
  relay_cmd "mkdir -p /tmp/cpu"
  relay_cmd "nohup bash -c 'PID=\$(pgrep -x $server_bin); \
    [ -n \"\$PID\" ] && pidstat -p \$PID -r -u 1 \
    > /tmp/cpu/${phase_label}_pidstat.txt 2>&1' &"
  relay_cmd "nohup mpstat -P ALL 1 \
    > /tmp/cpu/${phase_label}_mpstat.txt 2>&1 &"
}

stop_monitor() {
  relay_cmd "pkill -f pidstat 2>/dev/null; \
    pkill -f mpstat 2>/dev/null"
  sleep 1
}

download_cpu() {
  local phase_label=$1
  for suffix in pidstat mpstat; do
    relay_scp "/tmp/cpu/${phase_label}_${suffix}.txt" \
      "$OUT/cpu/"
  done
}

hd_worker_stats() {
  local label=$1
  relay_out "curl -s http://localhost:${HD_METRICS_PORT}\
/debug/workers" > "$OUT/workers/${label}.json" || true
}

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
except:
  print('  FAILED')
"
}

# ==============================================================
# MAIN
# ==============================================================

log "========================================="
log "  GAP-FILL RERUN (worker scaling)"
log "========================================="

# Ensure kTLS kernel module on both VMs.
sudo modprobe tls 2>/dev/null || true
relay_cmd "sudo modprobe tls 2>/dev/null || true"

# ---- Test 1c: kTLS Worker Sweep -----------------------------

log ""
log "=== Test 1c: kTLS Worker Sweep ==="

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

# ---- Patch contaminated Test 2a 4w data ---------------------

log ""
log "=== Patching Test 2a: 4w @20000 and @22500 ==="

drop_caches
start_hd 4
start_monitor "hyper-derp" "t2_4w_patch"

# 4w @20000: runs 4-10 were contaminated (0 Mbps).
log ""
log "--- 4w @20000 (runs 4-10) ---"
for run in $(seq 4 10); do
  padded=$(printf '%02d' "$run")
  log "  [$run/10] collapse 4w @20000:"
  run_rate "collapse_4w_20000_r${padded}" \
    "$HD_PORT" 20000
done

# 4w @22500: runs 1-6 were contaminated (0 Mbps).
log ""
log "--- 4w @22500 (runs 1-6) ---"
for run in $(seq 1 6); do
  padded=$(printf '%02d' "$run")
  log "  [$run/10] collapse 4w @22500:"
  run_rate "collapse_4w_22500_r${padded}" \
    "$HD_PORT" 22500
done

stop_monitor
download_cpu "t2_4w_patch"
stop_hd

# ---- Test 3c: Hash distribution test (40 peers) -------------

log ""
log "=== Test 3c: Hash distribution (40 peers, 8w) ==="

start_hd 8
for run in 1 2 3 4 5; do
  log "  [$run/5] 40-peer 8w @15G:"
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

# ---- Done ---------------------------------------------------

touch "$OUT/RERUN_DONE"
log ""
log "========================================="
log "  GAP-FILL RERUN COMPLETE"
log "========================================="
