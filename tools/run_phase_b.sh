#!/bin/bash
# Phase B — kTLS Full Sweep.
# HD kTLS vs TS TLS across 2/4/8/16 vCPU configs.
# Runs on bench-client VM, controls bench-relay via SSH.
# Implements test-design/PHASE_B_PLAN.md.
#
# Usage:
#   run_phase_b.sh [--start-at <vcpu>] [--skip-ts]
#                   [--skip-latency] [--dry-run]
#
# The --start-at flag resumes from a specific vCPU config
# (e.g. --start-at 4 skips 16 and 8).
set -uo pipefail

# ---- Flags ---------------------------------------------------

START_AT=16
ONLY=0
SKIP_TS=0
SKIP_LATENCY=0
LATENCY_ONLY=0
DRY_RUN=0

while [[ $# -gt 0 ]]; do
  case $1 in
    --start-at)      START_AT=$2; shift 2 ;;
    --only)          ONLY=$2; START_AT=$2; shift 2 ;;
    --skip-ts)       SKIP_TS=1; shift ;;
    --skip-latency)  SKIP_LATENCY=1; shift ;;
    --latency-only)  LATENCY_ONLY=1; shift ;;
    --dry-run)       DRY_RUN=1; shift ;;
    *)               echo "unknown flag: $1"; exit 1 ;;
  esac
done

# ---- Network / relay -----------------------------------------

RELAY=${RELAY:?Set RELAY env var}
RELAY_USER=${RELAY_USER:-worker}
RELAY_KEY=${RELAY_KEY:?Set RELAY_KEY env var}
HD_PORT=3341
TS_PORT=3340
HD_METRICS_PORT=9090
SIZE=1400

# ---- Test params ---------------------------------------------

PEERS=20
PAIRS=10
DURATION=15
RUNS_LOW=5
RUNS_HIGH=25

# Latency params.
PING_COUNT=5000
PING_WARMUP=500

# ---- vCPU configs -------------------------------------------
# Each line: vcpu workers
CONFIGS=(
  "16 8"
  "8 4"
  "4 2"
  "2 1"
)

# Estimated probe rates per vCPU config.
# Final rates adjusted after probes.
declare -A PROBE_RATES
PROBE_RATES[16]="3000 5000 7000 7500 10000"
PROBE_RATES[8]="2000 3000 4000 5000 7500"
PROBE_RATES[4]="1000 2000 2500 3000 5000"
PROBE_RATES[2]="500 1000 1500 2000 3000"

# Rate sweep per vCPU config (adjusted after probes).
declare -A SWEEP_RATES
SWEEP_RATES[16]="500 1000 2000 3000 5000 7500 10000 15000 20000"
SWEEP_RATES[8]="500 1000 2000 3000 5000 7500 10000 15000"
SWEEP_RATES[4]="500 1000 2000 3000 5000 7500 10000"
SWEEP_RATES[2]="500 1000 1500 2000 3000 5000 7500"

PROBE_RUNS=3

# ---- Output --------------------------------------------------

OUT_BASE=/tmp/bench_phase_b
mkdir -p "$OUT_BASE"

# ---- Helpers -------------------------------------------------

log() { echo "[$(date +%H:%M:%S)] $*" >&2; }

relay_cmd() {
  timeout 30 ssh -i "$RELAY_KEY" \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    -o ConnectTimeout=10 \
    -o LogLevel=ERROR \
    "${RELAY_USER}@${RELAY}" "$1" 2>/dev/null || true
}

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

wait_port() {
  local host=$1 port=$2 timeout=${3:-10}
  local deadline=$((SECONDS + timeout))
  while ! timeout 1 bash -c \
      "echo >/dev/tcp/$host/$port" 2>/dev/null; do
    if ((SECONDS >= deadline)); then
      log "ERROR: $host:$port not ready after ${timeout}s"
      return 1
    fi
    sleep 0.5
  done
}

# ---- Server management --------------------------------------

start_hd() {
  local workers=$1
  local cert_dir=$2
  log "  [relay] starting HD kTLS (workers=$workers)..."
  relay_cmd "sudo pkill -9 hyper-derp 2>/dev/null; sleep 1"
  relay_cmd "sudo setsid hyper-derp --port ${HD_PORT} \
    --workers $workers \
    --metrics-port $HD_METRICS_PORT \
    --debug-endpoints \
    --tls-cert ${cert_dir}/cert.pem \
    --tls-key ${cert_dir}/key.pem \
    </dev/null >/tmp/hd.log 2>&1 &"
  sleep 4
  local ok
  ok=$(relay_out "ss -tlnp | grep -c ${HD_PORT}" || echo 0)
  if [ "${ok:-0}" -lt 1 ]; then
    log "  WARNING: HD not listening, retrying..."
    relay_cmd "sudo setsid hyper-derp --port ${HD_PORT} \
      --workers $workers \
      --metrics-port $HD_METRICS_PORT \
      --debug-endpoints \
      --tls-cert ${cert_dir}/cert.pem \
      --tls-key ${cert_dir}/key.pem \
      </dev/null >/tmp/hd.log 2>&1 &"
    sleep 4
  fi
  # Verify kTLS activation.
  local ktls_ok
  ktls_ok=$(relay_out \
    "grep -c BIO_get_ktls /tmp/hd.log" || echo 0)
  if [ "${ktls_ok:-0}" -lt 1 ]; then
    log "  WARNING: kTLS may not be active (check log)"
  else
    log "  kTLS confirmed active"
  fi
}

stop_hd() {
  relay_cmd "sudo pkill -9 hyper-derp 2>/dev/null"
  sleep 2
}

start_ts() {
  local cert_dir=${1:-/tmp/bench_certs}
  log "  [relay] starting TS derper (TLS, -certmode manual)..."
  relay_cmd "sudo pkill -9 derper 2>/dev/null; sleep 1"
  relay_cmd "sudo setsid derper \
    -certmode manual -certdir ${cert_dir} \
    -a :${TS_PORT} -hostname derp.tailscale.com \
    </dev/null >/tmp/ts.log 2>&1 &"
  sleep 4
  local ok
  ok=$(relay_out "ss -tlnp | grep -c ${TS_PORT}" || echo 0)
  if [ "${ok:-0}" -lt 1 ]; then
    log "  WARNING: TS not listening, retrying..."
    relay_cmd "sudo setsid derper \
      -certmode manual -certdir ${cert_dir} \
      -a :${TS_PORT} -hostname derp.tailscale.com \
      </dev/null >/tmp/ts.log 2>&1 &"
    sleep 4
  fi
}

stop_ts() {
  relay_cmd "sudo pkill -9 derper 2>/dev/null"
  sleep 2
}

drop_caches() {
  relay_cmd "sync; echo 3 | sudo tee \
    /proc/sys/vm/drop_caches >/dev/null"
  sleep 2
}

# ---- CPU monitoring ------------------------------------------

start_monitor() {
  local server_bin=$1
  local label=$2
  relay_cmd "mkdir -p /tmp/cpu"
  relay_cmd "nohup bash -c 'PID=\$(pgrep -x $server_bin); \
    [ -n \"\$PID\" ] && pidstat -p \$PID -r -u 1 \
    > /tmp/cpu/${label}_pidstat.txt 2>&1' &"
  relay_cmd "nohup mpstat -P ALL 1 \
    > /tmp/cpu/${label}_mpstat.txt 2>&1 &"
  relay_cmd "nohup bash -c 'PID=\$(pgrep -x $server_bin); \
    while kill -0 \$PID 2>/dev/null; do \
      echo \"\$(date +%s) \$(ps -o rss= -p \$PID)\"; \
      sleep 1; \
    done > /tmp/cpu/${label}_rss.txt 2>&1' &"
}

stop_monitor() {
  relay_cmd "pkill -f pidstat 2>/dev/null; \
    pkill -f mpstat 2>/dev/null"
  sleep 1
}

download_cpu() {
  local label=$1
  local dest=$2
  for suffix in pidstat mpstat rss; do
    relay_scp "/tmp/cpu/${label}_${suffix}.txt" \
      "${dest}/"
  done
}

# ---- HD worker stats -----------------------------------------

hd_worker_stats() {
  local label=$1
  local dest=$2
  relay_out "curl -s http://localhost:${HD_METRICS_PORT}\
/debug/workers" > "${dest}/${label}.json" || true
}

# ---- Rate run ------------------------------------------------

run_rate() {
  local label=$1 port=$2 rate=$3 tls_flag=$4 outfile=$5
  local tmpfile="${outfile}.tmp"

  local extra=""
  if [ "$tls_flag" = "tls" ]; then
    extra="--tls --insecure"
  fi

  if [ "$DRY_RUN" -eq 1 ]; then
    log "    [dry] derp-scale-test rate=${rate} → ${outfile}"
    echo '{"throughput_mbps":0,"message_loss_pct":0}' \
      > "$outfile"
    return
  fi

  derp-scale-test \
    --host "$RELAY" --port "$port" \
    --peers "$PEERS" --active-pairs "$PAIRS" \
    --duration "$DURATION" --msg-size "$SIZE" \
    --rate-mbps "$rate" \
    $extra \
    --json 2>/dev/null > "$tmpfile" || true

  sed -n '/{/,/}/p' "$tmpfile" > "$outfile"
  rm -f "$tmpfile"
}

# Extract throughput from JSON result file.
get_throughput() {
  python3 -c "
import json
try:
  d = json.load(open('$1'))
  print(f'{d[\"throughput_mbps\"]:.0f}')
except: print('0')
" 2>/dev/null
}

# Extract loss from JSON result file.
get_loss() {
  python3 -c "
import json
try:
  d = json.load(open('$1'))
  print(f'{d[\"message_loss_pct\"]:.1f}')
except: print('100.0')
" 2>/dev/null
}

# ---- Latency run ---------------------------------------------

run_latency_test() {
  local label=$1 port=$2 tls_flag=$3
  local bg_rate=$4  # 0 = idle
  local runs=$5 dest=$6

  # derp-test-client only supports --tls (no --insecure).
  # derp-scale-test supports both --tls and --insecure.
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
      --count $((PING_COUNT + PING_WARMUP + 500)) \
      --timeout 30000 \
      $tc_extra \
      >/dev/null 2>"$echo_err" &
    local echo_pid=$!
    sleep 2

    local echo_key
    echo_key=$(grep '^pubkey:' "$echo_err" | \
      awk '{print $2}' 2>/dev/null)
    if [[ -z "$echo_key" ]]; then
      log "    ERROR: no echo key for ${label} r${r}"
      kill $echo_pid 2>/dev/null || true
      wait $echo_pid 2>/dev/null || true
      continue
    fi

    # Start background load if non-idle.
    local load_pid=""
    if [ "$bg_rate" -gt 0 ]; then
      derp-scale-test \
        --host "$RELAY" --port "$port" \
        --peers "$PEERS" --active-pairs "$PAIRS" \
        --duration $((DURATION + 10)) --msg-size "$SIZE" \
        --rate-mbps "$bg_rate" \
        $st_extra \
        --json 2>/dev/null >/dev/null &
      load_pid=$!
      sleep 2  # Let load stabilize.
    fi

    # Run pinger.
    derp-test-client \
      --host "$RELAY" --port "$port" --mode ping \
      --count "$PING_COUNT" --size "$SIZE" \
      --dst-key "$echo_key" --warmup "$PING_WARMUP" \
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

# ---- TS ceiling probe ----------------------------------------

# Finds the TS TLS ceiling: first rate where loss > 5%.
# Returns ceiling rate on stdout.
run_ts_probe() {
  local port=$1 probe_rates=$2 dest=$3

  log "  TS TLS ceiling probe..."
  mkdir -p "$dest"

  local ceiling=0
  local prev_rate=0
  for rate in $probe_rates; do
    local loss_sum=0
    local loss_count=0
    for r in $(seq 1 $PROBE_RUNS); do
      local f="${dest}/ts_tls_${rate}_r$(printf '%02d' $r).json"
      run_rate "probe" "$port" "$rate" "tls" "$f"
      local loss
      loss=$(get_loss "$f")
      loss_sum=$(python3 -c "print(${loss_sum}+${loss})")
      loss_count=$((loss_count + 1))
      sleep 1
    done
    local avg_loss
    avg_loss=$(python3 -c \
      "print(f'{${loss_sum}/${loss_count}:.1f}')")
    log "    probe ${rate} Mbps: avg loss ${avg_loss}%"

    local over
    over=$(python3 -c "print(1 if ${avg_loss} > 5.0 else 0)")
    if [ "$over" -eq 1 ]; then
      ceiling=$prev_rate
      log "  TS TLS ceiling: ${ceiling} Mbps"
      echo "$ceiling"
      return
    fi
    prev_rate=$rate
  done
  # All probes passed — ceiling is highest rate.
  ceiling=$prev_rate
  log "  TS TLS ceiling: ${ceiling} Mbps (all passed)"
  echo "$ceiling"
}

# ---- Compute latency load levels ----------------------------

# Given TS ceiling, compute 6 load levels.
compute_lat_levels() {
  local ceiling=$1
  python3 -c "
c = int($ceiling)
levels = {
  'idle': 0,
  'ceil25': max(100, int(c * 0.25 / 100) * 100),
  'ceil50': max(100, int(c * 0.50 / 100) * 100),
  'ceil75': max(100, int(c * 0.75 / 100) * 100),
  'ceil100': c,
  'ceil150': max(c, int(c * 1.50 / 100) * 100),
}
for name, rate in levels.items():
  print(f'{name} {rate}')
"
}

# ---- Cert generation -----------------------------------------

generate_certs() {
  local cert_dir=$1
  relay_cmd "mkdir -p ${cert_dir}"
  # CN must be derp.tailscale.com — Go's TLS requires SNI
  # match.  SAN includes both the DNS name and relay IP so
  # the cert validates for either.
  relay_cmd "openssl req -x509 \
    -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
    -keyout ${cert_dir}/key.pem \
    -out ${cert_dir}/cert.pem \
    -days 1 -nodes \
    -subj '/CN=derp.tailscale.com' \
    -addext 'subjectAltName=DNS:derp.tailscale.com,IP:${RELAY}' \
    2>/dev/null"
  # Go derper -certmode manual expects:
  #   <certdir>/derp.tailscale.com.crt
  #   <certdir>/derp.tailscale.com.key
  relay_cmd "cp ${cert_dir}/cert.pem \
    ${cert_dir}/derp.tailscale.com.crt"
  relay_cmd "cp ${cert_dir}/key.pem \
    ${cert_dir}/derp.tailscale.com.key"
  log "  certs generated in ${cert_dir}"
}

# ---- System info ---------------------------------------------

collect_system_info() {
  local vcpu=$1 workers=$2 dest=$3
  {
    echo "test: phase_b_ktls_sweep"
    echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "vcpu: $vcpu"
    echo "workers: $workers"
    echo "relay: $RELAY"
    echo "peers: $PEERS"
    echo "pairs: $PAIRS"
    echo "duration: ${DURATION}s"
    echo "size: ${SIZE}B"
    echo "runs_low: $RUNS_LOW"
    echo "runs_high: $RUNS_HIGH"
    echo "ping_count: $PING_COUNT"
    echo "ping_warmup: $PING_WARMUP"
    relay_out "echo \"kernel: \$(uname -r)\""
    relay_out "echo \"cpu: \$(lscpu | grep 'Model name' | \
      sed 's/.*: *//')\""
    relay_out "hyper-derp --version 2>&1 || echo unknown"
    relay_out "go version -m \$(which derper) 2>&1 | \
      head -3 || echo unknown"
    relay_out "lsmod | grep tls || echo 'tls module: none'"
  } > "${dest}/system_info.txt"
}

# ---- kTLS stats ----------------------------------------------

collect_tls_stat() {
  local label=$1 dest=$2
  relay_out "cat /proc/net/tls_stat 2>/dev/null || \
    echo 'tls_stat: not available'" \
    > "${dest}/${label}.txt"
}

# ---- GC trace (TS only) -------------------------------------

run_ts_gc_trace() {
  local port=$1 rate=$2 dest=$3
  local cert_dir=${4:-/tmp/bench_certs}
  log "  TS GC trace at ${rate} Mbps..."
  stop_ts
  sleep 2

  relay_cmd "sudo GODEBUG=gctrace=1 setsid derper \
    -certmode manual -certdir ${cert_dir} \
    -a :${TS_PORT} -hostname derp.tailscale.com \
    </dev/null >/dev/null 2>/tmp/ts_gctrace.log &"
  sleep 4
  wait_port "$RELAY" "$TS_PORT" 10 || return 1

  local f="${dest}/ts_gctrace_rate.json"
  run_rate "ts_gc" "$port" "$rate" "tls" "$f"
  sleep 2

  relay_scp "/tmp/ts_gctrace.log" "${dest}/"
  stop_ts
  log "  GC trace done"
}

run_ts_gogcoff() {
  local port=$1 rate=$2 dest=$3
  local cert_dir=${4:-/tmp/bench_certs}
  log "  TS GOGC=off at ${rate} Mbps..."
  stop_ts
  sleep 2

  relay_cmd "sudo GOGC=off setsid derper \
    -certmode manual -certdir ${cert_dir} \
    -a :${TS_PORT} -hostname derp.tailscale.com \
    </dev/null >/dev/null 2>/tmp/ts_gogcoff.log &"
  sleep 4
  wait_port "$RELAY" "$TS_PORT" 10 || return 1

  local f="${dest}/ts_gogcoff.json"
  run_rate "ts_gogcoff" "$port" "$rate" "tls" "$f"
  sleep 2

  stop_ts
  log "  GOGC=off done"
}

# ---- Rate sweep (one relay) ---------------------------------

run_rate_sweep() {
  local label=$1 port=$2 tls_flag=$3 rates=$4
  local ts_ceiling=$5 dest=$6

  log "  ${label} rate sweep..."
  mkdir -p "$dest"

  for rate in $rates; do
    # More runs at high rates (above TS ceiling).
    local runs=$RUNS_LOW
    if [ "$rate" -gt "$ts_ceiling" ]; then
      runs=$RUNS_HIGH
    fi

    log "    ${rate} Mbps (${runs} runs)..."
    for r in $(seq -w 1 "$runs"); do
      local f="${dest}/${label}_${rate}_r${r}.json"
      run_rate "${label}_${rate}" "$port" "$rate" \
        "$tls_flag" "$f"
      sleep 1
    done

    # Summary for this rate.
    python3 -c "
import json, statistics
from pathlib import Path
tps = []
losses = []
for f in sorted(Path('${dest}').glob('${label}_${rate}_r*.json')):
  try:
    d = json.loads(f.read_text())
    tps.append(d['throughput_mbps'])
    losses.append(d['message_loss_pct'])
  except: pass
if tps:
  avg = statistics.mean(tps)
  cv = statistics.stdev(tps)/avg*100 if len(tps)>1 else 0
  avg_loss = statistics.mean(losses)
  print(f'    {avg:.0f} Mbps (CV {cv:.1f}%), '
        f'loss {avg_loss:.1f}%')
" 2>/dev/null || true
    sleep 2
  done
}

# ---- Latency suite (one relay) ------------------------------

run_latency_suite() {
  local label=$1 port=$2 tls_flag=$3
  local ts_ceiling=$4 dest=$5

  if [ "$SKIP_LATENCY" -eq 1 ]; then
    log "  ${label} latency: skipped"
    return
  fi

  log "  ${label} latency suite..."
  mkdir -p "$dest"

  # Compute load levels from TS ceiling.
  while IFS=' ' read -r level_name level_rate; do
    local runs=10
    # More runs at/above ceiling.
    if [ "$level_rate" -ge "$ts_ceiling" ] && \
       [ "$ts_ceiling" -gt 0 ]; then
      runs=15
    fi

    log "    ${level_name} (bg=${level_rate} Mbps, ${runs} runs)"
    run_latency_test \
      "${label}_${level_name}" "$port" "$tls_flag" \
      "$level_rate" "$runs" "$dest"

  done < <(compute_lat_levels "$ts_ceiling")
}

# ---- Per-config orchestration --------------------------------

run_config() {
  local vcpu=$1 workers=$2
  local out="${OUT_BASE}/${vcpu}vcpu"
  mkdir -p "$out"/{probe,rate,latency,cpu,workers,gc_trace}
  mkdir -p "$out"/tls_stat

  log ""
  log "============================================"
  log "  Config: ${vcpu} vCPU / ${workers} workers"
  log "============================================"

  local cert_dir="/tmp/bench_certs"
  generate_certs "$cert_dir"

  # Load tls kernel module.
  relay_cmd "sudo modprobe tls"
  local tls_loaded
  tls_loaded=$(relay_out "lsmod | grep -c '^tls '" \
    || echo 0)
  if [ "${tls_loaded:-0}" -lt 1 ]; then
    log "  WARNING: tls kernel module not loaded"
  fi

  collect_system_info "$vcpu" "$workers" "$out"

  # ---- Resolve TS ceiling (needed for latency levels) ----

  local ts_ceiling=0
  # Read ceiling from previous run if available.
  if [ -f "$out/system_info.txt" ]; then
    ts_ceiling=$(grep ts_tls_ceiling \
      "$out/system_info.txt" | awk '{print $2}')
    ts_ceiling=${ts_ceiling:-0}
  fi

  # ---- TS ----

  if [ "$SKIP_TS" -eq 0 ]; then
    start_ts "$cert_dir"
    if ! wait_port "$RELAY" "$TS_PORT" 10; then
      log "ERROR: TS failed to start at ${vcpu} vCPU"
      stop_ts
      return 1
    fi
    log "  TS derper started on :${TS_PORT}"

    if [ "$LATENCY_ONLY" -eq 0 ]; then
      # Ceiling probe.
      ts_ceiling=$(run_ts_probe "$TS_PORT" \
        "${PROBE_RATES[$vcpu]}" "$out/probe")
      echo "ts_tls_ceiling: ${ts_ceiling}" \
        >> "$out/system_info.txt"

      # Rate sweep.
      start_monitor "derper" "ts_rate"
      run_rate_sweep "ts" "$TS_PORT" "tls" \
        "${SWEEP_RATES[$vcpu]}" "$ts_ceiling" "$out/rate"
      stop_monitor
      download_cpu "ts_rate" "$out/cpu"
    fi

    # Latency suite.
    if [ "$ts_ceiling" -eq 0 ]; then
      # Fallback ceilings if no prior data.
      case $vcpu in
        2)  ts_ceiling=1500 ;;
        4)  ts_ceiling=2500 ;;
        8)  ts_ceiling=4500 ;;
        16) ts_ceiling=7500 ;;
      esac
      log "  Using fallback TS ceiling: ${ts_ceiling} Mbps"
    fi
    start_monitor "derper" "ts_lat"
    run_latency_suite "ts" "$TS_PORT" "tls" \
      "$ts_ceiling" "$out/latency"
    stop_monitor
    download_cpu "ts_lat" "$out/cpu"

    if [ "$LATENCY_ONLY" -eq 0 ]; then
      # GC trace + GOGC=off (single run at ceiling).
      if [ "$ts_ceiling" -gt 0 ]; then
        run_ts_gc_trace "$TS_PORT" "$ts_ceiling" \
          "$out/gc_trace" "$cert_dir"
        run_ts_gogcoff "$TS_PORT" "$ts_ceiling" \
          "$out/gc_trace" "$cert_dir"
      fi
    fi

    stop_ts
    drop_caches
    sleep 10
  else
    log "  TS: skipped"
    if [ "$ts_ceiling" -eq 0 ]; then
      # Default fallback ceilings per vCPU.
      case $vcpu in
        2)  ts_ceiling=1500 ;;
        4)  ts_ceiling=2500 ;;
        8)  ts_ceiling=4500 ;;
        16) ts_ceiling=7500 ;;
      esac
    fi
    log "  Using TS ceiling: ${ts_ceiling} Mbps"
  fi

  # ---- HD kTLS ----

  # Record tls_stat before.
  collect_tls_stat "hd_before" "$out/tls_stat"

  start_hd "$workers" "$cert_dir"
  if ! wait_port "$RELAY" "$HD_PORT" 10; then
    log "ERROR: HD failed to start at ${vcpu} vCPU"
    stop_hd
    return 1
  fi
  log "  HD kTLS started on :${HD_PORT}"

  # Worker stats before.
  hd_worker_stats "hd_before" "$out/workers"

  if [ "$LATENCY_ONLY" -eq 0 ]; then
    # Rate sweep.
    start_monitor "hyper-derp" "hd_rate"
    run_rate_sweep "hd" "$HD_PORT" "tls" \
      "${SWEEP_RATES[$vcpu]}" "$ts_ceiling" "$out/rate"
    stop_monitor
    download_cpu "hd_rate" "$out/cpu"

    # Worker stats after rate sweep.
    hd_worker_stats "hd_rate_after" "$out/workers"
  fi

  # Latency suite.
  start_monitor "hyper-derp" "hd_lat"
  run_latency_suite "hd" "$HD_PORT" "tls" \
    "$ts_ceiling" "$out/latency"
  stop_monitor
  download_cpu "hd_lat" "$out/cpu"

  # Worker stats after latency suite.
  hd_worker_stats "hd_lat_after" "$out/workers"

  stop_hd

  # Record tls_stat after.
  collect_tls_stat "hd_after" "$out/tls_stat"

  # Mark config complete.
  echo "complete $(date -u +%Y-%m-%dT%H:%M:%SZ)" \
    > "$out/DONE"
  log "  ${vcpu} vCPU config complete"
}

# ---- Validation ----------------------------------------------

validate_config() {
  local vcpu=$1
  local out="${OUT_BASE}/${vcpu}vcpu"

  python3 -c "
import json, statistics
from pathlib import Path

out = Path('${out}')
errors = []

# Count rate files.
rate_dir = out / 'rate'
rate_files = list(rate_dir.glob('*.json')) if rate_dir.exists() else []
valid = [f for f in rate_files if f.stat().st_size > 10]
print(f'  Rate files: {len(valid)} valid / {len(rate_files)} total')

for f in rate_files:
  if f.stat().st_size < 10:
    errors.append(f'Empty: {f.name}')

# Count latency files.
lat_dir = out / 'latency'
lat_files = list(lat_dir.glob('*.json')) if lat_dir.exists() else []
valid_lat = [f for f in lat_files if f.stat().st_size > 10]
print(f'  Latency files: {len(valid_lat)} valid / {len(lat_files)} total')

# Worker stats.
w_dir = out / 'workers'
w_files = list(w_dir.glob('*.json')) if w_dir.exists() else []
non_empty = [f for f in w_files if f.stat().st_size > 10]
print(f'  Worker stat files: {len(non_empty)} valid / {len(w_files)} total')
if len(w_files) > 0 and len(non_empty) == 0:
  errors.append('All worker stat files empty (P2 bug?)')

# CV check on rate sweep.
import re
combos = {}
for f in valid:
  m = re.match(r'(hd|ts)_(\d+)_r\d+\.json', f.name)
  if not m:
    continue
  key = f'{m.group(1)}_{m.group(2)}'
  combos.setdefault(key, []).append(f)

for key, files in sorted(combos.items()):
  tps = []
  for f in files:
    try:
      d = json.loads(f.read_text())
      tps.append(d['throughput_mbps'])
    except: pass
  if len(tps) > 2:
    avg = statistics.mean(tps)
    if avg > 0:
      cv = statistics.stdev(tps) / avg * 100
      if cv > 15:
        errors.append(f'High CV: {key} = {cv:.1f}%')
        print(f'  WARNING: {key} CV={cv:.1f}%')

# TLS stat.
tls_dir = out / 'tls_stat'
if tls_dir.exists():
  after = tls_dir / 'hd_after.txt'
  if after.exists():
    content = after.read_text()
    if 'TlsCurrTxSw' in content:
      for line in content.splitlines():
        if 'TlsCurrTxSw' in line:
          print(f'  tls_stat: {line.strip()}')

if errors:
  print(f'  ERRORS ({len(errors)}):')
  for e in errors:
    print(f'    - {e}')
else:
  print(f'  Validation: PASS')
" 2>/dev/null || log "  Validation script failed"
}

# ---- Main ----------------------------------------------------

main() {
  log "Phase B — kTLS Full Sweep"
  log "Output: $OUT_BASE"
  log "Configs: 16/8/4/2 vCPU (starting at ${START_AT})"
  log ""

  local started=0
  for cfg in "${CONFIGS[@]}"; do
    local vcpu workers
    read -r vcpu workers <<< "$cfg"

    # Skip configs outside range.
    if [ "$vcpu" -gt "$START_AT" ]; then
      log "Skipping ${vcpu} vCPU (--start-at ${START_AT})"
      continue
    fi
    if [ "$ONLY" -gt 0 ] && [ "$vcpu" -ne "$ONLY" ]; then
      continue
    fi

    run_config "$vcpu" "$workers"
    log ""
    log "=== Validation: ${vcpu} vCPU ==="
    validate_config "$vcpu"
    log ""
  done

  log ""
  log "=============================="
  log "  Phase B sweep complete"
  log "=============================="
  log "Results: $OUT_BASE"
}

main
