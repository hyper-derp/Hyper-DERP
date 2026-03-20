#!/usr/bin/env bash
# Compare: Tailscale derper vs Hyper-DERP (plain) vs Hyper-DERP (kTLS).
#
# Uses veth bridge with network namespaces (same setup as run_tap_test.sh).
# Requires root for netns, CPU pinning, perf, sysctl.
#
# Usage: sudo ./run_ktls_comparison.sh [output_dir]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build"
RELAY="${BUILD_DIR}/hyper-derp"
SCALE="${BUILD_DIR}/tools/derp-scale-test"
CLIENT="${BUILD_DIR}/tools/derp-test-client"
DERPER="${DERPER:?Set DERPER env var (path to Go derper binary)}"

# Network config.
BRIDGE="${BRIDGE:-virbr-targets}"
NS_RELAY="ns-relay"
NS_CLIENT="ns-client"
VETH_RELAY_HOST="veth-rh"
VETH_RELAY_NS="veth-rns"
VETH_CLIENT_HOST="veth-ch"
VETH_CLIENT_NS="veth-cns"
RELAY_IP="${RELAY_IP:-10.101.2.1}"
CLIENT_IP="${CLIENT_IP:-10.101.2.2}"
NETMASK="20"

TS_PORT=3340
HD_PORT=3341
WORKERS=2

# CPU pinning (i5-13600KF).
HD_CORES="4,5"
CLIENT_CORES="12,13,14,15"

# Test params.
PEERS=20
PAIRS=10
DURATION=10
SIZE=1400

# Rate sweep.
RATES="100 500 1000 2000 5000 10000 20000 50000"

# Latency.
PING_COUNT=5000
PING_WARMUP=500

OUTPUT_DIR="${1:-${PROJECT_DIR}/bench_results/ktls-$(date +%Y%m%d)}"
mkdir -p "$OUTPUT_DIR"

log() { echo "[$(date +%H:%M:%S)] $*"; }

ns_relay() { ip netns exec "$NS_RELAY" "$@"; }
ns_client() { ip netns exec "$NS_CLIENT" "$@"; }

PIDS=()
kill_relays() {
  killall -9 hyper-derp derper derp-scale-test \
    derp-test-client 2>/dev/null || true
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
  log "Setting up network namespaces on bridge $BRIDGE..."

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

  sysctl -w net.core.wmem_max=16777216 >/dev/null
  sysctl -w net.core.rmem_max=16777216 >/dev/null
  sysctl -w net.core.wmem_default=262144 >/dev/null
  sysctl -w net.core.rmem_default=262144 >/dev/null
  sysctl -w net.core.somaxconn=4096 >/dev/null

  ns_relay sysctl -w \
    net.ipv4.tcp_wmem="4096 262144 16777216" >/dev/null
  ns_relay sysctl -w \
    net.ipv4.tcp_rmem="4096 262144 16777216" >/dev/null
  ns_client sysctl -w \
    net.ipv4.tcp_wmem="4096 262144 16777216" >/dev/null
  ns_client sysctl -w \
    net.ipv4.tcp_rmem="4096 262144 16777216" >/dev/null

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
    echo "peers: $PEERS"
    echo "pairs: $PAIRS"
    echo "duration: ${DURATION}s"
    echo "size: ${SIZE}B"
    echo "rates: $RATES"
    echo "tls: kTLS TLS_1.3 AES-GCM"
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

# -- Test runners ---------------------------------------------------

run_scale() {
  local label=$1 port=$2 rate=$3
  local tls_flag="${4:-}"

  log "  scale: ${rate} Mbps ..."

  local outfile="${OUTPUT_DIR}/${label}.json"
  local raw
  raw=$(ns_client taskset -c "$CLIENT_CORES" \
    "$SCALE" --host "$RELAY_IP" --port "$port" \
    --peers "$PEERS" --active-pairs "$PAIRS" \
    --duration "$DURATION" --msg-size "$SIZE" \
    --rate-mbps "$rate" $tls_flag --json \
    2>/dev/null) || true

  echo "$raw" | sed -n '/{/,/}/p' > "$outfile"

  python3 -c "
import json
try:
    d = json.load(open('$outfile'))
    print(f'    {d[\"throughput_mbps\"]:.1f} Mbps, '
          f'{d[\"message_loss_pct\"]:.2f}% loss')
except:
    print('    PARSE ERROR')
" 2>/dev/null
  sleep 2
}

run_latency() {
  local label=$1 port=$2
  local tls_flag="${3:-}"
  log "  latency 1400B ..."

  local echo_err
  echo_err=$(mktemp)

  ns_client taskset -c "$CLIENT_CORES" \
    "$CLIENT" --host "$RELAY_IP" --port "$port" \
    --mode echo \
    --count $((PING_COUNT + PING_WARMUP + 500)) \
    --timeout 60000 $tls_flag \
    2>"$echo_err" &
  local ECHO_PID=$!
  PIDS+=("$ECHO_PID")
  sleep 2

  local echo_key
  echo_key=$(grep '^pubkey:' "$echo_err" | \
    awk '{print $2}')
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
    --label "$label" --timeout 60000 $tls_flag \
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
              f'p99={l[\"p99\"]/1000:.0f}us')
        break
" 2>/dev/null
  rm -f "$raw_file"
}

run_loaded_latency() {
  local label=$1 port=$2 bg_rate=$3
  local tls_flag="${4:-}"
  log "  loaded latency @ ${bg_rate} Mbps ..."

  ns_client taskset -c 14,15 \
    "$SCALE" --host "$RELAY_IP" --port "$port" \
    --peers 10 --active-pairs 5 \
    --duration 15 --msg-size "$SIZE" \
    --rate-mbps "$bg_rate" $tls_flag --json \
    2>/dev/null >/dev/null &
  local BG_PID=$!
  PIDS+=("$BG_PID")
  sleep 3

  local echo_err
  echo_err=$(mktemp)

  ns_client taskset -c 12 \
    "$CLIENT" --host "$RELAY_IP" --port "$port" \
    --mode echo --count 3000 --timeout 30000 \
    $tls_flag \
    2>"$echo_err" &
  local ECHO_PID=$!
  PIDS+=("$ECHO_PID")
  sleep 2

  local echo_key
  echo_key=$(grep '^pubkey:' "$echo_err" | \
    awk '{print $2}')
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
    --label "$label" --timeout 30000 $tls_flag \
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
              f'p99={l[\"p99\"]/1000:.0f}us')
        break
" 2>/dev/null
  rm -f "$raw_file"
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

  for rate in $RATES; do
    run_scale "ts_${rate}mbps" "$TS_PORT" "$rate"
  done

  run_latency "ts_lat_1400B" "$TS_PORT"

  for bg in 500 2000; do
    run_loaded_latency "ts_loaded_lat_${bg}mbps" \
      "$TS_PORT" "$bg"
    sleep 2
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

  for rate in $RATES; do
    run_scale "hd_${rate}mbps" "$HD_PORT" "$rate"
  done

  run_latency "hd_lat_1400B" "$HD_PORT"

  for bg in 500 2000; do
    run_loaded_latency "hd_loaded_lat_${bg}mbps" \
      "$HD_PORT" "$bg"
    sleep 2
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

  # Generate self-signed cert for kTLS.
  local cert_dir="${OUTPUT_DIR}/certs"
  mkdir -p "$cert_dir"
  openssl req -x509 -newkey ec \
    -pkeyopt ec_paramgen_curve:prime256v1 \
    -keyout "${cert_dir}/key.pem" \
    -out "${cert_dir}/cert.pem" \
    -days 1 -nodes -subj '/CN=localhost' \
    2>/dev/null
  log "  TLS certs generated"

  local KTLS_PORT=3342
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

  for rate in $RATES; do
    run_scale "ktls_${rate}mbps" "$KTLS_PORT" \
      "$rate" "--tls"
  done

  run_latency "ktls_lat_1400B" "$KTLS_PORT" "--tls"

  for bg in 500 2000; do
    run_loaded_latency "ktls_loaded_lat_${bg}mbps" \
      "$KTLS_PORT" "$bg" "--tls"
    sleep 2
  done

  kill "$HD_PID" 2>/dev/null || true
  wait "$HD_PID" 2>/dev/null || true
  PIDS=()
  sleep 2
}

# -- Summary table ---------------------------------------------------

print_summary() {
  log ""
  log "=========================================="
  log "Results: $OUTPUT_DIR"
  log ""

  python3 -c "
import json, os

rates = [100, 500, 1000, 2000, 5000, 10000, 20000, 50000]
dir = '$OUTPUT_DIR'

def load(prefix, rate):
    try:
        return json.load(open(
            os.path.join(dir, f'{prefix}_{rate}mbps.json')))
    except:
        return None

print(f'{\"Rate\":>8} | {\"TS Mbps\":>8} {\"Loss\":>7} | '
      f'{\"HD Mbps\":>8} {\"Loss\":>7} | '
      f'{\"kTLS Mbps\":>9} {\"Loss\":>7} | '
      f'{\"kTLS/TS\":>7}')
print('-' * 85)
for rate in rates:
    ts = load('ts', rate)
    hd = load('hd', rate)
    kt = load('ktls', rate)
    ts_tp = ts['throughput_mbps'] if ts else 0
    ts_l = f'{ts[\"message_loss_pct\"]:.1f}%' if ts else '-'
    hd_tp = hd['throughput_mbps'] if hd else 0
    hd_l = f'{hd[\"message_loss_pct\"]:.1f}%' if hd else '-'
    kt_tp = kt['throughput_mbps'] if kt else 0
    kt_l = f'{kt[\"message_loss_pct\"]:.1f}%' if kt else '-'
    ratio = f'{kt_tp/ts_tp:.1f}x' if ts_tp > 0 else '-'
    print(f'{rate:>8} | {ts_tp:>8.1f} {ts_l:>7} | '
          f'{hd_tp:>8.1f} {hd_l:>7} | '
          f'{kt_tp:>9.1f} {kt_l:>7} | {ratio:>7}')

# Latency summary.
print()
for prefix, name in [('ts', 'TS'), ('hd', 'HD'),
                      ('ktls', 'kTLS')]:
    try:
        idle = json.load(open(
            os.path.join(dir, f'{prefix}_lat_1400B.json')))
        l = idle['latency_ns']
        print(f'{name} idle: p50={l[\"p50\"]/1000:.0f}us '
              f'p90={l[\"p90\"]/1000:.0f}us '
              f'p99={l[\"p99\"]/1000:.0f}us')
    except:
        pass
    for bg in [500, 2000]:
        try:
            loaded = json.load(open(os.path.join(
                dir,
                f'{prefix}_loaded_lat_{bg}mbps.json')))
            l = loaded['latency_ns']
            print(f'{name} @{bg}M: p50={l[\"p50\"]/1000:.0f}us '
                  f'p90={l[\"p90\"]/1000:.0f}us '
                  f'p99={l[\"p99\"]/1000:.0f}us')
        except:
            pass
" 2>/dev/null || true
}

# ====================================================================

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

  log "kTLS comparison: TS vs HD vs HD+kTLS"
  log "  CPU: $(lscpu | grep 'Model name' | \
    sed 's/.*: *//')"
  log "  Relay cores: $HD_CORES"
  log "  Client cores: $CLIENT_CORES"
  log "  Rates: $RATES Mbps"
  log "  Duration: ${DURATION}s per point"
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
