#!/bin/bash
# run_benchmark.sh — Rate sweep benchmark on GCP VMs.
#
# Runs ON the client VM. Both hyper-derp and derper must
# already be running on the relay. Results go to /tmp/bench/.
#
# Usage: run_benchmark.sh [relay_ip]
set -euo pipefail

RELAY=${1:-10.10.0.4}
TS_PORT=3340
HD_PORT=3341

PEERS=20
PAIRS=10
DURATION=10
SIZE=1400

# Rate sweep: 10 → 20000 Mbps.
RATES="10 50 100 250 500 1000 2000 3000 5000 7500 10000 15000 20000"

# Latency test.
PING_COUNT=5000
PING_WARMUP=500

OUT=/tmp/bench
rm -rf "$OUT"
mkdir -p "$OUT"

log() { echo "[$(date +%H:%M:%S)] $*"; }

# System info.
{
  echo "date: $(date -Iseconds)"
  echo "relay: $RELAY"
  echo "kernel: $(uname -r)"
  echo "cpu: $(lscpu | grep 'Model name' | sed 's/.*: *//')"
  echo "peers: $PEERS"
  echo "pairs: $PAIRS"
  echo "duration: ${DURATION}s"
  echo "size: ${SIZE}B"
  echo "rates: $RATES"
} > "$OUT/system_info.txt"

run_rate_test() {
  local label=$1 port=$2 rate=$3
  local outfile="$OUT/${label}.json"
  log "  $label: ${rate} Mbps ..."
  derp-scale-test \
    --host "$RELAY" --port "$port" \
    --peers "$PEERS" --active-pairs "$PAIRS" \
    --duration "$DURATION" --msg-size "$SIZE" \
    --rate-mbps "$rate" \
    --json 2>/dev/null | sed -n '/{/,/}/p' \
    > "$outfile" || true
  sleep 2
}

run_latency() {
  local label=$1 port=$2
  local outfile="$OUT/${label}.json"
  local echo_err="/tmp/${label}_echo.err"
  log "  $label: latency ${SIZE}B ..."

  # Start echo peer.
  derp-test-client \
    --host "$RELAY" --port "$port" --mode echo \
    --count $((PING_COUNT + PING_WARMUP + 500)) \
    --timeout 60000 \
    >/dev/null 2>"$echo_err" &
  local echo_pid=$!
  sleep 3

  local echo_key
  echo_key=$(grep '^pubkey:' "$echo_err" | awk '{print $2}')
  if [[ -z "$echo_key" ]]; then
    log "  ERROR: no echo key for $label"
    kill "$echo_pid" 2>/dev/null || true
    return 1
  fi

  derp-test-client \
    --host "$RELAY" --port "$port" --mode ping \
    --count "$PING_COUNT" --size "$SIZE" \
    --dst-key "$echo_key" --warmup "$PING_WARMUP" \
    --json --raw-latency \
    --label "$label" --output "$outfile" \
    --timeout 60000 2>/dev/null || true

  kill "$echo_pid" 2>/dev/null || true
  wait "$echo_pid" 2>/dev/null || true
  sleep 2
}

log "=== GCP Benchmark: HD vs TS ==="
log "Relay: $RELAY"
log "Rates: $RATES"
log "Output: $OUT"
log ""

# --- Tailscale derper ---
log "======== Tailscale derper (port $TS_PORT) ========"
for rate in $RATES; do
  run_rate_test "ts_${rate}mbps" "$TS_PORT" "$rate"
done
run_latency "ts_lat_1400B" "$TS_PORT"

# --- Hyper-DERP ---
log ""
log "======== Hyper-DERP (port $HD_PORT) ========"
for rate in $RATES; do
  run_rate_test "hd_${rate}mbps" "$HD_PORT" "$rate"
done
run_latency "hd_lat_1400B" "$HD_PORT"

log ""
log "=== All tests complete. Results in $OUT ==="
ls -la "$OUT/"
