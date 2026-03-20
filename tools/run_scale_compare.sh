#!/usr/bin/env bash
# Compare Hyper-DERP vs NetBird relay at peer scaling.
#
# Usage: ./run_scale_compare.sh [output_dir]
#
# Tests both relays at increasing peer counts (connection
# scaling + throughput + message loss).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../build"
HYPER_DERP="${BUILD_DIR}/hyper-derp"
SCALE_TEST="${BUILD_DIR}/tools/derp-scale-test"
NB_RELAY="/tmp/netbird-relay"
NB_LOADGEN="/tmp/netbird-loadgen"

OUTPUT_DIR="${1:-/tmp/scale-compare-$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUTPUT_DIR"

# Peer counts to test.
PEER_COUNTS="10 100 500 1000 2000 4000 8000 10000"
MSG_SIZE=1024
DURATION=10
NB_AUTH_SECRET="${NB_AUTH_SECRET:?Set NB_AUTH_SECRET env var}"

# Ports.
HD_PORT=3340
NB_PORT=3341

log() { echo "[$(date +%H:%M:%S)] $*" >&2; }

# Kill background processes on exit.
PIDS=()
cleanup() {
  for pid in "${PIDS[@]}"; do
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  done
}
trap cleanup EXIT

wait_port() {
  local port=$1 timeout=${2:-10}
  local deadline=$((SECONDS + timeout))
  while ! timeout 1 bash -c "echo >/dev/tcp/127.0.0.1/$port" \
      2>/dev/null; do
    if ((SECONDS >= deadline)); then
      log "ERROR: port $port not ready after ${timeout}s"
      return 1
    fi
    sleep 0.1
  done
}

raise_fd_limit() {
  # Need at least 2 * max_peers + some headroom.
  local target=32768
  local current
  current=$(ulimit -n)
  if ((current < target)); then
    ulimit -n "$target" 2>/dev/null || \
      log "WARNING: could not raise fd limit to $target " \
          "(current: $current)"
  fi
  log "fd limit: $(ulimit -n)"
}

# -----------------------------------------------------------
# Hyper-DERP scaling test.
# -----------------------------------------------------------
run_hyper_derp() {
  local peers=$1
  local outfile="${OUTPUT_DIR}/hyper-derp-${peers}p.json"

  log "Starting Hyper-DERP relay (port $HD_PORT, 4 workers)"
  "$HYPER_DERP" --port "$HD_PORT" --workers 4 &
  local relay_pid=$!
  PIDS+=("$relay_pid")

  if ! wait_port "$HD_PORT" 5; then
    log "Hyper-DERP relay failed to start"
    kill "$relay_pid" 2>/dev/null || true
    return 1
  fi

  log "Testing Hyper-DERP with $peers peers"
  "$SCALE_TEST" \
    --host 127.0.0.1 \
    --port "$HD_PORT" \
    --peers "$peers" \
    --msg-size "$MSG_SIZE" \
    --duration "$DURATION" \
    --json > "$outfile" 2>"${OUTPUT_DIR}/hyper-derp-${peers}p.log"
  local rc=$?

  kill "$relay_pid" 2>/dev/null || true
  wait "$relay_pid" 2>/dev/null || true
  PIDS=("${PIDS[@]/$relay_pid/}")

  # Brief pause to release port.
  sleep 1

  if [[ $rc -eq 0 ]]; then
    log "Hyper-DERP $peers peers: OK ($(cat "$outfile" | \
      grep throughput_mbps | head -1))"
  else
    log "Hyper-DERP $peers peers: FAILED (rc=$rc)"
  fi
  return $rc
}

# -----------------------------------------------------------
# NetBird scaling test.
# -----------------------------------------------------------
run_netbird() {
  local peers=$1
  local pairs=$((peers / 2))
  local outfile="${OUTPUT_DIR}/netbird-${peers}p.json"

  log "Starting NetBird relay (port $NB_PORT)"
  "$NB_RELAY" \
    --listen-address ":${NB_PORT}" \
    --exposed-address "127.0.0.1:${NB_PORT}" \
    --auth-secret "$NB_AUTH_SECRET" \
    --log-level warn &
  local relay_pid=$!
  PIDS+=("$relay_pid")

  if ! wait_port "$NB_PORT" 5; then
    log "NetBird relay failed to start"
    kill "$relay_pid" 2>/dev/null || true
    return 1
  fi

  log "Testing NetBird with $peers peers ($pairs pairs)"
  "$NB_LOADGEN" \
    --server "rel://127.0.0.1:${NB_PORT}" \
    --pairs "$pairs" \
    --message-size "$MSG_SIZE" \
    --duration "${DURATION}s" \
    --auth-secret "$NB_AUTH_SECRET" \
    --output "$outfile" \
    2>"${OUTPUT_DIR}/netbird-${peers}p.log"
  local rc=$?

  kill "$relay_pid" 2>/dev/null || true
  wait "$relay_pid" 2>/dev/null || true
  PIDS=("${PIDS[@]/$relay_pid/}")

  sleep 1

  if [[ $rc -eq 0 ]]; then
    log "NetBird $peers peers: OK"
  else
    log "NetBird $peers peers: FAILED (rc=$rc)"
  fi
  return $rc
}

# -----------------------------------------------------------
# Main.
# -----------------------------------------------------------
main() {
  log "Scale comparison: Hyper-DERP vs NetBird"
  log "Peer counts: $PEER_COUNTS"
  log "Message size: $MSG_SIZE bytes"
  log "Duration: ${DURATION}s per test"
  log "Output: $OUTPUT_DIR"

  raise_fd_limit

  # Check binaries exist.
  for bin in "$HYPER_DERP" "$SCALE_TEST" \
             "$NB_RELAY" "$NB_LOADGEN"; do
    if [[ ! -x "$bin" ]]; then
      log "ERROR: missing binary: $bin"
      exit 1
    fi
  done

  echo "relay,peers,connected,failed,msgs_sent,msgs_recv," \
       "loss_pct,throughput_mbps,connect_ms" \
    > "${OUTPUT_DIR}/summary.csv"

  for n in $PEER_COUNTS; do
    log ""
    log "========== $n peers =========="

    # Hyper-DERP.
    if run_hyper_derp "$n"; then
      local f="${OUTPUT_DIR}/hyper-derp-${n}p.json"
      if [[ -f "$f" ]]; then
        python3 -c "
import json, sys
d = json.load(open('$f'))
print(f\"hyper-derp,{d['total_peers']},{d['connected_peers']},"
      f\"{d['connect_failed']},{d['messages_sent']},"
      f\"{d['messages_recv']},{d['message_loss_pct']:.4f},"
      f\"{d['throughput_mbps']:.1f},{d['connect_time_ms']:.1f}\")
" >> "${OUTPUT_DIR}/summary.csv" 2>/dev/null || true
      fi
    fi

    # NetBird.
    if run_netbird "$n"; then
      local f="${OUTPUT_DIR}/netbird-${n}p.json"
      if [[ -f "$f" ]]; then
        python3 -c "
import json, sys
d = json.load(open('$f'))
for r in d.get('results', []):
  peers = r['peer_pairs'] * 2
  print(f\"netbird,{peers},{peers},0,0,0,0.0000,"
        f\"{r['throughput_mbps']:.1f},0.0\")
" >> "${OUTPUT_DIR}/summary.csv" 2>/dev/null || true
      fi
    fi
  done

  log ""
  log "========== Summary =========="
  if [[ -f "${OUTPUT_DIR}/summary.csv" ]]; then
    column -t -s',' "${OUTPUT_DIR}/summary.csv" >&2 || \
      cat "${OUTPUT_DIR}/summary.csv" >&2
  fi
  log "Full results in: $OUTPUT_DIR"
}

main
