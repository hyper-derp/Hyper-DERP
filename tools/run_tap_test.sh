#!/usr/bin/env bash
# Bare-metal test over bridged veth: Hyper-DERP vs Tailscale derper.
#
# Creates two network namespaces with veth pairs on virbr-targets.
# Relay runs in ns-relay, client in ns-client. Traffic goes through
# the real kernel bridge — ARP, TCP, bridge forwarding — same path
# as VM tap interfaces but without virtio overhead.
#
# CPU layout (i5-13600KF):
#   P-cores 0-5 (logical 0-11, HT, 5.1 GHz)
#   E-cores 6-13 (logical 12-19, 3.9 GHz)
#
# Pinning strategy:
#   Relay workers:  P-cores 2-3 (logical 4,5)
#   Test client:    E-cores 6-9 (logical 12-15)
#
# Usage: sudo ./run_tap_test.sh [output_dir]
#   Must run as root for netns, CPU pinning, perf, sysctl.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build"
RELAY="${BUILD_DIR}/hyper-derp"
SCALE="${BUILD_DIR}/tools/derp-scale-test"
CLIENT="${BUILD_DIR}/tools/derp-test-client"
DERPER="/home/karl/go/bin/derper"

# Network config — IPs outside the VM DHCP range.
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
WORKERS=2

# Relay pinned to P-cores 2-3 (logical 4,5).
HD_CORES="4,5"
# Client pinned to E-cores (logical 12-15).
CLIENT_CORES="12,13,14,15"

# Scale test params.
PEERS=20
PAIRS=10
DURATION=10
SIZE=1400

# Rate sweep.
RATES="100 500 1000 2000 5000 10000 20000 50000"

# Latency test.
PING_COUNT=5000
PING_WARMUP=500

OUTPUT_DIR="${1:-${PROJECT_DIR}/bench_results/tap_metal-$(date +%Y%m%d)}"
mkdir -p "$OUTPUT_DIR"

log() { echo "[$(date +%H:%M:%S)] $*"; }

# Run command in relay namespace.
ns_relay() { ip netns exec "$NS_RELAY" "$@"; }
# Run command in client namespace.
ns_client() { ip netns exec "$NS_CLIENT" "$@"; }

PIDS=()
cleanup() {
  log "Cleaning up..."
  for pid in "${PIDS[@]}"; do
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  done

  # Remove namespaces (also removes veth pairs).
  ip netns del "$NS_RELAY" 2>/dev/null || true
  ip netns del "$NS_CLIENT" 2>/dev/null || true
  # Clean host-side veths just in case.
  ip link del "$VETH_RELAY_HOST" 2>/dev/null || true
  ip link del "$VETH_CLIENT_HOST" 2>/dev/null || true

  # Restore governor.
  for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo powersave > "$cpu" 2>/dev/null || true
  done
}
trap cleanup EXIT

# -- Network namespace + veth setup --

setup_network() {
  log "Setting up network namespaces on bridge $BRIDGE..."

  # Verify bridge exists.
  if ! ip link show "$BRIDGE" &>/dev/null; then
    log "ERROR: bridge $BRIDGE does not exist"
    exit 1
  fi

  # Clean any stale state.
  ip netns del "$NS_RELAY" 2>/dev/null || true
  ip netns del "$NS_CLIENT" 2>/dev/null || true
  ip link del "$VETH_RELAY_HOST" 2>/dev/null || true
  ip link del "$VETH_CLIENT_HOST" 2>/dev/null || true

  # Create namespaces.
  ip netns add "$NS_RELAY"
  ip netns add "$NS_CLIENT"

  # Create veth pairs: host-side plugs into bridge,
  # ns-side gets the IP.
  ip link add "$VETH_RELAY_HOST" type veth \
    peer name "$VETH_RELAY_NS"
  ip link add "$VETH_CLIENT_HOST" type veth \
    peer name "$VETH_CLIENT_NS"

  # Move ns-side into respective namespaces.
  ip link set "$VETH_RELAY_NS" netns "$NS_RELAY"
  ip link set "$VETH_CLIENT_NS" netns "$NS_CLIENT"

  # Attach host-side to bridge.
  ip link set "$VETH_RELAY_HOST" master "$BRIDGE"
  ip link set "$VETH_CLIENT_HOST" master "$BRIDGE"
  ip link set "$VETH_RELAY_HOST" up
  ip link set "$VETH_CLIENT_HOST" up

  # Skip STP learning delay on our veth ports.
  bridge link set dev "$VETH_RELAY_HOST" state 3 2>/dev/null || true
  bridge link set dev "$VETH_CLIENT_HOST" state 3 2>/dev/null || true

  # Configure ns-side interfaces.
  ns_relay ip addr add "${RELAY_IP}/${NETMASK}" \
    dev "$VETH_RELAY_NS"
  ns_relay ip link set "$VETH_RELAY_NS" up
  ns_relay ip link set lo up

  ns_client ip addr add "${CLIENT_IP}/${NETMASK}" \
    dev "$VETH_CLIENT_NS"
  ns_client ip link set "$VETH_CLIENT_NS" up
  ns_client ip link set lo up

  sleep 1

  # Global TCP tuning (net.core.* is not per-namespace).
  sysctl -w net.core.wmem_max=16777216 >/dev/null
  sysctl -w net.core.rmem_max=16777216 >/dev/null
  sysctl -w net.core.wmem_default=262144 >/dev/null
  sysctl -w net.core.rmem_default=262144 >/dev/null
  sysctl -w net.core.somaxconn=4096 >/dev/null

  # Per-namespace TCP tuning (net.ipv4.* is per-ns).
  ns_relay sysctl -w net.ipv4.tcp_wmem="4096 262144 16777216" \
    >/dev/null
  ns_relay sysctl -w net.ipv4.tcp_rmem="4096 262144 16777216" \
    >/dev/null

  ns_client sysctl -w net.ipv4.tcp_wmem="4096 262144 16777216" \
    >/dev/null
  ns_client sysctl -w net.ipv4.tcp_rmem="4096 262144 16777216" \
    >/dev/null

  # Wait for STP to transition veths to forwarding.
  sleep 2

  # Verify TCP connectivity using Python (nc may not be available,
  # ICMP may be filtered by libvirt nftables).
  ns_relay python3 -c "
import socket, threading
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
    log "  ns-client -> ns-relay: TCP OK"
  else
    log "  ERROR: no TCP connectivity ($result)"
    exit 1
  fi

  log "  $NS_RELAY: $RELAY_IP ($VETH_RELAY_NS)"
  log "  $NS_CLIENT: $CLIENT_IP ($VETH_CLIENT_NS)"
  log "  bridge: $BRIDGE"
}

# -- System tuning (requires root) --

tune_system() {
  log "Tuning system for benchmarking..."

  # Performance governor on all cores.
  for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo performance > "$cpu" 2>/dev/null || true
  done

  # Disable NMI watchdog (reduces jitter).
  echo 0 > /proc/sys/kernel/nmi_watchdog 2>/dev/null || true

  log "  governor=performance, NMI off, TCP 16MB (per-ns)"
}

# -- Collect system info --

collect_sysinfo() {
  {
    echo "date: $(date -Iseconds)"
    echo "kernel: $(uname -r)"
    echo "cpu: $(lscpu | grep 'Model name' | sed 's/.*: *//')"
    echo "cores: $(nproc)"
    echo "governor: $(cat /sys/devices/system/cpu/cpu4/cpufreq/scaling_governor)"
    echo "relay_cores: $HD_CORES"
    echo "client_cores: $CLIENT_CORES"
    echo "workers: $WORKERS"
    echo "peers: $PEERS"
    echo "pairs: $PAIRS"
    echo "duration: ${DURATION}s"
    echo "size: ${SIZE}B"
    echo "rates: $RATES"
    echo "network: veth on bridge ($BRIDGE)"
    echo "relay_ip: $RELAY_IP (ns: $NS_RELAY)"
    echo "client_ip: $CLIENT_IP (ns: $NS_CLIENT)"
    echo "tcp_wmem: $(ns_relay cat /proc/sys/net/ipv4/tcp_wmem)"
    echo "wmem_max: $(ns_relay cat /proc/sys/net/core/wmem_max)"
  } > "${OUTPUT_DIR}/system_info.txt"
}

# -- Wait for port inside a namespace --

wait_ns_port() {
  local ns=$1 host=$2 port=$3 timeout=${4:-10}
  local deadline=$((SECONDS + timeout))
  while ! ip netns exec "$ns" timeout 1 bash -c \
      "echo >/dev/tcp/$host/$port" 2>/dev/null; do
    if ((SECONDS >= deadline)); then
      log "ERROR: $host:$port not ready in $ns after ${timeout}s"
      return 1
    fi
    sleep 0.1
  done
}

# -- Scale test runner --

run_scale() {
  local label=$1 port=$2 rate=$3
  local rate_flag="--rate-mbps $rate"

  log "  scale: ${rate} Mbps ..."

  local outfile="${OUTPUT_DIR}/${label}.json"
  local raw
  raw=$(ns_client taskset -c "$CLIENT_CORES" \
    "$SCALE" --host "$RELAY_IP" --port "$port" \
    --peers "$PEERS" --active-pairs "$PAIRS" \
    --duration "$DURATION" --msg-size "$SIZE" \
    $rate_flag --json 2>/dev/null) || true

  echo "$raw" | sed -n '/{/,/}/p' > "$outfile"

  python3 -c "
import json, sys
try:
    d = json.load(open('$outfile'))
    print(f'    {d[\"throughput_mbps\"]:.1f} Mbps, '
          f'{d[\"message_loss_pct\"]:.2f}% loss, '
          f'{d.get(\"send_errors\", 0)} errs')
except:
    print('    PARSE ERROR')
" 2>/dev/null
  sleep 2
}

# -- Latency test runner --

run_latency() {
  local label=$1 port=$2
  log "  latency 1400B ..."

  local echo_err
  echo_err=$(mktemp)

  ns_client taskset -c "$CLIENT_CORES" \
    "$CLIENT" --host "$RELAY_IP" --port "$port" \
    --mode echo \
    --count $((PING_COUNT + PING_WARMUP + 500)) \
    --timeout 60000 \
    2>"$echo_err" &
  local ECHO_PID=$!
  PIDS+=("$ECHO_PID")
  sleep 2

  local echo_key
  echo_key=$(grep '^pubkey:' "$echo_err" | awk '{print $2}')
  rm -f "$echo_err"

  if [[ -z "$echo_key" ]]; then
    log "    ERROR: no echo key"
    kill "$ECHO_PID" 2>/dev/null || true
    wait "$ECHO_PID" 2>/dev/null || true
    return 1
  fi

  local raw_file
  raw_file=$(mktemp)

  ns_client taskset -c "$CLIENT_CORES" \
    "$CLIENT" --host "$RELAY_IP" --port "$port" \
    --mode ping --count "$PING_COUNT" --size "$SIZE" \
    --dst-key "$echo_key" --warmup "$PING_WARMUP" \
    --json --raw-latency \
    --label "$label" --timeout 60000 \
    2>/dev/null > "$raw_file" || true

  kill "$ECHO_PID" 2>/dev/null || true
  wait "$ECHO_PID" 2>/dev/null || true

  python3 -c "
import json
text = open('$raw_file').read()
start = text.index('{')
depth = 0
for i in range(start, len(text)):
    if text[i] == '{': depth += 1
    elif text[i] == '}': depth -= 1
    if depth == 0:
        data = json.loads(text[start:i+1])
        with open('${OUTPUT_DIR}/${label}.json', 'w') as f:
            json.dump(data, f, indent=2)
        l = data['latency_ns']
        print(f'    p50={l[\"p50\"]/1000:.0f}us '
              f'p90={l[\"p90\"]/1000:.0f}us '
              f'p99={l[\"p99\"]/1000:.0f}us '
              f'pps={data[\"throughput_pps\"]:.0f}')
        break
" 2>/dev/null
  rm -f "$raw_file"
}

# -- Loaded latency --

run_loaded_latency() {
  local label=$1 port=$2 bg_rate=$3
  log "  loaded latency @ ${bg_rate} Mbps background ..."

  ns_client taskset -c 14,15 \
    "$SCALE" --host "$RELAY_IP" --port "$port" \
    --peers 10 --active-pairs 5 \
    --duration 15 --msg-size "$SIZE" \
    --rate-mbps "$bg_rate" --json \
    2>/dev/null >/dev/null &
  local BG_PID=$!
  PIDS+=("$BG_PID")
  sleep 3

  local echo_err
  echo_err=$(mktemp)

  ns_client taskset -c 12 \
    "$CLIENT" --host "$RELAY_IP" --port "$port" \
    --mode echo \
    --count 3000 --timeout 30000 \
    2>"$echo_err" &
  local ECHO_PID=$!
  PIDS+=("$ECHO_PID")
  sleep 2

  local echo_key
  echo_key=$(grep '^pubkey:' "$echo_err" | awk '{print $2}')
  rm -f "$echo_err"

  if [[ -z "$echo_key" ]]; then
    log "    ERROR: no echo key"
    kill "$ECHO_PID" "$BG_PID" 2>/dev/null || true
    wait "$ECHO_PID" "$BG_PID" 2>/dev/null || true
    return 1
  fi

  local raw_file
  raw_file=$(mktemp)

  ns_client taskset -c 13 \
    "$CLIENT" --host "$RELAY_IP" --port "$port" \
    --mode ping --count 2000 --size "$SIZE" \
    --dst-key "$echo_key" --warmup 200 \
    --json --raw-latency \
    --label "$label" --timeout 30000 \
    2>/dev/null > "$raw_file" || true

  kill "$ECHO_PID" 2>/dev/null || true
  wait "$ECHO_PID" 2>/dev/null || true
  wait "$BG_PID" 2>/dev/null || true

  python3 -c "
import json
text = open('$raw_file').read()
start = text.index('{')
depth = 0
for i in range(start, len(text)):
    if text[i] == '{': depth += 1
    elif text[i] == '}': depth -= 1
    if depth == 0:
        data = json.loads(text[start:i+1])
        with open('${OUTPUT_DIR}/${label}.json', 'w') as f:
            json.dump(data, f, indent=2)
        l = data['latency_ns']
        print(f'    p50={l[\"p50\"]/1000:.0f}us '
              f'p90={l[\"p90\"]/1000:.0f}us '
              f'p99={l[\"p99\"]/1000:.0f}us '
              f'pps={data[\"throughput_pps\"]:.0f}')
        break
" 2>/dev/null
  rm -f "$raw_file"
}

# -- Perf stat capture --

run_perf_stat() {
  local label=$1 port=$2 rate=$3 pid=$4
  log "  perf stat @ ${rate} Mbps ..."

  perf stat -e task-clock,cycles,instructions,cache-misses,\
cache-references,context-switches,cpu-migrations \
    -p "$pid" \
    -o "${OUTPUT_DIR}/${label}_perf.txt" -- \
    sleep "$DURATION" 2>/dev/null &
  local PERF_PID=$!

  ns_client taskset -c "$CLIENT_CORES" \
    "$SCALE" --host "$RELAY_IP" --port "$port" \
    --peers "$PEERS" --active-pairs "$PAIRS" \
    --duration "$DURATION" --msg-size "$SIZE" \
    --rate-mbps "$rate" --json \
    2>/dev/null | sed -n '/{/,/}/p' \
    > "${OUTPUT_DIR}/${label}.json"

  wait "$PERF_PID" 2>/dev/null || true

  grep -E 'instructions|context-switch' \
    "${OUTPUT_DIR}/${label}_perf.txt" 2>/dev/null || true
  sleep 2
}

# =====================================================================

test_ts() {
  log ""
  log "========== Tailscale derper (veth bridge) =========="

  # Run derper inside the relay namespace.
  ns_relay "$DERPER" -dev \
    2>"${OUTPUT_DIR}/derper.log" &
  local TS_PID=$!
  PIDS+=("$TS_PID")

  if ! wait_ns_port "$NS_CLIENT" "$RELAY_IP" "$TS_PORT" 10; then
    log "ERROR: derper failed to start"
    return 1
  fi
  log "derper started (pid=$TS_PID) in $NS_RELAY at $RELAY_IP:$TS_PORT"

  for rate in $RATES; do
    run_scale "ts_${rate}mbps" "$TS_PORT" "$rate"
  done

  run_latency "ts_lat_1400B" "$TS_PORT"

  for bg in 500 2000; do
    run_loaded_latency "ts_loaded_lat_${bg}mbps" \
      "$TS_PORT" "$bg"
    sleep 2
  done

  run_perf_stat "ts_perf_5000" "$TS_PORT" 5000 "$TS_PID"

  kill "$TS_PID" 2>/dev/null || true
  wait "$TS_PID" 2>/dev/null || true
  sleep 2
}

test_hd() {
  log ""
  log "========== Hyper-DERP (veth bridge) =========="

  # Run hyper-derp inside the relay namespace.
  ns_relay "$RELAY" --port "$HD_PORT" --workers "$WORKERS" \
    --pin-workers "$HD_CORES" \
    2>"${OUTPUT_DIR}/hyper-derp.log" &
  local HD_PID=$!
  PIDS+=("$HD_PID")

  if ! wait_ns_port "$NS_CLIENT" "$RELAY_IP" "$HD_PORT" 10; then
    log "ERROR: hyper-derp failed to start"
    return 1
  fi
  log "hyper-derp started (pid=$HD_PID) in $NS_RELAY at $RELAY_IP:$HD_PORT"

  for rate in $RATES; do
    run_scale "hd_${rate}mbps" "$HD_PORT" "$rate"
  done

  run_latency "hd_lat_1400B" "$HD_PORT"

  for bg in 500 2000; do
    run_loaded_latency "hd_loaded_lat_${bg}mbps" \
      "$HD_PORT" "$bg"
    sleep 2
  done

  run_perf_stat "hd_perf_5000" "$HD_PORT" 5000 "$HD_PID"

  kill "$HD_PID" 2>/dev/null || true
  wait "$HD_PID" 2>/dev/null || true
  sleep 2
}

main() {
  if [[ "$(id -u)" -ne 0 ]]; then
    log "ERROR: must run as root"
    exit 1
  fi

  for bin in "$RELAY" "$SCALE" "$CLIENT" "$DERPER"; do
    if [[ ! -x "$bin" ]]; then
      log "ERROR: missing: $bin"
      exit 1
    fi
  done

  log "Veth bridge test: real TCP stack, no VM/virtio overhead"
  log "  CPU: $(lscpu | grep 'Model name' | sed 's/.*: *//')"
  log "  Bridge: $BRIDGE"
  log "  Relay: $RELAY_IP ($NS_RELAY) -> cores $HD_CORES"
  log "  Client: $CLIENT_IP ($NS_CLIENT) -> cores $CLIENT_CORES"
  log "  Rates: $RATES Mbps"
  log "  Duration: ${DURATION}s per point"
  log "  Output: $OUTPUT_DIR"

  setup_network
  tune_system
  collect_sysinfo

  test_ts
  test_hd

  log ""
  log "=========================================="
  log "All tests complete: $OUTPUT_DIR"
  log ""

  # Summary table.
  python3 -c "
import json, os

rates = [100, 500, 1000, 2000, 5000, 10000, 20000, 50000]
dir = '$OUTPUT_DIR'

print(f'{\"Rate\":>10} | {\"TS Mbps\":>10} {\"TS Loss\":>10} | '
      f'{\"HD Mbps\":>10} {\"HD Loss\":>10} | {\"HD/TS\":>6}')
print('-' * 75)
for rate in rates:
    ts = hd = None
    try:
        ts = json.load(open(os.path.join(dir, f'ts_{rate}mbps.json')))
    except: pass
    try:
        hd = json.load(open(os.path.join(dir, f'hd_{rate}mbps.json')))
    except: pass
    ts_tp = ts['throughput_mbps'] if ts else 0
    ts_loss = f'{ts[\"message_loss_pct\"]:.2f}%' if ts else '-'
    hd_tp = hd['throughput_mbps'] if hd else 0
    hd_loss = f'{hd[\"message_loss_pct\"]:.2f}%' if hd else '-'
    ratio = f'{hd_tp/ts_tp:.1f}x' if ts_tp > 0 else '-'
    ts_s = f'{ts_tp:.1f}' if ts else '-'
    hd_s = f'{hd_tp:.1f}' if hd else '-'
    print(f'{rate:>10} | {ts_s:>10} {ts_loss:>10} | '
          f'{hd_s:>10} {hd_loss:>10} | {ratio:>6}')
" 2>/dev/null || true
}

main
