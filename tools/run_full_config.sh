#!/bin/bash
# Full sweep benchmark — single vCPU configuration.
# Implements the protocol from test-design/FULL_SWEEP_DESIGN.md.
# Runs on the bench-client VM.
#
# Usage:
#   run_full_config.sh <vcpus> [workers] [label]
#
# Example:
#   run_full_config.sh 8 4 8vcpu
#   run_full_config.sh 2 1 2vcpu_1w
#   run_full_config.sh 2 2 2vcpu_2w
set -uo pipefail

# ---- Arguments -----------------------------------------------

VCPUS=${1:?usage: run_full_config.sh <vcpus> [workers] [label]}
WORKERS=${2:-$((VCPUS / 2))}
[ "$WORKERS" -lt 1 ] && WORKERS=1
LABEL=${3:-"${VCPUS}vcpu"}

# ---- Network / relay -----------------------------------------

RELAY=${RELAY:-10.10.0.2}
RELAY_USER=${RELAY_USER:-worker}
RELAY_KEY=${RELAY_KEY:-$HOME/.ssh/id_ed25519_targets}
HD_PORT=3341
TS_PORT=3340
HD_METRICS_PORT=9090
SIZE=1400

# ---- Rate table per config -----------------------------------

case $VCPUS in
  2)  ALL_RATES="500 1000 2000 3000 5000 7500 10000"
      TS_CEIL_EST=1500 ;;
  4)  ALL_RATES="500 1000 2000 3000 5000 7500 10000 15000"
      TS_CEIL_EST=2500 ;;
  8)  ALL_RATES="500 1000 2000 3000 5000 7500 10000 15000 20000"
      TS_CEIL_EST=5000 ;;
  16) ALL_RATES="500 1000 2000 3000 5000 7500 10000 15000 20000 25000"
      TS_CEIL_EST=8000 ;;
  *)  echo "unsupported vcpu count: $VCPUS"; exit 1 ;;
esac

LOW_THRESH=3000
LOW_RUNS=5
HIGH_RUNS=25

# ---- Rate sweep params ---------------------------------------

RATE_PEERS=20
RATE_PAIRS=10
RATE_DURATION=15

# ---- Latency params ------------------------------------------

PING_COUNT=5000
PING_WARMUP=500
LAT_RUNS_LOW=10
LAT_RUNS_HIGH=15

# ---- Output directory ----------------------------------------

OUT=/tmp/bench_${LABEL}
rm -rf "$OUT"
mkdir -p "$OUT"/{rate,latency,cpu,gc_trace}

# ---- Helpers -------------------------------------------------

log() { echo "[$(date +%H:%M:%S)] $*"; }

# Fire-and-forget relay command (never fails the script).
relay_cmd() {
  timeout 30 ssh -i "$RELAY_KEY" \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    -o ConnectTimeout=10 \
    -o LogLevel=ERROR \
    "${RELAY_USER}@${RELAY}" "$1" 2>/dev/null || true
}

# Relay command that captures stdout.
relay_out() {
  timeout 30 ssh -i "$RELAY_KEY" \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    -o ConnectTimeout=10 \
    -o LogLevel=ERROR \
    "${RELAY_USER}@${RELAY}" "$1" 2>/dev/null
}

relay_scp() {
  scp -i "$RELAY_KEY" \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    -o LogLevel=ERROR \
    "${RELAY_USER}@${RELAY}:$1" "$2" 2>/dev/null || true
}

# ---- Server management --------------------------------------

start_hd() {
  log "  [relay] starting HD (workers=$WORKERS)..."
  relay_cmd "sudo pkill -9 hyper-derp 2>/dev/null; sleep 1"
  relay_cmd "setsid hyper-derp --port ${HD_PORT} \
    --workers $WORKERS \
    --metrics-port $HD_METRICS_PORT \
    --debug-endpoints \
    </dev/null >/tmp/hd.log 2>&1 &"
  sleep 4
  local ok
  ok=$(relay_out "ss -tlnp | grep -c ${HD_PORT}" || echo 0)
  if [ "${ok:-0}" -lt 1 ]; then
    log "  WARNING: HD not listening, retrying..."
    relay_cmd "setsid hyper-derp --port ${HD_PORT} \
      --workers $WORKERS \
      --metrics-port $HD_METRICS_PORT \
      --debug-endpoints \
      </dev/null >/tmp/hd.log 2>&1 &"
    sleep 4
  fi
}

stop_hd() {
  relay_cmd "sudo pkill -9 hyper-derp 2>/dev/null"
  sleep 2
}

start_ts() {
  log "  [relay] starting TS derper..."
  relay_cmd "sudo pkill -9 derper 2>/dev/null; sleep 1"
  relay_cmd "setsid derper -dev -a :${TS_PORT} \
    </dev/null >/tmp/ts.log 2>&1 &"
  sleep 4
  local ok
  ok=$(relay_out "ss -tlnp | grep -c ${TS_PORT}" || echo 0)
  if [ "${ok:-0}" -lt 1 ]; then
    log "  WARNING: TS not listening, retrying..."
    relay_cmd "setsid derper -dev -a :${TS_PORT} \
      </dev/null >/tmp/ts.log 2>&1 &"
    sleep 4
  fi
}

stop_ts() {
  relay_cmd "sudo pkill -9 derper 2>/dev/null"
  sleep 2
}

drop_caches() {
  relay_cmd "sync; echo 3 | sudo tee /proc/sys/vm/drop_caches \
    >/dev/null"
  sleep 2
}

# ---- CPU monitoring ------------------------------------------

# Start phase-level monitoring on relay. Runs until stopped.
# Phase-level (not per-run) to avoid 500+ SSH round trips.
# Per-run correlation: match timestamps in CPU data against
# timestamps in the rate/latency JSON output.
start_monitor() {
  local server_bin=$1  # hyper-derp or derper
  local phase_label=$2
  relay_cmd "mkdir -p /tmp/cpu"
  # pidstat for the server process (1s interval).
  relay_cmd "nohup bash -c 'PID=\$(pgrep -x $server_bin); \
    [ -n \"\$PID\" ] && pidstat -p \$PID -r -u 1 \
    > /tmp/cpu/${phase_label}_pidstat.txt 2>&1' &"
  # mpstat system-wide (1s interval).
  relay_cmd "nohup mpstat -P ALL 1 \
    > /tmp/cpu/${phase_label}_mpstat.txt 2>&1 &"
  # RSS sampling (1s interval).
  relay_cmd "nohup bash -c 'PID=\$(pgrep -x $server_bin); \
    while kill -0 \$PID 2>/dev/null; do \
      echo \"\$(date +%s) \$(ps -o rss= -p \$PID)\"; \
      sleep 1; \
    done > /tmp/cpu/${phase_label}_rss.txt 2>&1' &"
}

stop_monitor() {
  relay_cmd "pkill -f pidstat 2>/dev/null; \
    pkill -f mpstat 2>/dev/null"
  sleep 1
}

download_cpu() {
  local phase_label=$1
  for suffix in pidstat mpstat rss; do
    relay_scp "/tmp/cpu/${phase_label}_${suffix}.txt" \
      "$OUT/cpu/"
  done
}

# ---- HD worker stats -----------------------------------------

hd_worker_stats() {
  local label=$1
  relay_out "curl -s http://localhost:${HD_METRICS_PORT}\
/debug/workers" > "$OUT/cpu/${label}_workers.json" || true
}

# ---- Rate sweep ----------------------------------------------

run_rate() {
  local label=$1 port=$2 rate=$3
  local outfile="$OUT/rate/${label}.json"
  local tmpfile="${outfile}.tmp"

  derp-scale-test \
    --host "$RELAY" --port "$port" \
    --peers "$RATE_PEERS" --active-pairs "$RATE_PAIRS" \
    --duration "$RATE_DURATION" --msg-size "$SIZE" \
    --rate-mbps "$rate" \
    --json 2>/dev/null > "$tmpfile" || true

  # Extract JSON object (skip any text before/after).
  sed -n '/{/,/}/p' "$tmpfile" > "$outfile"
  rm -f "$tmpfile"

  python3 -c "
import json
try:
  d = json.load(open('$outfile'))
  print(f'  {d[\"throughput_mbps\"]:.0f} Mbps, '
        f'{d[\"message_loss_pct\"]:.1f}% loss')
except: print('  FAILED')
" 2>/dev/null
  sleep 2
}

# Run the full rate sweep for one server.
# CPU: phase-level pidstat/mpstat/rss run continuously
# (started by caller). Per-run correlation uses timestamps
# in the JSON output. HD worker stats captured per-rate.
rate_sweep() {
  local server=$1  # hd or ts
  local port=$2
  for rate in $ALL_RATES; do
    local runs=$HIGH_RUNS
    if [ "$rate" -le "$LOW_THRESH" ]; then
      runs=$LOW_RUNS
    fi
    # HD worker stats before this rate.
    if [ "$server" = "hd" ]; then
      hd_worker_stats "hd_rate_${rate}_before"
    fi
    for run in $(seq 1 "$runs"); do
      local padded
      padded=$(printf '%02d' "$run")
      local label="${server}_${rate}_r${padded}"
      log "[$run/$runs] ${server^^} ${rate}:"
      run_rate "$label" "$port" "$rate"
    done
    # HD worker stats after this rate.
    if [ "$server" = "hd" ]; then
      hd_worker_stats "hd_rate_${rate}_after"
    fi
  done
}

# ---- TS ceiling computation ---------------------------------

# Find first rate where median loss > 5%.
compute_ts_ceiling() {
  local data_dir=$1  # directory with rate JSON files
  python3 -c "
import json, glob, statistics
from pathlib import Path

rates = [500,1000,2000,3000,5000,7500,10000,15000,20000,25000]
d = Path('$data_dir')
ceiling = rates[-1]
for rate in rates:
    files = sorted(d.glob(f'ts_{rate}_r*.json'))
    if not files:
        continue
    losses = []
    for f in files:
        try:
            data = json.loads(f.read_text())
            losses.append(data['message_loss_pct'])
        except:
            pass
    if losses and statistics.median(losses) > 5.0:
        # Ceiling is the rate BEFORE this one.
        idx = rates.index(rate)
        ceiling = rates[idx - 1] if idx > 0 else rates[0]
        break
print(ceiling)
" 2>/dev/null
}

# ---- Latency loads from ceiling ------------------------------

# Compute 6 load levels as percentage of TS ceiling.
# Rounds to nearest 100 Mbps.
compute_lat_loads() {
  local ceil=$1
  python3 -c "
ceil = $ceil
pcts = [0, 25, 50, 75, 100, 150]
labels = ['idle','ceil25','ceil50','ceil75','ceil100','ceil150']
for pct, label in zip(pcts, labels):
    rate = int(round(ceil * pct / 100 / 100) * 100) \
        if pct > 0 else 0
    print(f'{label}:{rate}')
" 2>/dev/null
}

# ---- Latency measurement ------------------------------------

run_latency() {
  local label=$1 port=$2 bg_rate=$3 bg_pairs=$4
  local bg_peers=$((bg_pairs * 2))
  local echo_err="/tmp/${label}_echo.err"

  rm -f "$echo_err"
  derp-test-client \
    --host "$RELAY" --port "$port" --mode echo \
    --count $((PING_COUNT + PING_WARMUP + 1000)) \
    --timeout 120000 \
    >/dev/null 2>"$echo_err" &
  local echo_pid=$!
  sleep 3

  local echo_key
  echo_key=$(grep '^pubkey:' "$echo_err" \
    | awk '{print $2}')
  if [[ -z "$echo_key" ]]; then
    log "    ERROR: no echo key for $label"
    kill "$echo_pid" 2>/dev/null || true
    return 1
  fi

  local bg_pid=""
  if [[ "$bg_rate" -gt 0 && "$bg_pairs" -gt 0 ]]; then
    derp-scale-test \
      --host "$RELAY" --port "$port" \
      --peers "$bg_peers" --active-pairs "$bg_pairs" \
      --duration 120 --msg-size "$SIZE" \
      --rate-mbps "$bg_rate" \
      >/dev/null 2>/dev/null &
    bg_pid=$!
    sleep 5
  fi

  derp-test-client \
    --host "$RELAY" --port "$port" --mode ping \
    --count "$PING_COUNT" --size "$SIZE" \
    --dst-key "$echo_key" --warmup "$PING_WARMUP" \
    --json --raw-latency \
    --label "$label" \
    --output "$OUT/latency/${label}.json" \
    --timeout 120000 2>/dev/null || true

  if [[ -n "$bg_pid" ]]; then
    kill "$bg_pid" 2>/dev/null || true
    wait "$bg_pid" 2>/dev/null || true
  fi
  kill "$echo_pid" 2>/dev/null || true
  wait "$echo_pid" 2>/dev/null || true

  python3 -c "
import json
try:
  d = json.load(open('$OUT/latency/${label}.json'))
  ln = d['latency_ns']
  print(f'  p50={ln[\"p50\"]/1000:.0f}us '
        f'p99={ln[\"p99\"]/1000:.0f}us '
        f'p999={ln[\"p999\"]/1000:.0f}us '
        f'max={ln[\"max\"]/1000:.0f}us')
except Exception as e: print(f'  FAILED: {e}')
" 2>/dev/null
  rm -f "$echo_err"
  sleep 2
}

# Run the full latency suite for one server.
# HD worker stats captured per load level (before+after).
latency_suite() {
  local server=$1  # hd or ts
  local port=$2
  local loads=$3   # space-separated "label:rate" pairs

  for entry in $loads; do
    local load_label="${entry%%:*}"
    local bg_rate="${entry##*:}"
    local runs=$LAT_RUNS_LOW
    if [[ "$load_label" == "ceil100" ]] || \
       [[ "$load_label" == "ceil150" ]]; then
      runs=$LAT_RUNS_HIGH
    fi
    local bg_pairs=10
    if [[ "$bg_rate" -eq 0 ]]; then
      bg_pairs=0
    fi
    # HD worker stats before this load level.
    if [ "$server" = "hd" ]; then
      hd_worker_stats "hd_lat_${load_label}_before"
    fi
    for run in $(seq 1 "$runs"); do
      local padded
      padded=$(printf '%02d' "$run")
      local label="${server}_${load_label}_r${padded}"
      log "[$run/$runs] ${server^^} ${load_label} \
(bg=${bg_rate}M):"
      run_latency "$label" "$port" "$bg_rate" "$bg_pairs"
    done
    # HD worker stats after this load level.
    if [ "$server" = "hd" ]; then
      hd_worker_stats "hd_lat_${load_label}_after"
    fi
  done
}

# ---- GC analysis (TS only) ----------------------------------

run_gc_trace() {
  local ceil=$1
  log "=== GC Trace: TS at ceiling (${ceil}M) ==="
  relay_cmd "sudo pkill -9 derper 2>/dev/null; sleep 1"
  relay_cmd "setsid bash -c 'GODEBUG=gctrace=1 \
    derper -dev -a :${TS_PORT} \
    </dev/null >/tmp/ts_gctrace_stderr.log 2>&1' &"
  sleep 4

  derp-scale-test \
    --host "$RELAY" --port "$TS_PORT" \
    --peers "$RATE_PEERS" --active-pairs "$RATE_PAIRS" \
    --duration 30 --msg-size "$SIZE" \
    --rate-mbps "$ceil" \
    --json 2>/dev/null \
    > "$OUT/gc_trace/ts_gctrace_rate.json" || true

  relay_cmd "sudo pkill -9 derper 2>/dev/null"
  sleep 2
  relay_scp "/tmp/ts_gctrace_stderr.log" \
    "$OUT/gc_trace/ts_gctrace.log"
  log "  GC trace collected."
}

run_gogc_off() {
  local ceil=$1
  log "=== GOGC=off: TS at ceiling (${ceil}M) ==="
  relay_cmd "sudo pkill -9 derper 2>/dev/null; sleep 1"
  relay_cmd "setsid bash -c 'GOGC=off \
    derper -dev -a :${TS_PORT} \
    </dev/null >/tmp/ts.log 2>&1' &"
  sleep 4

  derp-scale-test \
    --host "$RELAY" --port "$TS_PORT" \
    --peers "$RATE_PEERS" --active-pairs "$RATE_PAIRS" \
    --duration 30 --msg-size "$SIZE" \
    --rate-mbps "$ceil" \
    --json 2>/dev/null \
    > "$OUT/gc_trace/ts_gogcoff.json" || true

  relay_cmd "sudo pkill -9 derper 2>/dev/null"
  sleep 2
  log "  GOGC=off result collected."
}

# ---- System info ---------------------------------------------

collect_system_info() {
  log "Collecting system info..."
  {
    echo "date: $(date -Iseconds)"
    echo "label: $LABEL"
    echo "relay: $RELAY"
    echo "relay_vcpus: $VCPUS"
    echo "relay_workers: $WORKERS"
    echo "client_kernel: $(uname -r)"
    echo "client_cpu: $(lscpu | grep 'Model name' \
      | sed 's/.*: *//')"
    echo "msg_size: $SIZE"
    echo "rate_peers: $RATE_PEERS"
    echo "rate_pairs: $RATE_PAIRS"
    echo "rate_duration: $RATE_DURATION"
    echo "low_rates_runs: $LOW_RUNS"
    echo "high_rates_runs: $HIGH_RUNS"
    echo "ping_count: $PING_COUNT"
    echo "ping_warmup: $PING_WARMUP"
    echo "lat_runs_low: $LAT_RUNS_LOW"
    echo "lat_runs_high: $LAT_RUNS_HIGH"
    echo "ts_ceiling_est: $TS_CEIL_EST"
    echo "all_rates: $ALL_RATES"
  } > "$OUT/system_info.txt"

  # Relay system info.
  relay_out "uname -a" \
    >> "$OUT/system_info.txt" 2>/dev/null
  relay_out "lscpu" \
    > "$OUT/relay_lscpu.txt" 2>/dev/null
  relay_out "cat /proc/version" \
    > "$OUT/relay_version.txt" 2>/dev/null
  relay_out "sysctl net.core.rmem_max \
    net.core.wmem_max \
    net.core.somaxconn \
    net.ipv4.tcp_wmem \
    net.ipv4.tcp_rmem 2>/dev/null" \
    > "$OUT/relay_sysctl.txt" 2>/dev/null

  # Binary versions.
  relay_out "hyper-derp --version 2>&1 || true" \
    > "$OUT/hd_version.txt" 2>/dev/null
  relay_out "go version -m \$(which derper) 2>&1 || true" \
    > "$OUT/ts_version.txt" 2>/dev/null
}

# ---- Validation ----------------------------------------------

validate_results() {
  log "=== Validation ==="
  python3 -c "
import json, glob, statistics
from pathlib import Path

out = Path('$OUT')
errors = []

# Count rate files.
rate_files = list((out / 'rate').glob('*.json'))
valid_rate = [f for f in rate_files
              if f.stat().st_size > 10]
total_rate = len(rate_files)

# Expected rate count.
low_rates = [r for r in [$( echo "$ALL_RATES" \
  | tr ' ' ',' )] if r <= $LOW_THRESH]
high_rates = [r for r in [$( echo "$ALL_RATES" \
  | tr ' ' ',' )] if r > $LOW_THRESH]
expected_rate = (len(low_rates) * $LOW_RUNS + \
    len(high_rates) * $HIGH_RUNS) * 2
print(f'Rate files: {len(valid_rate)}/{expected_rate} valid')
if len(valid_rate) < expected_rate:
    errors.append(
        f'Missing rate files: {expected_rate - len(valid_rate)}')

# Count latency files.
lat_files = list((out / 'latency').glob('*.json'))
valid_lat = [f for f in lat_files
             if f.stat().st_size > 10]
print(f'Latency files: {len(valid_lat)} valid')

# Sanity: low-rate throughput near offered.
for server in ['hd', 'ts']:
    for rate in [500, 1000]:
        files = sorted(
            (out / 'rate').glob(f'{server}_{rate}_r*.json'))
        if not files:
            continue
        tps = []
        for f in files:
            try:
                d = json.loads(f.read_text())
                tps.append(d['throughput_mbps'])
            except:
                pass
        if tps:
            med = statistics.median(tps)
            expected = rate * 0.87  # ~87% for 1400B frames
            if abs(med - expected) > expected * 0.1:
                errors.append(
                    f'{server} @{rate}: median {med:.0f} '
                    f'far from expected {expected:.0f}')

# Check for empty/truncated files.
for f in rate_files + lat_files:
    if f.stat().st_size < 10:
        errors.append(f'Empty/truncated: {f.name}')

# CV check at high rates.
for server in ['hd', 'ts']:
    for rate in high_rates:
        files = sorted(
            (out / 'rate').glob(f'{server}_{rate}_r*.json'))
        tps = []
        for f in files:
            try:
                d = json.loads(f.read_text())
                tps.append(d['throughput_mbps'])
            except:
                pass
        if len(tps) > 2:
            mean = statistics.mean(tps)
            if mean > 0:
                cv = statistics.stdev(tps) / mean * 100
                if cv > 15:
                    errors.append(
                        f'{server} @{rate}: CV={cv:.1f}%')

# Outlier stall check in latency.
for f in valid_lat:
    try:
        d = json.loads(f.read_text())
        ln = d['latency_ns']
        if ln['max'] > 10 * ln['p999']:
            errors.append(
                f'Outlier stall: {f.name} '
                f'max={ln[\"max\"]/1e6:.1f}ms '
                f'vs p999={ln[\"p999\"]/1e6:.1f}ms')
    except:
        pass

if errors:
    print(f'\\n{len(errors)} issues:')
    for e in errors:
        print(f'  - {e}')
else:
    print('All checks passed.')
" 2>/dev/null
}

# ==============================================================
# MAIN
# ==============================================================

log "========================================="
log "  FULL SWEEP: ${VCPUS} vCPU, ${WORKERS} w"
log "  Label: ${LABEL}"
log "========================================="
log ""

collect_system_info

# Ensure sysstat is installed on relay.
relay_cmd "which pidstat >/dev/null 2>&1 || \
  sudo apt-get install -y -qq sysstat >/dev/null 2>&1"

# Clean relay monitoring state.
relay_cmd "sudo rm -rf /tmp/cpu; mkdir -p /tmp/cpu"

# ---- Phase 0: TS ceiling probe ------------------------------

log "=== Phase 0: TS Ceiling Probe ==="
stop_hd
start_ts

PROBE_RATES="1000 3000 5000 7500 10000"
PROBE_DIR="$OUT/probe"
mkdir -p "$PROBE_DIR"

for rate in $PROBE_RATES; do
  for run in 1 2 3; do
    log "  probe TS @${rate} [$run/3]:"
    derp-scale-test \
      --host "$RELAY" --port "$TS_PORT" \
      --peers "$RATE_PEERS" --active-pairs "$RATE_PAIRS" \
      --duration 10 --msg-size "$SIZE" \
      --rate-mbps "$rate" \
      --json 2>/dev/null \
      | sed -n '/{/,/}/p' \
      > "$PROBE_DIR/ts_${rate}_r${run}.json" || true
    python3 -c "
import json
try:
  d = json.load(open('$PROBE_DIR/ts_${rate}_r${run}.json'))
  print(f'    {d[\"throughput_mbps\"]:.0f} Mbps, '
        f'{d[\"message_loss_pct\"]:.1f}% loss')
except: print('    FAILED')
" 2>/dev/null
    sleep 1
  done
done

stop_ts

# Compute actual TS ceiling from probe data.
TS_CEILING=$(compute_ts_ceiling "$PROBE_DIR")
TS_CEILING=${TS_CEILING:-$TS_CEIL_EST}
log "  TS ceiling (probe): ${TS_CEILING}M \
(estimate was ${TS_CEIL_EST}M)"
echo "ts_ceiling_actual: $TS_CEILING" \
  >> "$OUT/system_info.txt"

# Compute latency load levels.
LAT_LOADS=$(compute_lat_loads "$TS_CEILING")
log "  Latency loads: $LAT_LOADS"
echo "latency_loads: $LAT_LOADS" >> "$OUT/system_info.txt"

# ---- Phase 1: HD full run -----------------------------------

log ""
log "=== Phase 1: HD Rate Sweep ==="
drop_caches
start_hd
hd_worker_stats "hd_before"
start_monitor "hyper-derp" "hd_rate"

rate_sweep "hd" "$HD_PORT"

stop_monitor
download_cpu "hd_rate"
hd_worker_stats "hd_after_rate"

log ""
log "=== Phase 1: HD Latency ==="
start_monitor "hyper-derp" "hd_lat"

latency_suite "hd" "$HD_PORT" "$LAT_LOADS"

stop_monitor
download_cpu "hd_lat"
hd_worker_stats "hd_after_lat"
stop_hd

# ---- Phase 2: TS full run -----------------------------------

log ""
log "=== Phase 2: TS Rate Sweep ==="
drop_caches
start_ts
start_monitor "derper" "ts_rate"

rate_sweep "ts" "$TS_PORT"

stop_monitor
download_cpu "ts_rate"

# Compute actual TS ceiling from full rate sweep data.
TS_CEIL_ACTUAL=$(compute_ts_ceiling "$OUT/rate")
TS_CEIL_ACTUAL=${TS_CEIL_ACTUAL:-$TS_CEILING}
log "  TS ceiling (full sweep): ${TS_CEIL_ACTUAL}M \
(probe was ${TS_CEILING}M)"
echo "ts_ceiling_full: $TS_CEIL_ACTUAL" \
  >> "$OUT/system_info.txt"

# Recompute latency loads if actual ceiling differs by >20%.
DIFF=$(python3 -c "
a, p = $TS_CEIL_ACTUAL, $TS_CEILING
print(abs(a - p) / max(p, 1) * 100)
" 2>/dev/null)
DIFF=${DIFF:-0}
DIFF_INT=${DIFF%%.*}
if [ "${DIFF_INT:-0}" -gt 20 ]; then
  log "  WARNING: ceiling shift >${DIFF_INT}%, \
recomputing loads"
  LAT_LOADS=$(compute_lat_loads "$TS_CEIL_ACTUAL")
  echo "latency_loads_adjusted: $LAT_LOADS" \
    >> "$OUT/system_info.txt"
fi

log ""
log "=== Phase 2: TS Latency ==="
start_monitor "derper" "ts_lat"

latency_suite "ts" "$TS_PORT" "$LAT_LOADS"

stop_monitor
download_cpu "ts_lat"
stop_ts

# ---- Phase 3: GC analysis -----------------------------------

log ""
log "=== Phase 3: GC Analysis ==="
run_gc_trace "$TS_CEIL_ACTUAL"
run_gogc_off "$TS_CEIL_ACTUAL"

# ---- Phase 4: Validation ------------------------------------

log ""
validate_results

# ---- Done ----------------------------------------------------

rate_count=$(find "$OUT/rate" -name "*.json" \
  -size +0c | wc -l)
lat_count=$(find "$OUT/latency" -name "*.json" \
  -size +0c | wc -l)
log ""
log "=== ${LABEL} complete ==="
log "Rate:    $rate_count valid"
log "Latency: $lat_count valid"
log "Output:  $OUT"
echo "complete $(date -Iseconds)" > "$OUT/DONE"
