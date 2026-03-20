#!/usr/bin/env bash
# Quick verification: all fixes (poll re-arm, backpressure, EOF handling).
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
RELAY="${BUILD_DIR}/hyper-derp"
SCALE="${BUILD_DIR}/tools/derp-scale-test"
CLIENT="${BUILD_DIR}/tools/derp-test-client"

BRIDGE="virbr-targets"
NS_RELAY="ns-relay"
NS_CLIENT="ns-client"
RELAY_IP="${RELAY_IP:?Set RELAY_IP env var}"
CLIENT_IP="${CLIENT_IP:?Set CLIENT_IP env var}"
HD_PORT=3341
WORKERS=2
HD_CORES="4,5"
CLIENT_CORES="12,13,14,15"

log() { echo "[$(date +%H:%M:%S)] $*"; }
ns_relay() { ip netns exec "$NS_RELAY" "$@"; }
ns_client() { ip netns exec "$NS_CLIENT" "$@"; }

RELAY_PID=""
cleanup() {
  log "Cleaning up..."
  [[ -n "$RELAY_PID" ]] && kill "$RELAY_PID" 2>/dev/null; wait "$RELAY_PID" 2>/dev/null || true
  ip netns del "$NS_RELAY" 2>/dev/null || true
  ip netns del "$NS_CLIENT" 2>/dev/null || true
  ip link del veth-rh 2>/dev/null || true
  ip link del veth-ch 2>/dev/null || true
}
trap cleanup EXIT

# -- Setup network --
log "Setting up network namespaces..."
ip netns del "$NS_RELAY" 2>/dev/null || true
ip netns del "$NS_CLIENT" 2>/dev/null || true
ip link del veth-rh 2>/dev/null || true
ip link del veth-ch 2>/dev/null || true

ip netns add "$NS_RELAY"
ip netns add "$NS_CLIENT"
ip link add veth-rh type veth peer name veth-rns
ip link add veth-ch type veth peer name veth-cns
ip link set veth-rns netns "$NS_RELAY"
ip link set veth-cns netns "$NS_CLIENT"
ip link set veth-rh master "$BRIDGE"
ip link set veth-ch master "$BRIDGE"
ip link set veth-rh up
ip link set veth-ch up
bridge link set dev veth-rh state 3 2>/dev/null || true
bridge link set dev veth-ch state 3 2>/dev/null || true

ns_relay ip addr add "${RELAY_IP}/20" dev veth-rns
ns_relay ip link set veth-rns up
ns_relay ip link set lo up
ns_client ip addr add "${CLIENT_IP}/20" dev veth-cns
ns_client ip link set veth-cns up
ns_client ip link set lo up

sysctl -w net.core.wmem_max=16777216 >/dev/null
sysctl -w net.core.rmem_max=16777216 >/dev/null
ns_relay sysctl -w net.ipv4.tcp_wmem="4096 262144 16777216" >/dev/null
ns_relay sysctl -w net.ipv4.tcp_rmem="4096 262144 16777216" >/dev/null
ns_client sysctl -w net.ipv4.tcp_wmem="4096 262144 16777216" >/dev/null
ns_client sysctl -w net.ipv4.tcp_rmem="4096 262144 16777216" >/dev/null

sleep 2
for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
  echo performance > "$cpu" 2>/dev/null || true
done

# -- Start relay --
log "Starting Hyper-DERP relay..."
ns_relay "$RELAY" --port "$HD_PORT" --workers "$WORKERS" \
  --pin-workers "$HD_CORES" --sockbuf 4194304 &
RELAY_PID=$!
sleep 2

run_latency() {
  local label=$1
  local echo_err
  echo_err=$(mktemp)
  ns_client taskset -c "$CLIENT_CORES" \
    "$CLIENT" --host "$RELAY_IP" --port "$HD_PORT" \
    --mode echo --count 6000 --timeout 60000 \
    2>"$echo_err" &
  local ECHO_PID=$!
  sleep 2

  local echo_key
  echo_key=$(grep '^pubkey:' "$echo_err" | awk '{print $2}')
  rm -f "$echo_err"

  if [[ -z "$echo_key" ]]; then
    log "  ERROR: no echo key for $label"
    kill "$ECHO_PID" 2>/dev/null || true
    wait "$ECHO_PID" 2>/dev/null || true
    return 1
  fi

  local ping_out
  ping_out=$(ns_client taskset -c "$CLIENT_CORES" \
    "$CLIENT" --host "$RELAY_IP" --port "$HD_PORT" \
    --mode ping --count 3000 --size 1400 \
    --dst-key "$echo_key" --warmup 500 \
    --timeout 60000 2>&1) || true
  echo "  $label: $(echo "$ping_out" | grep -oP 'p50=\S+' || echo 'FAIL')" \
       "$(echo "$ping_out" | grep -oP 'p99=\S+' || true)"

  kill "$ECHO_PID" 2>/dev/null || true
  wait "$ECHO_PID" 2>/dev/null || true
}

run_throughput() {
  local label=$1 rate=$2
  local raw
  raw=$(ns_client taskset -c "$CLIENT_CORES" \
    "$SCALE" --host "$RELAY_IP" --port "$HD_PORT" \
    --peers 20 --active-pairs 10 \
    --duration 10 --msg-size 1400 \
    --rate-mbps "$rate" --json 2>/dev/null) || true
  echo "$raw" | python3 -c "
import json,sys
try:
  d=json.load(sys.stdin)
  print(f'  {label}: {d[\"throughput_mbps\"]:.0f} Mbps, {d[\"message_loss_pct\"]:.2f}% loss')
except: print(f'  {label}: PARSE ERROR')
" || true
  sleep 2
}

# -- Tests --
log "=== Baseline latency ==="
run_latency baseline
sleep 1

for rate in 1000 5000 10000 20000 50000; do
  log "=== ${rate} Mbps throughput ==="
  run_throughput "rate_${rate}" "$rate"

  log "=== Idle latency after ${rate} Mbps ==="
  run_latency "after_${rate}"
  sleep 1
done

log "=== Done ==="
