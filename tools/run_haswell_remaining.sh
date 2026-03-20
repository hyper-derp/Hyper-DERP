#!/bin/bash
# Haswell Remaining Tests — D (TS flame graph) + E (TCP latency).
# Implements the Haswell remaining test plan.
set -uo pipefail

RELAY=${RELAY:?Set RELAY env var}
RELAY_SSH=${RELAY_SSH:?Set RELAY_SSH env var}
RELAY_USER=${RELAY_USER:-worker}
HD_PORT=3341
TS_PORT=3340
SIZE=1400
PEERS=20
PAIRS=10
PERF_DURATION=15

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
PERF_OUT=${PERF_OUT:-${PROJECT_DIR}/bench_results/bare-metal-haswell/perf}
LAT_OUT=${LAT_OUT:-${PROJECT_DIR}/bench_results/bare-metal-haswell/tcp_latency}
mkdir -p "$PERF_OUT" "$LAT_OUT"

log() { echo "[$(date +%H:%M:%S)] $*" >&2; }

rcmd() {
  timeout "${2:-30}" ssh -o StrictHostKeyChecking=no \
    -o ConnectTimeout=10 -o LogLevel=ERROR \
    "${RELAY_USER}@${RELAY_SSH}" "$1" 2>/dev/null || true
}

rout() {
  timeout "${2:-60}" ssh -o StrictHostKeyChecking=no \
    -o ConnectTimeout=10 -o LogLevel=ERROR \
    "${RELAY_USER}@${RELAY_SSH}" "$1" 2>/dev/null
}

rscp() {
  scp -o StrictHostKeyChecking=no -o LogLevel=ERROR \
    "${RELAY_USER}@${RELAY_SSH}:$1" "$2" 2>/dev/null || true
}

wait_port() {
  local host=$1 port=$2 deadline=$((SECONDS + 15))
  while ! timeout 1 bash -c \
      "echo >/dev/tcp/$host/$port" 2>/dev/null; do
    ((SECONDS >= deadline)) && return 1
    sleep 0.5
  done
}

start_ts() {
  log "  Starting TS derper..."
  rcmd "sudo pkill -9 derper 2>/dev/null; sleep 1"
  rcmd "sudo setsid derper \
    -certmode manual -certdir /etc/hyper-derp/certs \
    -a :${TS_PORT} -hostname derp.tailscale.com \
    </dev/null >/tmp/ts.log 2>&1 &"
  sleep 4
  wait_port "$RELAY" "$TS_PORT" || {
    log "  ERROR: TS not listening"; return 1; }
  log "  TS on :${TS_PORT}"
}

stop_ts() {
  rcmd "sudo pkill -9 derper 2>/dev/null"; sleep 2
}

start_hd() {
  local workers=$1 tls=${2:-notls}
  log "  Starting HD ${workers}w (${tls})..."
  rcmd "sudo pkill -9 hyper-derp 2>/dev/null; sleep 1"
  if [ "$tls" = "tls" ]; then
    rcmd "sudo modprobe tls; sudo setsid hyper-derp \
      --port ${HD_PORT} --workers $workers \
      --tls-cert /etc/hyper-derp/certs/derp.tailscale.com.crt \
      --tls-key /etc/hyper-derp/certs/derp.tailscale.com.key \
      </dev/null >/tmp/hd.log 2>&1 &"
  else
    rcmd "sudo setsid hyper-derp \
      --port ${HD_PORT} --workers $workers \
      </dev/null >/tmp/hd.log 2>&1 &"
  fi
  sleep 4
  wait_port "$RELAY" "$HD_PORT" || {
    log "  ERROR: HD not listening"; return 1; }
  log "  HD on :${HD_PORT}"
}

stop_hd() {
  rcmd "sudo pkill -9 hyper-derp 2>/dev/null"; sleep 2
}

drop_caches() {
  rcmd "sync; echo 3 | sudo tee \
    /proc/sys/vm/drop_caches >/dev/null"
  sleep 2
}

run_bench_bg() {
  local port=$1 rate=$2 tls_flag=$3 dur=$4
  local extra=""
  [ "$tls_flag" = "tls" ] && extra="--tls --insecure"
  derp-scale-test \
    --host "$RELAY" --port "$port" \
    --peers "$PEERS" --active-pairs "$PAIRS" \
    --duration "$dur" --msg-size "$SIZE" \
    --rate-mbps "$rate" \
    $extra \
    --json 2>/dev/null >/dev/null &
  echo $!
}

# ---- Test D: TS Flame Graph + perf stat ----------------------

run_test_d() {
  log "=== Test D: TS Flame Graph + perf stat @ 5000 Mbps ==="

  start_ts

  # D1: perf record (LBR)
  local bg
  bg=$(run_bench_bg "$TS_PORT" 5000 tls 25)
  sleep 5

  log "  perf record (LBR)..."
  local pid
  pid=$(rout "pgrep -x derper")
  if [ -n "$pid" ]; then
    rcmd "sudo perf record --call-graph lbr \
      -o /tmp/perf_ts.data \
      -p $pid -- sleep $PERF_DURATION" 60
    rout "sudo perf report -i /tmp/perf_ts.data --stdio \
      --no-children 2>/dev/null" 120 \
      > "$PERF_OUT/ts_5000_report.txt"
    rscp "/tmp/perf_ts.data" "$PERF_OUT/ts_5000.perf.data"
    rcmd "sudo rm -f /tmp/perf_ts.data"
  fi

  kill "$bg" 2>/dev/null; wait "$bg" 2>/dev/null
  sleep 3

  # D2: perf stat pipeline
  bg=$(run_bench_bg "$TS_PORT" 5000 tls 25)
  sleep 5

  log "  Pipeline counters..."
  pid=$(rout "pgrep -x derper")
  [ -n "$pid" ] && rout "sudo perf stat -e \
    cycles,instructions,branches,branch-misses \
    -p $pid -- sleep $PERF_DURATION 2>&1" \
    > "$PERF_OUT/ts_stat_pipeline_5000.txt"

  kill "$bg" 2>/dev/null; wait "$bg" 2>/dev/null
  sleep 3

  # D3: perf stat memory + context
  bg=$(run_bench_bg "$TS_PORT" 5000 tls 25)
  sleep 5

  log "  Memory + context counters..."
  pid=$(rout "pgrep -x derper")
  [ -n "$pid" ] && rout "sudo perf stat -e \
    L1-dcache-loads,L1-dcache-load-misses,\
LLC-loads,LLC-load-misses,\
context-switches,cpu-migrations \
    -p $pid -- sleep $PERF_DURATION 2>&1" \
    > "$PERF_OUT/ts_stat_memory_5000.txt"

  kill "$bg" 2>/dev/null; wait "$bg" 2>/dev/null

  stop_ts
  drop_caches

  # Print summary.
  log "  TS report top 5:"
  head -30 "$PERF_OUT/ts_5000_report.txt" | \
    grep -E "^\s+[0-9]" | head -5
  log "  TS pipeline:"
  cat "$PERF_OUT/ts_stat_pipeline_5000.txt" 2>/dev/null
  log "  TS memory:"
  cat "$PERF_OUT/ts_stat_memory_5000.txt" 2>/dev/null
}

# ---- Test E: Plain TCP Latency Suite -------------------------

run_latency_test() {
  local label=$1 port=$2 tls_flag=$3
  local bg_rate=$4 runs=$5 dest=$6

  local tc_extra=""
  local st_extra=""
  if [ "$tls_flag" = "tls" ]; then
    tc_extra="--tls"
    st_extra="--tls --insecure"
  fi

  for r in $(seq -w 1 "$runs"); do
    log "    ${label} run ${r}/${runs}"
    local outfile="${dest}/${label}_r${r}.json"
    local echo_err="/tmp/echo_${label}_${r}.err"

    # Start echo server.
    nohup derp-test-client \
      --host "$RELAY" --port "$port" --mode echo \
      --count 6000 --timeout 30000 \
      $tc_extra \
      >/dev/null 2>"$echo_err" &
    local echo_pid=$!
    sleep 2

    local echo_key
    echo_key=$(grep '^pubkey:' "$echo_err" | \
      awk '{print $2}' 2>/dev/null)
    if [[ -z "$echo_key" ]]; then
      log "    ERROR: no echo key"
      kill $echo_pid 2>/dev/null || true
      wait $echo_pid 2>/dev/null || true
      continue
    fi

    # Background load.
    local load_pid=""
    if [ "$bg_rate" -gt 0 ]; then
      derp-scale-test \
        --host "$RELAY" --port "$port" \
        --peers "$PEERS" --active-pairs "$PAIRS" \
        --duration 25 --msg-size "$SIZE" \
        --rate-mbps "$bg_rate" \
        $st_extra \
        --json 2>/dev/null >/dev/null &
      load_pid=$!
      sleep 2
    fi

    # Pinger.
    derp-test-client \
      --host "$RELAY" --port "$port" --mode ping \
      --count 5000 --size "$SIZE" \
      --dst-key "$echo_key" --warmup 500 \
      --json --raw-latency \
      --label "$label" --output "$outfile" \
      --timeout 30000 \
      $tc_extra 2>/dev/null || true

    # Cleanup.
    if [ -n "$load_pid" ]; then
      kill $load_pid 2>/dev/null || true
      wait $load_pid 2>/dev/null || true
    fi
    kill $echo_pid 2>/dev/null || true
    wait $echo_pid 2>/dev/null || true
    sleep 1
  done
}

run_test_e() {
  local workers=$1
  local config="hd_${workers}w"

  log "=== Test E: Plain TCP latency ${workers}w ==="

  start_hd "$workers" notls

  for rate in 0 1000 3000 5000 7500; do
    local level_name
    if [ "$rate" -eq 0 ]; then
      level_name="idle"
    else
      level_name="${rate}"
    fi

    log "  Load level: ${level_name} (bg=${rate} Mbps)"
    run_latency_test \
      "${config}_tcp_lat_${level_name}" "$HD_PORT" "notls" \
      "$rate" 10 "$LAT_OUT"
  done

  stop_hd
  drop_caches
}

# ---- Main ----------------------------------------------------

main() {
  log "Haswell Remaining Tests"
  log ""

  # Test D: TS flame graph + perf stat.
  run_test_d

  # Test E: Plain TCP latency.
  run_test_e 2
  run_test_e 4

  # Test F: analysis (print summary).
  log ""
  log "=== Test F: Latency Analysis ==="
  python3 -c "
import json, glob, statistics

def analyze(base, pattern):
    files = sorted(glob.glob(f'{base}/{pattern}'))
    if not files:
        return None
    p50s, p99s, p999s, maxs = [], [], [], []
    for f in files:
        with open(f) as fh:
            d = json.load(fh)
        ln = d.get('latency_ns', {})
        p50s.append(ln.get('p50', 0) / 1000)
        p99s.append(ln.get('p99', 0) / 1000)
        p999s.append(ln.get('p999', 0) / 1000)
        maxs.append(ln.get('max', 0) / 1000)
    return {
        'n': len(files),
        'p50': statistics.mean(p50s),
        'p99': statistics.mean(p99s),
        'p999': statistics.mean(p999s),
        'max': statistics.mean(maxs),
    }

print('TCP vs kTLS latency comparison (us):')
print(f'{\"Config\":<30s} {\"p50\":>8s} {\"p99\":>8s} {\"p999\":>8s} {\"max\":>8s}  n')
print('-' * 80)

bm = 'bench_results/bare-metal-haswell'
for config, base, pattern_fmt in [
    ('kTLS 2w', f'{bm}/2w_ktls/latency', 'hd_{level}_r*.json'),
    ('TCP 2w', f'{bm}/tcp_latency', 'hd_2w_tcp_lat_{level}_r*.json'),
    ('kTLS 4w', f'{bm}/4w_ktls/latency', 'hd_{level}_r*.json'),
    ('TCP 4w', f'{bm}/tcp_latency', 'hd_4w_tcp_lat_{level}_r*.json'),
]:
    for level in ['idle', 'ceil25', 'ceil50', 'ceil75', 'ceil100', 'ceil150',
                  '1000', '3000', '5000', '7500']:
        pat = pattern_fmt.replace('{level}', level)
        r = analyze(base, pat)
        if r and r['n'] > 0 and r['p50'] > 0:
            label = f'{config} @ {level}'
            print(f'{label:<30s} {r[\"p50\"]:>8.0f} {r[\"p99\"]:>8.0f} {r[\"p999\"]:>8.0f} {r[\"max\"]:>8.0f}  {r[\"n\"]}')
" 2>/dev/null

  log ""
  log "=============================="
  log "  Remaining tests complete"
  log "=============================="
}

main
