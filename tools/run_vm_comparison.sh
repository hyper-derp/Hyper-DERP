#!/usr/bin/env bash
# VM-based comparison: Hyper-DERP vs Tailscale derper.
#
# Relay runs on bench-relay (10.101.1.1), clients on
# bench-client (10.101.1.2). Uses tap networking for
# minimal overhead.
#
# Usage: ./run_vm_comparison.sh [output_dir]

set -euo pipefail

SSH_KEY="${SSH_KEY:?Set SSH_KEY env var (path to SSH key)}"
RELAY_IP="${RELAY_IP:?Set RELAY_IP env var}"
CLIENT_IP="${CLIENT_IP:?Set CLIENT_IP env var}"
SSH_USER="${RELAY_USER:-worker}"
SSH_RELAY="ssh -i $SSH_KEY ${SSH_USER}@$RELAY_IP"
SSH_CLIENT="ssh -i $SSH_KEY ${SSH_USER}@$CLIENT_IP"
SCP_FROM_CLIENT="scp -i $SSH_KEY ${SSH_USER}@$CLIENT_IP"

TS_PORT=3340
HD_PORT=3341
WORKERS=2

# Scale test parameters.
PEERS=20
PAIRS=10
DURATION=5
SIZE=1400

# Rate sweep (Mbps).
RATES="10 50 100 250 500 1000 2000 5000"

# Latency test.
PING_COUNT=2000
PING_WARMUP=200

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
OUTPUT_DIR="${1:-${PROJECT_DIR}/bench_results/comparison-$(date +%Y%m%d)/vm_scale_v3}"
mkdir -p "$OUTPUT_DIR"

log() { echo "[$(date +%H:%M:%S)] $*"; }

cleanup() {
  $SSH_RELAY "sudo pkill -9 hyper-derp 2>/dev/null; \
    sudo pkill -9 derper 2>/dev/null" 2>/dev/null || true
  $SSH_CLIENT "pkill -9 derp-scale-test 2>/dev/null; \
    pkill -9 derp-test-client 2>/dev/null" 2>/dev/null || true
}
trap cleanup EXIT

wait_port() {
  local host=$1 port=$2 timeout=${3:-10}
  local deadline=$((SECONDS + timeout))
  while ! $SSH_CLIENT "timeout 1 bash -c \
      'echo >/dev/tcp/$host/$port'" 2>/dev/null; do
    if ((SECONDS >= deadline)); then
      log "ERROR: $host:$port not ready after ${timeout}s"
      return 1
    fi
    sleep 0.2
  done
}

run_scale_test() {
  local label=$1 host=$2 port=$3 rate=$4
  local outfile="/tmp/${label}.json"
  log "  scale test: ${rate} Mbps ..."
  # JSON is appended at end of stdout; extract it.
  $SSH_CLIENT "derp-scale-test \
    --host $host --port $port \
    --peers $PEERS --active-pairs $PAIRS \
    --duration $DURATION --msg-size $SIZE \
    --rate-mbps $rate \
    --json 2>/dev/null | sed -n '/{/,/}/p' \
    > $outfile" 2>/dev/null || true
  $SCP_FROM_CLIENT:$outfile \
    "${OUTPUT_DIR}/${label}.json" 2>/dev/null || true
  log "  ${rate} Mbps done"
}

run_latency() {
  local label=$1 host=$2 port=$3
  local outfile="/tmp/${label}.json"
  local echo_err="/tmp/${label}_echo.err"
  log "  latency 1400B ..."

  # Start echo server on client.
  $SSH_CLIENT "nohup derp-test-client \
    --host $host --port $port --mode echo \
    --count $((PING_COUNT + PING_WARMUP + 500)) \
    --timeout 30000 \
    >/dev/null 2>$echo_err &"
  sleep 2

  local echo_key
  echo_key=$($SSH_CLIENT "grep '^pubkey:' $echo_err | \
    awk '{print \$2}'" 2>/dev/null)
  if [[ -z "$echo_key" ]]; then
    log "  ERROR: no echo key for $label"
    $SSH_CLIENT "pkill -f 'mode echo'" 2>/dev/null || true
    return 1
  fi

  $SSH_CLIENT "derp-test-client \
    --host $host --port $port --mode ping \
    --count $PING_COUNT --size $SIZE \
    --dst-key $echo_key --warmup $PING_WARMUP \
    --json --raw-latency \
    --label $label --output $outfile \
    --timeout 30000" 2>/dev/null || true

  $SSH_CLIENT "pkill -f 'mode echo'" 2>/dev/null || true
  $SCP_FROM_CLIENT:$outfile \
    "${OUTPUT_DIR}/${label}.json" 2>/dev/null || true
  log "  latency done"
}

test_ts() {
  log ""
  log "======== Tailscale derper ========"
  $SSH_RELAY "sudo nohup derper -dev \
    >/dev/null 2>/tmp/derper.log &"
  if ! wait_port "$RELAY_IP" "$TS_PORT" 10; then
    log "ERROR: derper failed to start"
    return 1
  fi
  log "derper started on :${TS_PORT}"

  for rate in $RATES; do
    run_scale_test "ts_${rate}mbps" "$RELAY_IP" "$TS_PORT" \
      "$rate" || true
    sleep 2
  done

  run_latency "ts_lat_1400B" "$RELAY_IP" "$TS_PORT" || true

  $SSH_RELAY "sudo pkill -9 derper" 2>/dev/null || true
  sleep 2
}

test_hd() {
  log ""
  log "======== Hyper-DERP (backpressure v3) ========"
  $SSH_RELAY "sudo nohup hyper-derp --port ${HD_PORT} \
    --workers $WORKERS \
    >/dev/null 2>/tmp/hyper-derp.log &"
  if ! wait_port "$RELAY_IP" "$HD_PORT" 10; then
    log "ERROR: hyper-derp failed to start"
    return 1
  fi
  log "hyper-derp started on :${HD_PORT}"

  for rate in $RATES; do
    run_scale_test "hd_${rate}mbps" "$RELAY_IP" "$HD_PORT" \
      "$rate" || true
    sleep 2
  done

  run_latency "hd_lat_1400B" "$RELAY_IP" "$HD_PORT" || true

  $SSH_RELAY "sudo pkill -9 hyper-derp" 2>/dev/null || true
  sleep 2
}

main() {
  log "VM Comparison: Hyper-DERP (backpressure v3) vs TS"
  log "Relay: $RELAY_IP, Client: $CLIENT_IP"
  log "Output: $OUTPUT_DIR"
  log "Rates: $RATES Mbps"
  log ""

  # System info.
  {
    echo "date: $(date -Iseconds)"
    echo "relay_vm: $RELAY_IP"
    echo "client_vm: $CLIENT_IP"
    $SSH_RELAY "echo \"kernel: \$(uname -r)\""
    $SSH_RELAY "echo \"cpu: \$(lscpu | grep 'Model name' | \
      sed 's/.*: *//')\""
    echo "workers: $WORKERS"
    echo "peers: $PEERS"
    echo "pairs: $PAIRS"
    echo "duration: ${DURATION}s"
    echo "size: ${SIZE}B"
    echo "rates: $RATES"
  } > "${OUTPUT_DIR}/system_info.txt"

  # Clean slate.
  cleanup

  test_ts
  test_hd

  log ""
  log "All tests complete. Results in: $OUTPUT_DIR"
}

main
