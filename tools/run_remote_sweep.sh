#!/usr/bin/env bash
# Remote scaling sweep: throughput vs active peer pairs.
# Runs on separate relay and client machines connected via
# a direct link (e.g. 50GbE). Deploys binaries via scp,
# runs processes via ssh.
#
# Usage:
#   ./run_remote_sweep.sh \
#     --relay-host worker@10.0.0.1 \
#     --client-host worker@10.0.0.2 \
#     --relay-ip 10.0.0.1 \
#     [--relay-cores 0,1] [--client-cores 0,1,2,3] \
#     [--workers 2] [--output-dir path] [--duration 10]
#
# Prerequisites on both machines:
#   - derper binary at /home/karl/go/bin/derper (relay)
#   - SSH key auth configured
#   - Elevated permissions for sysctl tuning (sudo)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build"

# Defaults.
RELAY_HOST=""
CLIENT_HOST=""
RELAY_IP=""
RELAY_CORES=""
CLIENT_CORES=""
WORKERS=2
DURATION=10
SIZE=1400
RATE=0
PAIR_COUNTS="1 2 5 10 25 50 100 250 500 1000"
OUTPUT_DIR=""
DERPER_PATH="/home/karl/go/bin/derper"
REMOTE_DIR="/tmp/hyper-derp-bench"
SSH_KEY="${HOME}/.ssh/id_ed25519_targets"

TS_PORT=3340
HD_PORT=3341
KTLS_PORT=3342

# Parse args.
while [[ $# -gt 0 ]]; do
  case $1 in
    --relay-host) RELAY_HOST="$2"; shift 2 ;;
    --client-host) CLIENT_HOST="$2"; shift 2 ;;
    --relay-ip) RELAY_IP="$2"; shift 2 ;;
    --relay-cores) RELAY_CORES="$2"; shift 2 ;;
    --client-cores) CLIENT_CORES="$2"; shift 2 ;;
    --workers) WORKERS="$2"; shift 2 ;;
    --duration) DURATION="$2"; shift 2 ;;
    --pair-counts) PAIR_COUNTS="$2"; shift 2 ;;
    --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
    --ssh-key) SSH_KEY="$2"; shift 2 ;;
    --derper) DERPER_PATH="$2"; shift 2 ;;
    *) echo "Unknown flag: $1"; exit 1 ;;
  esac
done

if [[ -z "$RELAY_HOST" || -z "$CLIENT_HOST" || \
      -z "$RELAY_IP" ]]; then
  echo "Required: --relay-host, --client-host, --relay-ip"
  exit 1
fi

OUTPUT_DIR="${OUTPUT_DIR:-${PROJECT_DIR}/bench_results/remote-$(date +%Y%m%d)}"
mkdir -p "$OUTPUT_DIR"

log() { echo "[$(date +%H:%M:%S)] $*"; }

SSH_OPTS="-o StrictHostKeyChecking=no \
  -o ConnectTimeout=5 -o BatchMode=yes"
if [[ -f "$SSH_KEY" ]]; then
  SSH_OPTS="$SSH_OPTS -i $SSH_KEY"
fi

rssh() { ssh $SSH_OPTS "$RELAY_HOST" "$@"; }
cssh() { ssh $SSH_OPTS "$CLIENT_HOST" "$@"; }
rscp() { scp $SSH_OPTS "$@" "${RELAY_HOST}:${REMOTE_DIR}/"; }
cscp() { scp $SSH_OPTS "$@" "${CLIENT_HOST}:${REMOTE_DIR}/"; }

# -- Deployment ------------------------------------------------------

deploy() {
  log "Deploying binaries..."

  local relay_bin="${BUILD_DIR}/hyper-derp"
  local scale_bin="${BUILD_DIR}/tools/derp-scale-test"
  local client_bin="${BUILD_DIR}/tools/derp-test-client"

  for bin in "$relay_bin" "$scale_bin" "$client_bin"; do
    if [[ ! -x "$bin" ]]; then
      log "ERROR: missing $bin — run cmake --build build"
      exit 1
    fi
  done

  rssh "mkdir -p $REMOTE_DIR"
  cssh "mkdir -p $REMOTE_DIR"

  rscp "$relay_bin"
  rscp "$DERPER_PATH" 2>/dev/null || true
  cscp "$scale_bin"
  cscp "$client_bin"

  log "  Deployed to $REMOTE_DIR on both machines"
}

# -- System tuning ---------------------------------------------------

tune_remote() {
  local host_func=$1
  log "  Tuning $($host_func hostname)..."
  $host_func "sudo sysctl -w \
    net.core.wmem_max=16777216 \
    net.core.rmem_max=16777216 \
    net.core.wmem_default=262144 \
    net.core.rmem_default=262144 \
    net.core.somaxconn=16384 \
    net.core.netdev_max_backlog=16384 \
    net.ipv4.tcp_wmem='4096 262144 16777216' \
    net.ipv4.tcp_rmem='4096 262144 16777216' \
    net.ipv4.tcp_max_syn_backlog=16384 \
    >/dev/null 2>&1" || true

  # Performance governor.
  $host_func "for c in /sys/devices/system/cpu/cpu*/\
cpufreq/scaling_governor; do \
    echo performance | sudo tee \$c >/dev/null 2>&1; \
  done" || true

  # Raise fd limits.
  $host_func "ulimit -n 65536" || true
}

tune_system() {
  log "Tuning systems..."
  tune_remote rssh
  tune_remote cssh
}

# -- Collect system info ---------------------------------------------

collect_sysinfo() {
  {
    echo "date: $(date -Iseconds)"
    echo "relay_host: $RELAY_HOST"
    echo "client_host: $CLIENT_HOST"
    echo "relay_ip: $RELAY_IP"
    echo "relay_cores: ${RELAY_CORES:-auto}"
    echo "client_cores: ${CLIENT_CORES:-auto}"
    echo "workers: $WORKERS"
    echo "size: ${SIZE}B"
    echo "duration: ${DURATION}s"
    echo "rate: ${RATE:-unlimited}"
    echo "pair_counts: $PAIR_COUNTS"
    echo ""
    echo "--- relay ---"
    rssh "uname -r; lscpu | grep 'Model name'; \
      cat /proc/meminfo | head -1" 2>/dev/null || true
    echo ""
    echo "--- client ---"
    cssh "uname -r; lscpu | grep 'Model name'; \
      cat /proc/meminfo | head -1" 2>/dev/null || true
  } > "${OUTPUT_DIR}/system_info.txt"
}

# -- Process management ----------------------------------------------

kill_remote_relays() {
  rssh "killall -9 hyper-derp derper 2>/dev/null; true"
  cssh "killall -9 derp-scale-test \
    derp-test-client 2>/dev/null; true"
  sleep 0.5
}

wait_remote_port() {
  local host=$1 port=$2 timeout=${3:-15}
  local deadline=$((SECONDS + timeout))
  while ! cssh "timeout 1 bash -c \
      'echo >/dev/tcp/$host/$port'" 2>/dev/null; do
    if ((SECONDS >= deadline)); then
      log "ERROR: $host:$port not ready after ${timeout}s"
      return 1
    fi
    sleep 0.2
  done
}

# -- Test runner -----------------------------------------------------

run_point() {
  local label=$1 port=$2 pairs=$3
  local tls_flag="${4:-}"
  local peers=$((pairs * 2))

  log "  ${label}: ${pairs} pairs (${peers} peers)..."

  local outfile="${OUTPUT_DIR}/${label}_${pairs}p.json"
  local taskset_cmd=""
  if [[ -n "$CLIENT_CORES" ]]; then
    taskset_cmd="taskset -c $CLIENT_CORES"
  fi

  local raw
  raw=$(cssh "$taskset_cmd \
    ${REMOTE_DIR}/derp-scale-test \
    --host $RELAY_IP --port $port \
    --peers $peers --active-pairs $pairs \
    --duration $DURATION --msg-size $SIZE \
    --rate-mbps $RATE $tls_flag --json" \
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
  kill_remote_relays

  rssh "${REMOTE_DIR}/derper -dev" \
    2>"${OUTPUT_DIR}/derper.log" &
  local SSH_PID=$!

  if ! wait_remote_port "$RELAY_IP" "$TS_PORT" 15; then
    log "ERROR: derper failed to start"
    kill "$SSH_PID" 2>/dev/null || true
    return 1
  fi
  log "  derper started on $RELAY_HOST"

  for pairs in $PAIR_COUNTS; do
    run_point "ts" "$TS_PORT" "$pairs"
  done

  rssh "killall -9 derper 2>/dev/null; true"
  kill "$SSH_PID" 2>/dev/null || true
  wait "$SSH_PID" 2>/dev/null || true
  sleep 2
}

test_hd() {
  log ""
  log "========== Hyper-DERP (plain TCP) =========="
  kill_remote_relays

  local pin_flag=""
  if [[ -n "$RELAY_CORES" ]]; then
    pin_flag="--pin-workers $RELAY_CORES"
  fi

  rssh "${REMOTE_DIR}/hyper-derp \
    --port $HD_PORT --workers $WORKERS \
    $pin_flag" \
    2>"${OUTPUT_DIR}/hyper-derp.log" &
  local SSH_PID=$!

  if ! wait_remote_port "$RELAY_IP" "$HD_PORT" 15; then
    log "ERROR: hyper-derp failed to start"
    kill "$SSH_PID" 2>/dev/null || true
    return 1
  fi
  log "  hyper-derp started on $RELAY_HOST"

  for pairs in $PAIR_COUNTS; do
    run_point "hd" "$HD_PORT" "$pairs"
  done

  rssh "killall -9 hyper-derp 2>/dev/null; true"
  kill "$SSH_PID" 2>/dev/null || true
  wait "$SSH_PID" 2>/dev/null || true
  sleep 2
}

test_hd_ktls() {
  log ""
  log "========== Hyper-DERP (kTLS) =========="
  kill_remote_relays

  # Generate TLS certs on relay machine.
  rssh "openssl req -x509 -newkey ec \
    -pkeyopt ec_paramgen_curve:prime256v1 \
    -keyout ${REMOTE_DIR}/key.pem \
    -out ${REMOTE_DIR}/cert.pem \
    -days 1 -nodes -subj '/CN=localhost' \
    2>/dev/null"
  log "  TLS certs generated on relay"

  local pin_flag=""
  if [[ -n "$RELAY_CORES" ]]; then
    pin_flag="--pin-workers $RELAY_CORES"
  fi

  rssh "${REMOTE_DIR}/hyper-derp \
    --port $KTLS_PORT --workers $WORKERS \
    $pin_flag \
    --tls-cert ${REMOTE_DIR}/cert.pem \
    --tls-key ${REMOTE_DIR}/key.pem" \
    2>"${OUTPUT_DIR}/hyper-derp-ktls.log" &
  local SSH_PID=$!

  if ! wait_remote_port "$RELAY_IP" "$KTLS_PORT" 15; then
    log "ERROR: hyper-derp (kTLS) failed to start"
    kill "$SSH_PID" 2>/dev/null || true
    return 1
  fi
  log "  hyper-derp (kTLS) started on $RELAY_HOST"

  for pairs in $PAIR_COUNTS; do
    run_point "ktls" "$KTLS_PORT" "$pairs" "--tls"
  done

  rssh "killall -9 hyper-derp 2>/dev/null; true"
  kill "$SSH_PID" 2>/dev/null || true
  wait "$SSH_PID" 2>/dev/null || true
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
  log "Remote scaling sweep: TS vs HD vs HD+kTLS"
  log "  Relay: $RELAY_HOST ($RELAY_IP)"
  log "  Client: $CLIENT_HOST"
  log "  Workers: $WORKERS"
  log "  Relay cores: ${RELAY_CORES:-auto}"
  log "  Client cores: ${CLIENT_CORES:-auto}"
  log "  Pair counts: $PAIR_COUNTS"
  log "  Duration: ${DURATION}s per point"
  log "  Output: $OUTPUT_DIR"

  deploy
  tune_system
  collect_sysinfo

  test_ts
  test_hd
  test_hd_ktls

  print_summary

  log ""
  log "Cleaning up remote files..."
  rssh "rm -rf $REMOTE_DIR" || true
  cssh "rm -rf $REMOTE_DIR" || true
  log "Done."
}

main
