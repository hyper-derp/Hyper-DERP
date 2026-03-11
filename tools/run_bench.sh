#!/bin/bash
# Benchmark suite for Hyper-DERP relay.
#
# Runs throughput and latency tests at various packet sizes.
# Outputs JSON results to $RESULTS_DIR.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build"
RESULTS_DIR="${PROJECT_DIR}/bench_results"
RELAY="${BUILD_DIR}/hyper-derp"
CLIENT="${BUILD_DIR}/derp-test-client"

HOST=127.0.0.1
PORT=3340
WORKERS=2
WARMUP=100
TIMEOUT=30000

mkdir -p "$RESULTS_DIR"

cleanup() {
  if [[ -n "${RELAY_PID:-}" ]]; then
    kill "$RELAY_PID" 2>/dev/null || true
    wait "$RELAY_PID" 2>/dev/null || true
  fi
  if [[ -n "${ECHO_PID:-}" ]]; then
    kill "$ECHO_PID" 2>/dev/null || true
    wait "$ECHO_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

echo "=== Hyper-DERP Benchmark Suite ==="
echo "Build: ${BUILD_DIR}"
echo "Results: ${RESULTS_DIR}"
echo ""

# Start relay.
"$RELAY" --port "$PORT" --workers "$WORKERS" &
RELAY_PID=$!
sleep 0.5

# Verify relay is up.
if ! kill -0 "$RELAY_PID" 2>/dev/null; then
  echo "ERROR: relay failed to start"
  exit 1
fi
echo "Relay started (pid=$RELAY_PID, workers=$WORKERS)"

# Start echo peer.
ECHO_KEY_FILE=$(mktemp)
"$CLIENT" --host "$HOST" --port "$PORT" \
  --mode echo --count 1000000 --timeout 60000 \
  2>"$ECHO_KEY_FILE" &
ECHO_PID=$!
sleep 0.3

# Extract echo peer's public key.
ECHO_KEY=$(grep '^pubkey:' "$ECHO_KEY_FILE" | \
  awk '{print $2}')
rm -f "$ECHO_KEY_FILE"

if [[ -z "$ECHO_KEY" ]]; then
  echo "ERROR: failed to get echo peer key"
  exit 1
fi
echo "Echo peer started (key=${ECHO_KEY:0:16}...)"
echo ""

run_throughput() {
  local label=$1 count=$2 size=$3

  echo "--- Throughput: ${label} (${count} x ${size}B) ---"

  # Start receiver.
  local recv_key_file=$(mktemp)
  "$CLIENT" --host "$HOST" --port "$PORT" \
    --mode recv --count "$count" --timeout "$TIMEOUT" \
    --warmup "$WARMUP" --json \
    --label "${label}_recv" --workers "$WORKERS" \
    --output "${RESULTS_DIR}/${label}_recv.json" \
    2>"$recv_key_file" &
  local RECV_PID=$!
  sleep 0.2

  local RECV_KEY=$(grep '^pubkey:' "$recv_key_file" | \
    awk '{print $2}')
  rm -f "$recv_key_file"

  # Run sender.
  "$CLIENT" --host "$HOST" --port "$PORT" \
    --mode send --count "$count" --size "$size" \
    --dst-key "$RECV_KEY" --warmup "$WARMUP" --json \
    --label "${label}_send" --workers "$WORKERS" \
    --timeout "$TIMEOUT" \
    --output "${RESULTS_DIR}/${label}_send.json" \
    2>&1 | grep -v '^pubkey:' || true

  wait "$RECV_PID" 2>/dev/null || true
  echo ""
}

run_latency() {
  local label=$1 count=$2 size=$3

  echo "--- Latency: ${label} (${count} pings, ${size}B) ---"

  "$CLIENT" --host "$HOST" --port "$PORT" \
    --mode ping --count "$count" --size "$size" \
    --dst-key "$ECHO_KEY" --warmup "$WARMUP" \
    --json --raw-latency \
    --label "${label}" --workers "$WORKERS" \
    --timeout "$TIMEOUT" \
    --output "${RESULTS_DIR}/${label}.json" \
    2>&1 | grep -v '^pubkey:' || true

  echo ""
}

# === Throughput tests ===
echo "===== THROUGHPUT ====="
run_throughput "tp_64B"   100000  64
run_throughput "tp_256B"  100000  256
run_throughput "tp_1024B" 100000  1024
run_throughput "tp_4096B"  50000  4096
run_throughput "tp_8192B"  50000  8192

# === Latency tests ===
echo "===== LATENCY ====="
run_latency "lat_64B"    10000  64
run_latency "lat_256B"   10000  256
run_latency "lat_1024B"  10000  1024

# === Summary ===
echo "===== RESULTS ====="
if command -v python3 &>/dev/null; then
  RESULT_FILES=$(ls "${RESULTS_DIR}"/*.json 2>/dev/null)
  if [[ -n "$RESULT_FILES" ]]; then
    python3 "${SCRIPT_DIR}/plot_results.py" \
      --no-table $RESULT_FILES 2>/dev/null || true
    python3 "${SCRIPT_DIR}/plot_results.py" \
      $RESULT_FILES
  fi
fi

echo ""
echo "Results saved to ${RESULTS_DIR}/"
echo "Done."
