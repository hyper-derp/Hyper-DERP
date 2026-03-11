#!/usr/bin/env bash
# Compare Hyper-DERP vs Tailscale derper (Go) performance.
#
# Workstation-safe: uses count-based send/recv (NOT flood).
# Each test sends a fixed number of packets and measures
# throughput and delivery.
#
# Usage: ./run_derp_comparison.sh [output_dir]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build"
RELAY="${BUILD_DIR}/hyper-derp"
CLIENT="${BUILD_DIR}/tools/derp-test-client"
DERPER="${HOME}/go/bin/derper"

OUTPUT_DIR="${1:-${PROJECT_DIR}/bench_results/comparison-$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUTPUT_DIR"

HD_PORT=3341
TS_PORT=3340
WORKERS=2
WARMUP=100

# Fixed packet counts — bounded, cannot run away.
SEND_COUNT=20000
PING_COUNT=2000
PING_WARMUP=200
TIMEOUT=30000

# Packet sizes to test.
TP_SIZES="64 256 1024 4096"
LAT_SIZES="64 1024"

log() { echo "[$(date +%H:%M:%S)] $*"; }

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
  while ! timeout 1 bash -c \
      "echo >/dev/tcp/127.0.0.1/$port" 2>/dev/null; do
    if ((SECONDS >= deadline)); then
      log "ERROR: port $port not ready after ${timeout}s"
      return 1
    fi
    sleep 0.1
  done
}

# Throughput: send N packets, measure time + delivery.
run_throughput() {
  local label=$1 port=$2 size=$3
  local recv_key_file
  recv_key_file=$(mktemp)

  log "  throughput ${size}B x ${SEND_COUNT} ..."

  # Start receiver first.
  "$CLIENT" --host 127.0.0.1 --port "$port" \
    --mode recv --count "$SEND_COUNT" \
    --timeout "$TIMEOUT" --warmup "$WARMUP" \
    --json --label "${label}_recv" \
    --output "${OUTPUT_DIR}/${label}_recv.json" \
    2>"$recv_key_file" &
  local RECV_PID=$!
  PIDS+=("$RECV_PID")
  sleep 1

  local RECV_KEY
  RECV_KEY=$(grep '^pubkey:' "$recv_key_file" | \
    awk '{print $2}')
  rm -f "$recv_key_file"

  if [[ -z "$RECV_KEY" ]]; then
    log "  ERROR: no recv key for ${label}"
    kill "$RECV_PID" 2>/dev/null || true
    wait "$RECV_PID" 2>/dev/null || true
    PIDS=("${PIDS[@]/$RECV_PID/}")
    return 1
  fi

  # Send exactly SEND_COUNT packets.
  "$CLIENT" --host 127.0.0.1 --port "$port" \
    --mode send --count "$SEND_COUNT" --size "$size" \
    --dst-key "$RECV_KEY" --warmup "$WARMUP" \
    --json --label "${label}_send" \
    --timeout "$TIMEOUT" \
    --output "${OUTPUT_DIR}/${label}_send.json" \
    2>/dev/null || true

  wait "$RECV_PID" 2>/dev/null || true
  PIDS=("${PIDS[@]/$RECV_PID/}")

  log "  throughput ${size}B done"
}

# Latency: ping/pong round trips.
run_latency() {
  local label=$1 port=$2 size=$3
  local echo_key_file
  echo_key_file=$(mktemp)

  log "  latency ${size}B x ${PING_COUNT} ..."

  "$CLIENT" --host 127.0.0.1 --port "$port" \
    --mode echo \
    --count "$((PING_COUNT + PING_WARMUP + 500))" \
    --timeout "$TIMEOUT" \
    2>"$echo_key_file" &
  local ECHO_PID=$!
  PIDS+=("$ECHO_PID")
  sleep 1

  local ECHO_KEY
  ECHO_KEY=$(grep '^pubkey:' "$echo_key_file" | \
    awk '{print $2}')
  rm -f "$echo_key_file"

  if [[ -z "$ECHO_KEY" ]]; then
    log "  ERROR: no echo key for ${label}"
    kill "$ECHO_PID" 2>/dev/null || true
    wait "$ECHO_PID" 2>/dev/null || true
    PIDS=("${PIDS[@]/$ECHO_PID/}")
    return 1
  fi

  "$CLIENT" --host 127.0.0.1 --port "$port" \
    --mode ping --count "$PING_COUNT" --size "$size" \
    --dst-key "$ECHO_KEY" --warmup "$PING_WARMUP" \
    --json --raw-latency \
    --label "${label}" \
    --output "${OUTPUT_DIR}/${label}.json" \
    --timeout "$TIMEOUT" \
    2>/dev/null || true

  kill "$ECHO_PID" 2>/dev/null || true
  wait "$ECHO_PID" 2>/dev/null || true
  PIDS=("${PIDS[@]/$ECHO_PID/}")

  log "  latency ${size}B done"
}

test_relay() {
  local name=$1 port=$2

  log ""
  log "======== Testing ${name} on port ${port} ========"

  for sz in $TP_SIZES; do
    run_throughput "${name}_tp_${sz}B" "$port" "$sz" || true
    sleep 2
  done

  for sz in $LAT_SIZES; do
    run_latency "${name}_lat_${sz}B" "$port" "$sz" || true
    sleep 2
  done
}

main() {
  log "Hyper-DERP vs Tailscale derper comparison"
  log "Output: $OUTPUT_DIR"
  log "Params: send=${SEND_COUNT}, ping=${PING_COUNT}"
  log ""

  for bin in "$RELAY" "$CLIENT" "$DERPER"; do
    if [[ ! -x "$bin" ]]; then
      log "ERROR: missing binary: $bin"
      exit 1
    fi
  done

  {
    echo "date: $(date -Iseconds)"
    echo "kernel: $(uname -r)"
    echo "cpu: $(lscpu | grep 'Model name' | \
      sed 's/.*: *//')"
    echo "cores: $(nproc)"
    echo "workers: $WORKERS"
    echo "send_count: $SEND_COUNT"
    echo "ping_count: $PING_COUNT"
    echo "tp_sizes: $TP_SIZES"
    echo "lat_sizes: $LAT_SIZES"
  } > "${OUTPUT_DIR}/system_info.txt"

  # ----- Tailscale derper (Go) -----
  log "Starting Tailscale derper on port ${TS_PORT} ..."
  "$DERPER" -dev -a ":${TS_PORT}" \
    2>"${OUTPUT_DIR}/derper.log" &
  local TS_PID=$!
  PIDS+=("$TS_PID")

  if ! wait_port "$TS_PORT" 5; then
    log "ERROR: derper failed to start"
    exit 1
  fi
  log "Tailscale derper started (pid=$TS_PID)"

  test_relay "ts" "$TS_PORT"

  kill "$TS_PID" 2>/dev/null || true
  wait "$TS_PID" 2>/dev/null || true
  PIDS=("${PIDS[@]/$TS_PID/}")
  sleep 2

  # ----- Hyper-DERP -----
  log "Starting Hyper-DERP on port ${HD_PORT} ..."
  "$RELAY" --port "$HD_PORT" --workers "$WORKERS" \
    2>"${OUTPUT_DIR}/hyper-derp.log" &
  local HD_PID=$!
  PIDS+=("$HD_PID")

  if ! wait_port "$HD_PORT" 5; then
    log "ERROR: Hyper-DERP failed to start"
    exit 1
  fi
  log "Hyper-DERP started (pid=$HD_PID)"

  test_relay "hd" "$HD_PORT"

  kill "$HD_PID" 2>/dev/null || true
  wait "$HD_PID" 2>/dev/null || true
  PIDS=("${PIDS[@]/$HD_PID/}")

  log ""
  log "All tests complete. Results in: $OUTPUT_DIR"
}

main
