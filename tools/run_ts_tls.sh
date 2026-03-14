#!/bin/bash
# TS (Go derper) TLS benchmark — rate sweep matching
# Test 1b control for apples-to-apples TLS comparison.
# Runs on the bench-client VM.
set -uo pipefail

# ---- Network / relay -----------------------------------------

RELAY=${RELAY:-10.10.0.2}
RELAY_USER=${RELAY_USER:-worker}
RELAY_KEY=${RELAY_KEY:-$HOME/.ssh/id_ed25519_targets}
TS_PORT=3340
SIZE=1400

# ---- Test params ---------------------------------------------

RATES="5000 7500 10000 15000 20000"
RUNS=10
RATE_PEERS=20
RATE_PAIRS=10
RATE_DURATION=15

# ---- Output --------------------------------------------------

OUT=/tmp/bench_worker_scaling
mkdir -p "$OUT"/{rate,cpu}

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

start_monitor() {
  local phase_label=$1
  relay_cmd "mkdir -p /tmp/cpu"
  relay_cmd "nohup bash -c 'PID=\$(pgrep -x derper); \
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
log "  TS TLS BENCHMARK"
log "========================================="

# Ensure cert has IP SAN for derper hostname matching.
relay_cmd '
openssl req -x509 -newkey ec \
  -pkeyopt ec_paramgen_curve:prime256v1 \
  -keyout /tmp/key.pem -out /tmp/cert.pem \
  -days 1 -nodes \
  -subj "/CN=10.10.0.2" \
  -addext "subjectAltName=IP:10.10.0.2,DNS:bench-relay" \
  2>/dev/null
CERTDIR=/tmp/derper-certs
mkdir -p "$CERTDIR"
cp /tmp/cert.pem "$CERTDIR/10.10.0.2.crt"
cp /tmp/key.pem "$CERTDIR/10.10.0.2.key"
'

# Start TS with TLS.
log "Starting TS derper with TLS..."
relay_cmd "sudo pkill -9 derper 2>/dev/null; sleep 1"
relay_cmd "sudo setsid derper -dev \
  -a :${TS_PORT} \
  -certmode manual \
  -certdir /tmp/derper-certs \
  -hostname 10.10.0.2 \
  </dev/null >/tmp/ts.log 2>&1 &"
sleep 4

ok=$(relay_out "ss -tlnp | grep -c ${TS_PORT}" || echo 0)
if [ "${ok:-0}" -lt 1 ]; then
  log "ERROR: TS not listening on ${TS_PORT}"
  exit 1
fi
log "  TS TLS listening on :${TS_PORT}"

drop_caches
start_monitor "ts_tls"

for rate in $RATES; do
  for run in $(seq 1 $RUNS); do
    padded=$(printf '%02d' "$run")
    log "  [$run/$RUNS] ts_tls @${rate}:"
    run_rate "ts_tls_${rate}_r${padded}" \
      "$TS_PORT" "$rate" "--tls"
  done
done

stop_monitor
download_cpu "ts_tls"

# Stop TS.
relay_cmd "sudo pkill -9 derper 2>/dev/null"

touch "$OUT/TS_TLS_DONE"
log ""
log "========================================="
log "  TS TLS BENCHMARK COMPLETE"
log "========================================="
