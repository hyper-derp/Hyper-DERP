#!/usr/bin/env bash
# Scaling sweep: throughput vs active peer pairs.
# Runs TS, HD (plain TCP), and HD (kTLS) at each pair count.
#
# Usage: sudo ./run_scaling_sweep.sh [output_dir]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build"
RELAY="${BUILD_DIR}/hyper-derp"
SCALE="${BUILD_DIR}/tools/derp-scale-test"
DERPER="/home/karl/go/bin/derper"

# Network config (same as run_ktls_comparison.sh).
BRIDGE="virbr-targets"
NS_RELAY="ns-relay"
NS_CLIENT="ns-client"
VETH_RELAY_HOST="veth-rh"
VETH_RELAY_NS="veth-rns"
VETH_CLIENT_HOST="veth-ch"
VETH_CLIENT_NS="veth-cns"
RELAY_IP="10.101.2.1"
CLIENT_IP="10.101.2.2"
NETMASK="20"

TS_PORT=3340
HD_PORT=3341
KTLS_PORT=3342
WORKERS=2

# CPU pinning.
HD_CORES="4,5"
CLIENT_CORES="12,13,14,15"

# Test params.
SIZE=1400
DURATION=5
RATE=0  # Unlimited — measure peak throughput at each scale.

# Peer pair sweep.
# Each point: peers = pairs*2 (every peer is part of a pair).
PAIR_COUNTS="1 2 5 10 25 50 100 250 500"

OUTPUT_DIR="${1:-${PROJECT_DIR}/bench_results/scaling-$(date +%Y%m%d)}"
mkdir -p "$OUTPUT_DIR"

log() { echo "[$(date +%H:%M:%S)] $*"; }

ns_relay() { ip netns exec "$NS_RELAY" "$@"; }
ns_client() { ip netns exec "$NS_CLIENT" "$@"; }

PIDS=()
kill_relays() {
  # Kill any leftover relay/test processes to prevent
  # zombie accumulation between runs.
  killall -9 hyper-derp derper derp-scale-test \
    2>/dev/null || true
  sleep 0.5
}

cleanup() {
  log "Cleaning up..."
  for pid in "${PIDS[@]}"; do
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  done
  PIDS=()
  kill_relays
  ip netns del "$NS_RELAY" 2>/dev/null || true
  ip netns del "$NS_CLIENT" 2>/dev/null || true
  ip link del "$VETH_RELAY_HOST" 2>/dev/null || true
  ip link del "$VETH_CLIENT_HOST" 2>/dev/null || true
  for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo powersave > "$cpu" 2>/dev/null || true
  done
}
trap cleanup EXIT

setup_network() {
  log "Setting up network namespaces..."

  if ! ip link show "$BRIDGE" &>/dev/null; then
    log "ERROR: bridge $BRIDGE does not exist"
    exit 1
  fi

  ip netns del "$NS_RELAY" 2>/dev/null || true
  ip netns del "$NS_CLIENT" 2>/dev/null || true
  ip link del "$VETH_RELAY_HOST" 2>/dev/null || true
  ip link del "$VETH_CLIENT_HOST" 2>/dev/null || true

  ip netns add "$NS_RELAY"
  ip netns add "$NS_CLIENT"

  ip link add "$VETH_RELAY_HOST" type veth \
    peer name "$VETH_RELAY_NS"
  ip link add "$VETH_CLIENT_HOST" type veth \
    peer name "$VETH_CLIENT_NS"

  ip link set "$VETH_RELAY_NS" netns "$NS_RELAY"
  ip link set "$VETH_CLIENT_NS" netns "$NS_CLIENT"

  ip link set "$VETH_RELAY_HOST" master "$BRIDGE"
  ip link set "$VETH_CLIENT_HOST" master "$BRIDGE"
  ip link set "$VETH_RELAY_HOST" up
  ip link set "$VETH_CLIENT_HOST" up

  bridge link set dev "$VETH_RELAY_HOST" state 3 \
    2>/dev/null || true
  bridge link set dev "$VETH_CLIENT_HOST" state 3 \
    2>/dev/null || true

  ns_relay ip addr add "${RELAY_IP}/${NETMASK}" \
    dev "$VETH_RELAY_NS"
  ns_relay ip link set "$VETH_RELAY_NS" up
  ns_relay ip link set lo up

  ns_client ip addr add "${CLIENT_IP}/${NETMASK}" \
    dev "$VETH_CLIENT_NS"
  ns_client ip link set "$VETH_CLIENT_NS" up
  ns_client ip link set lo up

  sleep 1

  # Raise socket buffer limits for high pair counts.
  sysctl -w net.core.wmem_max=16777216 >/dev/null
  sysctl -w net.core.rmem_max=16777216 >/dev/null
  sysctl -w net.core.wmem_default=262144 >/dev/null
  sysctl -w net.core.rmem_default=262144 >/dev/null
  sysctl -w net.core.somaxconn=16384 >/dev/null
  sysctl -w net.core.netdev_max_backlog=16384 >/dev/null

  ns_relay sysctl -w \
    net.ipv4.tcp_wmem="4096 262144 16777216" >/dev/null
  ns_relay sysctl -w \
    net.ipv4.tcp_rmem="4096 262144 16777216" >/dev/null
  ns_relay sysctl -w \
    net.ipv4.tcp_max_syn_backlog=16384 >/dev/null
  ns_client sysctl -w \
    net.ipv4.tcp_wmem="4096 262144 16777216" >/dev/null
  ns_client sysctl -w \
    net.ipv4.tcp_rmem="4096 262144 16777216" >/dev/null

  # Raise file descriptor limits.
  ulimit -n 65536

  sleep 2

  # Verify connectivity.
  ns_relay python3 -c "
import socket
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('0.0.0.0', 9999)); s.listen(1)
c, _ = s.accept(); c.send(b'ok'); c.close(); s.close()
" &
  local CHECK_PID=$!
  sleep 1
  local result
  result=$(ns_client python3 -c "
import socket
s = socket.socket(); s.settimeout(3)
try:
    s.connect(('$RELAY_IP', 9999))
    print(s.recv(10).decode())
except Exception as e:
    print(f'FAIL:{e}')
s.close()
" 2>/dev/null)
  wait "$CHECK_PID" 2>/dev/null || true

  if [[ "$result" == "ok" ]]; then
    log "  TCP connectivity: OK"
  else
    log "  ERROR: no TCP connectivity ($result)"
    exit 1
  fi
}

tune_system() {
  log "Tuning system..."
  for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo performance > "$cpu" 2>/dev/null || true
  done
  echo 0 > /proc/sys/kernel/nmi_watchdog 2>/dev/null || true
}

collect_sysinfo() {
  {
    echo "date: $(date -Iseconds)"
    echo "kernel: $(uname -r)"
    echo "cpu: $(lscpu | grep 'Model name' | \
      sed 's/.*: *//')"
    echo "cores: $(nproc)"
    echo "governor: $(cat \
      /sys/devices/system/cpu/cpu4/cpufreq/scaling_governor)"
    echo "relay_cores: $HD_CORES"
    echo "client_cores: $CLIENT_CORES"
    echo "workers: $WORKERS"
    echo "size: ${SIZE}B"
    echo "duration: ${DURATION}s"
    echo "rate: unlimited"
    echo "pair_counts: $PAIR_COUNTS"
  } > "${OUTPUT_DIR}/system_info.txt"
}

wait_ns_port() {
  local ns=$1 host=$2 port=$3 timeout=${4:-10}
  local deadline=$((SECONDS + timeout))
  while ! ip netns exec "$ns" timeout 1 bash -c \
      "echo >/dev/tcp/$host/$port" 2>/dev/null; do
    if ((SECONDS >= deadline)); then
      log "ERROR: $host:$port not ready after ${timeout}s"
      return 1
    fi
    sleep 0.1
  done
}

# Run one scale test point.
run_point() {
  local label=$1 port=$2 pairs=$3
  local tls_flag="${4:-}"
  local peers=$((pairs * 2))

  log "  ${label}: ${pairs} pairs (${peers} peers)..."

  local outfile="${OUTPUT_DIR}/${label}_${pairs}p.json"
  local raw
  raw=$(ns_client taskset -c "$CLIENT_CORES" \
    "$SCALE" --host "$RELAY_IP" --port "$port" \
    --peers "$peers" --active-pairs "$pairs" \
    --duration "$DURATION" --msg-size "$SIZE" \
    --rate-mbps "$RATE" $tls_flag --json \
    2>/dev/null) || true

  echo "$raw" | sed -n '/{/,/}/p' > "$outfile"

  python3 -c "
import json
try:
    d = json.load(open('$outfile'))
    print(f'    {d[\"throughput_mbps\"]:.1f} Mbps, '
          f'{d[\"message_loss_pct\"]:.2f}% loss, '
          f'{d[\"connect_time_ms\"]:.0f}ms connect')
except:
    print('    PARSE ERROR')
" 2>/dev/null
  sleep 1
}

# -- Per-relay test suites -------------------------------------------

test_ts() {
  log ""
  log "========== Tailscale derper =========="
  kill_relays

  ns_relay "$DERPER" -dev \
    2>"${OUTPUT_DIR}/derper.log" &
  local TS_PID=$!
  PIDS+=("$TS_PID")

  if ! wait_ns_port "$NS_CLIENT" "$RELAY_IP" \
      "$TS_PORT" 10; then
    log "ERROR: derper failed to start"
    return 1
  fi
  log "derper started (pid=$TS_PID)"

  for pairs in $PAIR_COUNTS; do
    run_point "ts" "$TS_PORT" "$pairs"
  done

  kill "$TS_PID" 2>/dev/null || true
  wait "$TS_PID" 2>/dev/null || true
  PIDS=()
  sleep 2
}

test_hd() {
  log ""
  log "========== Hyper-DERP (plain TCP) =========="
  kill_relays

  ns_relay "$RELAY" --port "$HD_PORT" --workers "$WORKERS" \
    --pin-workers "$HD_CORES" \
    2>"${OUTPUT_DIR}/hyper-derp.log" &
  local HD_PID=$!
  PIDS+=("$HD_PID")

  if ! wait_ns_port "$NS_CLIENT" "$RELAY_IP" \
      "$HD_PORT" 10; then
    log "ERROR: hyper-derp failed to start"
    return 1
  fi
  log "hyper-derp started (pid=$HD_PID)"

  for pairs in $PAIR_COUNTS; do
    run_point "hd" "$HD_PORT" "$pairs"
  done

  kill "$HD_PID" 2>/dev/null || true
  wait "$HD_PID" 2>/dev/null || true
  PIDS=()
  sleep 2
}

test_hd_ktls() {
  log ""
  log "========== Hyper-DERP (kTLS) =========="
  kill_relays

  local cert_dir="${OUTPUT_DIR}/certs"
  mkdir -p "$cert_dir"
  openssl req -x509 -newkey ec \
    -pkeyopt ec_paramgen_curve:prime256v1 \
    -keyout "${cert_dir}/key.pem" \
    -out "${cert_dir}/cert.pem" \
    -days 1 -nodes -subj '/CN=localhost' \
    2>/dev/null
  log "  TLS certs generated"

  ns_relay "$RELAY" --port "$KTLS_PORT" \
    --workers "$WORKERS" \
    --pin-workers "$HD_CORES" \
    --tls-cert "${cert_dir}/cert.pem" \
    --tls-key "${cert_dir}/key.pem" \
    2>"${OUTPUT_DIR}/hyper-derp-ktls.log" &
  local HD_PID=$!
  PIDS+=("$HD_PID")

  if ! wait_ns_port "$NS_CLIENT" "$RELAY_IP" \
      "$KTLS_PORT" 10; then
    log "ERROR: hyper-derp (kTLS) failed to start"
    return 1
  fi
  log "hyper-derp (kTLS) started (pid=$HD_PID)"

  for pairs in $PAIR_COUNTS; do
    run_point "ktls" "$KTLS_PORT" "$pairs" "--tls"
  done

  kill "$HD_PID" 2>/dev/null || true
  wait "$HD_PID" 2>/dev/null || true
  PIDS=()
  sleep 2
}

# -- Summary ---------------------------------------------------------

print_summary() {
  log ""
  log "=========================================="
  log "Results: $OUTPUT_DIR"
  log ""

  python3 -c "
import json, os

pair_counts = [int(x) for x in '$PAIR_COUNTS'.split()]
dir = '$OUTPUT_DIR'

def load(prefix, pairs):
    try:
        return json.load(open(
            os.path.join(dir, f'{prefix}_{pairs}p.json')))
    except:
        return None

print(f'{\"Pairs\":>6} | {\"TS Mbps\":>8} {\"Loss\":>7} | '
      f'{\"HD Mbps\":>8} {\"Loss\":>7} | '
      f'{\"kTLS Mbps\":>9} {\"Loss\":>7}')
print('-' * 72)
for pairs in pair_counts:
    ts = load('ts', pairs)
    hd = load('hd', pairs)
    kt = load('ktls', pairs)
    ts_tp = ts['throughput_mbps'] if ts else 0
    ts_l = f'{ts[\"message_loss_pct\"]:.1f}%' if ts else '-'
    hd_tp = hd['throughput_mbps'] if hd else 0
    hd_l = f'{hd[\"message_loss_pct\"]:.1f}%' if hd else '-'
    kt_tp = kt['throughput_mbps'] if kt else 0
    kt_l = f'{kt[\"message_loss_pct\"]:.1f}%' if kt else '-'
    print(f'{pairs:>6} | {ts_tp:>8.1f} {ts_l:>7} | '
          f'{hd_tp:>8.1f} {hd_l:>7} | '
          f'{kt_tp:>9.1f} {kt_l:>7}')
" 2>/dev/null || true
}

# ====================================================================

main() {
  if [[ "$(id -u)" -ne 0 ]]; then
    log "ERROR: must run as root"
    exit 1
  fi

  for bin in "$RELAY" "$SCALE" "$DERPER"; do
    if [[ ! -x "$bin" ]]; then
      log "ERROR: missing: $bin"
      exit 1
    fi
  done

  log "Scaling sweep: TS vs HD vs HD+kTLS"
  log "  CPU: $(lscpu | grep 'Model name' | \
    sed 's/.*: *//')"
  log "  Relay cores: $HD_CORES"
  log "  Client cores: $CLIENT_CORES"
  log "  Pair counts: $PAIR_COUNTS"
  log "  Duration: ${DURATION}s per point"
  log "  Rate: unlimited"
  log "  Output: $OUTPUT_DIR"

  setup_network
  tune_system
  collect_sysinfo

  test_ts
  test_hd
  test_hd_ktls

  print_summary
}

main
