#!/bin/bash
# P3 (ProcessXfer bitmask) validation benchmark.
# Runs on the bench-client VM.
set -uo pipefail

# ---- Network / relay -----------------------------------------

RELAY="${RELAY:?Set RELAY env var (relay IP)}"
RELAY_USER="${RELAY_USER:-worker}"
RELAY_KEY="${RELAY_KEY:?Set RELAY_KEY env var (path to SSH key)}"
HD_PORT=3341
SIZE=1400

# ---- Test params ---------------------------------------------

RUNS=10
RATE_PEERS=20
RATE_PAIRS=10
RATE_DURATION=15

# ---- Output --------------------------------------------------

OUT=/tmp/bench_p3_validation
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

drop_caches() {
  relay_cmd "sync; echo 3 | sudo tee \
    /proc/sys/vm/drop_caches >/dev/null"
  sleep 2
}

start_hd() {
  local workers=$1
  local extra="${2:-}"
  relay_cmd "sudo pkill -9 hyper-derp 2>/dev/null; sleep 1"
  relay_cmd "sudo setsid /usr/local/bin/hyper-derp \
    --port ${HD_PORT} --workers ${workers} \
    --debug-endpoints --metrics-port 9100 \
    ${extra} \
    </dev/null >/tmp/hd.log 2>&1 &"
  sleep 3
  local ok
  ok=$(relay_out "ss -tlnp | grep -c ${HD_PORT}" || echo 0)
  if [ "${ok:-0}" -lt 1 ]; then
    log "ERROR: HD not listening on ${HD_PORT}"
    return 1
  fi
  log "  HD listening (workers=${workers}) ${extra}"
}

collect_worker_stats() {
  local label=$1
  relay_scp "/tmp/hd_worker_stats.json" \
    "$OUT/workers/${label}_workers.json" 2>/dev/null
  # Grab from debug endpoint.
  local stats
  stats=$(relay_out \
    "curl -s http://127.0.0.1:9100/debug/workers" \
    2>/dev/null) || true
  if [ -n "$stats" ]; then
    echo "$stats" > "$OUT/workers/${label}_workers.json"
  fi
}

start_monitor() {
  local label=$1
  relay_cmd "mkdir -p /tmp/cpu"
  relay_cmd "nohup bash -c 'PID=\$(pgrep -x hyper-derp); \
    [ -n \"\$PID\" ] && pidstat -p \$PID -r -u 1 \
    > /tmp/cpu/${label}_pidstat.txt 2>&1' &"
  relay_cmd "nohup mpstat -P ALL 1 \
    > /tmp/cpu/${label}_mpstat.txt 2>&1 &"
}

stop_monitor() {
  relay_cmd "pkill -f pidstat 2>/dev/null; \
    pkill -f mpstat 2>/dev/null"
  sleep 1
}

download_cpu() {
  local label=$1
  for suffix in pidstat mpstat; do
    relay_scp "/tmp/cpu/${label}_${suffix}.txt" \
      "$OUT/cpu/"
  done
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
log "  P3 BITMASK VALIDATION"
log "========================================="

# ---- Phase 1: 8w plain TCP at 15G and 20G -------------------

log ""
log "Phase 1: 8w plain TCP @ 15G, 20G"
start_hd 8
drop_caches
start_monitor "p3_8w_tcp"

for rate in 15000 20000; do
  for run in $(seq 1 $RUNS); do
    padded=$(printf '%02d' "$run")
    log "  [$run/$RUNS] 8w TCP @${rate}:"
    run_rate "8w_tcp_${rate}_r${padded}" \
      "$HD_PORT" "$rate"
  done
  collect_worker_stats "8w_tcp_${rate}"
done

stop_monitor
download_cpu "p3_8w_tcp"
relay_cmd "sudo pkill -9 hyper-derp 2>/dev/null"

# ---- Phase 2: 4w plain TCP at 15G ---------------------------

log ""
log "Phase 2: 4w plain TCP @ 15G (regression check)"
start_hd 4
drop_caches
start_monitor "p3_4w_tcp"

for run in $(seq 1 $RUNS); do
  padded=$(printf '%02d' "$run")
  log "  [$run/$RUNS] 4w TCP @15G:"
  run_rate "4w_tcp_15000_r${padded}" \
    "$HD_PORT" "15000"
done
collect_worker_stats "4w_tcp_15000"

stop_monitor
download_cpu "p3_4w_tcp"
relay_cmd "sudo pkill -9 hyper-derp 2>/dev/null"

# ---- Phase 3: 8w kTLS at 15G and 20G ------------------------

log ""
log "Phase 3: 8w kTLS @ 15G, 20G"

# Ensure tls module loaded and certs exist.
relay_cmd "sudo modprobe tls 2>/dev/null"
relay_cmd '
openssl req -x509 -newkey ec \
  -pkeyopt ec_paramgen_curve:prime256v1 \
  -keyout /tmp/hd_key.pem -out /tmp/hd_cert.pem \
  -days 1 -nodes \
  -subj "/CN=bench-relay" \
  -addext "subjectAltName=IP:${RELAY},DNS:bench-relay" \
  2>/dev/null
'
# Load tls module on client too.
sudo modprobe tls 2>/dev/null || true

start_hd 8 "--tls-cert /tmp/hd_cert.pem --tls-key /tmp/hd_key.pem"
drop_caches
start_monitor "p3_8w_ktls"

for rate in 15000 20000; do
  for run in $(seq 1 $RUNS); do
    padded=$(printf '%02d' "$run")
    log "  [$run/$RUNS] 8w kTLS @${rate}:"
    run_rate "8w_ktls_${rate}_r${padded}" \
      "$HD_PORT" "$rate" "--tls"
  done
  collect_worker_stats "8w_ktls_${rate}"
done

stop_monitor
download_cpu "p3_8w_ktls"
relay_cmd "sudo pkill -9 hyper-derp 2>/dev/null"

# ---- Summary ------------------------------------------------

log ""
log "========================================="
log "  P3 VALIDATION COMPLETE"
log "========================================="
log ""
log "Results in $OUT/"
log "Rate files: $(ls "$OUT/rate/" | wc -l)"

# Quick summary.
python3 -c "
import json, glob, statistics

configs = [
    ('8w TCP @15G', '8w_tcp_15000'),
    ('8w TCP @20G', '8w_tcp_20000'),
    ('4w TCP @15G', '4w_tcp_15000'),
    ('8w kTLS @15G', '8w_ktls_15000'),
    ('8w kTLS @20G', '8w_ktls_20000'),
]

print()
print(f'{\"Config\":<16} {\"Mean\":>8} {\"Min\":>8} {\"Max\":>8} {\"Loss%\":>8}')
print('-' * 52)
for label, prefix in configs:
    files = sorted(glob.glob(f'$OUT/rate/{prefix}_r*.json'))
    if not files:
        print(f'{label:<16} NO DATA')
        continue
    tputs = []
    losses = []
    for f in files:
        try:
            d = json.load(open(f))
            tputs.append(d['throughput_mbps'])
            losses.append(d['message_loss_pct'])
        except:
            pass
    if tputs:
        print(f'{label:<16} {statistics.mean(tputs):>8.0f} '
              f'{min(tputs):>8.0f} {max(tputs):>8.0f} '
              f'{statistics.mean(losses):>8.1f}')
print()
" 2>/dev/null || true

touch "$OUT/P3_VALIDATION_DONE"
