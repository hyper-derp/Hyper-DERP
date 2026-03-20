#!/bin/bash
# Bare Metal — Swapped Roles.
# Bare Metal -- Swapped Roles.
# Local machine = relay, remote ($CLIENT_SSH) = client.
# HD kTLS vs TS TLS, 4w and 8w configurations.
set -uo pipefail

# ---- Flags ---------------------------------------------------

ONLY=""
SKIP_TS=0
SKIP_LATENCY=0
LATENCY_ONLY=0
DRY_RUN=0

while [[ $# -gt 0 ]]; do
  case $1 in
    --only)          ONLY=$2; shift 2 ;;
    --skip-ts)       SKIP_TS=1; shift ;;
    --skip-latency)  SKIP_LATENCY=1; shift ;;
    --latency-only)  LATENCY_ONLY=1; shift ;;
    --dry-run)       DRY_RUN=1; shift ;;
    *)               echo "unknown flag: $1"; exit 1 ;;
  esac
done

# ---- Network ------------------------------------------------
# Relay = local ($RELAY_IP), Client = $CLIENT_SSH

RELAY_IP="${RELAY_IP:?Set RELAY_IP env var (relay IP address)}"
CLIENT_SSH="${CLIENT_SSH:?Set CLIENT_SSH env var (client hostname)}"
CLIENT_USER="${CLIENT_USER:-worker}"
HD_PORT=3341
TS_PORT=3340
HD_METRICS_PORT=9090
SIZE=1400
CERT_DIR=/tmp/bench_certs_local

# ---- Test params ---------------------------------------------

PEERS=20
PAIRS=10
DURATION=15
RUNS_LOW=5
RUNS_HIGH=25
PING_COUNT=5000
PING_WARMUP=500

# ---- Worker configs ------------------------------------------

CONFIGS=(
  "4w 4"
  "8w 8"
)

PROBE_RATES="1000 3000 5000 7500 10000 15000"
SWEEP_RATES="500 1000 2000 3000 5000 7500 10000 12500 15000 17500 20000 25000"

PROBE_RUNS=3

# ---- Output --------------------------------------------------

OUT_BASE=/tmp/bench_bare_metal_swapped
mkdir -p "$OUT_BASE"

# ---- Helpers -------------------------------------------------

log() { echo "[$(date +%H:%M:%S)] $*" >&2; }

client_cmd() {
  timeout 30 ssh -o StrictHostKeyChecking=no \
    -o ConnectTimeout=10 -o LogLevel=ERROR \
    "${CLIENT_USER}@${CLIENT_SSH}" "$1" 2>/dev/null || true
}

client_out() {
  timeout 60 ssh -o StrictHostKeyChecking=no \
    -o ConnectTimeout=10 -o LogLevel=ERROR \
    "${CLIENT_USER}@${CLIENT_SSH}" "$1" 2>/dev/null
}

wait_port() {
  local host=$1 port=$2 timeout=${3:-10}
  local deadline=$((SECONDS + timeout))
  while ! timeout 1 bash -c \
      "echo >/dev/tcp/$host/$port" 2>/dev/null; do
    ((SECONDS >= deadline)) && { log "ERROR: $host:$port not ready"; return 1; }
    sleep 0.5
  done
}

# ---- Server management (LOCAL) ------------------------------

start_hd() {
  local workers=$1
  log "  [local] starting HD kTLS (workers=$workers)..."
  sudo pkill -9 hyper-derp 2>/dev/null; sleep 1
  sudo modprobe tls
  sudo setsid hyper-derp --port ${HD_PORT} \
    --workers $workers \
    --metrics-port $HD_METRICS_PORT \
    --debug-endpoints \
    --tls-cert ${CERT_DIR}/cert.pem \
    --tls-key ${CERT_DIR}/key.pem \
    </dev/null >/tmp/hd.log 2>&1 &
  sleep 4
  local ok
  if ! wait_port "$RELAY_IP" "$HD_PORT" 10; then
    log "  WARNING: HD not listening, retrying..."
    sudo setsid hyper-derp --port ${HD_PORT} \
      --workers $workers \
      --metrics-port $HD_METRICS_PORT \
      --debug-endpoints \
      --tls-cert ${CERT_DIR}/cert.pem \
      --tls-key ${CERT_DIR}/key.pem \
      </dev/null >/tmp/hd.log 2>&1 &
    sleep 4
  fi
  sleep 2
  if grep -q 'kTLS' /tmp/hd.log 2>/dev/null; then
    log "  kTLS confirmed active"
  else
    log "  NOTE: kTLS status will be confirmed on first conn"
  fi
}

stop_hd() {
  sudo pkill -9 hyper-derp 2>/dev/null
  sleep 2
}

start_ts() {
  log "  [local] starting TS derper (TLS, -certmode manual)..."
  sudo pkill -9 derper 2>/dev/null; sleep 1
  sudo setsid derper \
    -certmode manual -certdir ${CERT_DIR} \
    -a :${TS_PORT} -hostname derp.tailscale.com \
    </dev/null >/tmp/ts.log 2>&1 &
  sleep 4
  local ok
  if ! wait_port "$RELAY_IP" "$TS_PORT" 10; then
    log "  WARNING: TS not listening, retrying..."
    sudo setsid derper \
      -certmode manual -certdir ${CERT_DIR} \
      -a :${TS_PORT} -hostname derp.tailscale.com \
      </dev/null >/tmp/ts.log 2>&1 &
    sleep 4
  fi
}

stop_ts() {
  sudo pkill -9 derper 2>/dev/null
  sleep 2
}

drop_caches() {
  sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
  sleep 2
}

# ---- CPU monitoring (both machines) -------------------------

start_monitor() {
  local server_bin=$1
  local label=$2
  mkdir -p /tmp/cpu
  # Relay (local) mpstat.
  nohup mpstat -P ALL 1 \
    > /tmp/cpu/${label}_relay_mpstat.txt 2>&1 &
  # Relay pidstat.
  nohup bash -c "PID=\$(pgrep -x $server_bin); \
    [ -n \"\$PID\" ] && pidstat -p \$PID -r -u 1 \
    > /tmp/cpu/${label}_relay_pidstat.txt 2>&1" &
  # Client mpstat.
  client_cmd "nohup mpstat -P ALL 1 \
    > /tmp/cpu/${label}_client_mpstat.txt 2>&1 &"
}

stop_monitor() {
  pkill -f "mpstat.*-P ALL" 2>/dev/null
  pkill -f pidstat 2>/dev/null
  client_cmd "pkill -f mpstat 2>/dev/null"
  sleep 1
}

download_cpu() {
  local label=$1
  local dest=$2
  cp /tmp/cpu/${label}_relay_mpstat.txt "${dest}/" 2>/dev/null
  cp /tmp/cpu/${label}_relay_pidstat.txt "${dest}/" 2>/dev/null
  scp -o StrictHostKeyChecking=no -o LogLevel=ERROR \
    "${CLIENT_USER}@${CLIENT_SSH}:/tmp/cpu/${label}_client_mpstat.txt" \
    "${dest}/" 2>/dev/null || true
}

# ---- HD worker stats -----------------------------------------

hd_worker_stats() {
  local label=$1
  local dest=$2
  curl -s "http://localhost:${HD_METRICS_PORT}/debug/workers" \
    > "${dest}/${label}.json" || true
}

# ---- Rate run (via SSH to client) ----------------------------

run_rate() {
  local label=$1 port=$2 rate=$3 tls_flag=$4 outfile=$5

  local extra=""
  if [ "$tls_flag" = "tls" ]; then
    extra="--tls --insecure"
  fi

  if [ "$DRY_RUN" -eq 1 ]; then
    log "    [dry] rate=${rate} -> ${outfile}"
    echo '{"throughput_mbps":0,"message_loss_pct":0}' \
      > "$outfile"
    return
  fi

  local raw
  raw=$(client_out "derp-scale-test \
    --host $RELAY_IP --port $port \
    --peers $PEERS --active-pairs $PAIRS \
    --duration $DURATION --msg-size $SIZE \
    --rate-mbps $rate \
    $extra \
    --json 2>/dev/null")

  echo "$raw" | sed -n '/{/,/}/p' > "$outfile"
}

get_throughput() {
  python3 -c "
import json
try:
  d = json.load(open('$1'))
  print(f'{d[\"throughput_mbps\"]:.0f}')
except: print('0')
" 2>/dev/null
}

get_loss() {
  python3 -c "
import json
try:
  d = json.load(open('$1'))
  print(f'{d[\"message_loss_pct\"]:.1f}')
except: print('100.0')
" 2>/dev/null
}

# ---- Latency run (via SSH to client) -------------------------

run_latency_test() {
  local label=$1 port=$2 tls_flag=$3
  local bg_rate=$4
  local runs=$5 dest=$6

  local tc_extra=""
  local st_extra=""
  if [ "$tls_flag" = "tls" ]; then
    tc_extra="--tls"
    st_extra="--tls --insecure"
  fi

  for r in $(seq -w 1 "$runs"); do
    log "    ${label} run ${r}/${runs}"
    local outfile="${dest}/${label}_r${r}.json"

    # Start echo server on client.
    client_cmd "nohup derp-test-client \
      --host $RELAY_IP --port $port --mode echo \
      --count $((PING_COUNT + PING_WARMUP + 500)) \
      --timeout 30000 \
      $tc_extra \
      >/dev/null 2>/tmp/echo_${label}_${r}.err &"
    sleep 2

    local echo_key
    echo_key=$(client_out \
      "grep '^pubkey:' /tmp/echo_${label}_${r}.err | \
       awk '{print \$2}'" 2>/dev/null)
    if [[ -z "$echo_key" ]]; then
      log "    ERROR: no echo key for ${label} r${r}"
      client_cmd "pkill -f 'derp-test-client.*echo' 2>/dev/null"
      continue
    fi

    # Start background load on client.
    local load_started=0
    if [ "$bg_rate" -gt 0 ]; then
      client_cmd "nohup derp-scale-test \
        --host $RELAY_IP --port $port \
        --peers $PEERS --active-pairs $PAIRS \
        --duration $((DURATION + 10)) --msg-size $SIZE \
        --rate-mbps $bg_rate \
        $st_extra \
        --json 2>/dev/null >/dev/null &"
      load_started=1
      sleep 2
    fi

    # Run pinger on client, capture output.
    client_out "derp-test-client \
      --host $RELAY_IP --port $port --mode ping \
      --count $PING_COUNT --size $SIZE \
      --dst-key $echo_key --warmup $PING_WARMUP \
      --json --raw-latency \
      --label $label \
      --timeout 30000 \
      $tc_extra 2>/dev/null" > "$outfile" || true

    # Cleanup.
    if [ "$load_started" -eq 1 ]; then
      client_cmd "pkill -f derp-scale-test 2>/dev/null"
    fi
    client_cmd "pkill -f 'derp-test-client.*echo' 2>/dev/null"
    sleep 1
  done
}

# ---- TS ceiling probe ----------------------------------------

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
  ceiling=$prev_rate
  log "  TS TLS ceiling: ${ceiling} Mbps (all passed)"
  echo "$ceiling"
}

# ---- Compute latency load levels ----------------------------

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

# ---- System info ---------------------------------------------

collect_system_info() {
  local label=$1 workers=$2 dest=$3
  {
    echo "test: bare_metal_swapped_ktls"
    echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "config: $label"
    echo "workers: $workers"
    echo "relay: $RELAY_IP (local)"
    echo "client: $CLIENT_SSH"
    echo "peers: $PEERS"
    echo "pairs: $PAIRS"
    echo "duration: ${DURATION}s"
    echo "size: ${SIZE}B"
    echo "runs_low: $RUNS_LOW"
    echo "runs_high: $RUNS_HIGH"
    echo "ping_count: $PING_COUNT"
    echo "ping_warmup: $PING_WARMUP"
    echo "relay_kernel: $(uname -r)"
    echo "relay_cpu: $(lscpu | grep 'Model name' | \
      sed 's/.*: *//')"
    echo "$(hyper-derp --version 2>&1 || echo unknown)"
    client_out "echo \"client_kernel: \$(uname -r)\""
    client_out "echo \"client_cpu: \$(lscpu | \
      grep 'Model name' | sed 's/.*: *//')\""
    echo "$(lsmod | grep tls || echo 'tls module: none')"
    echo "relay_nic_speed: $(ethtool enp3s0f0np0 2>/dev/null | \
      grep Speed | sed 's/.*: *//' || echo unknown)"
  } > "${dest}/system_info.txt"
}

collect_tls_stat() {
  local label=$1 dest=$2
  cat /proc/net/tls_stat 2>/dev/null \
    > "${dest}/${label}.txt" || \
    echo "tls_stat: not available" > "${dest}/${label}.txt"
}

# ---- GC trace ------------------------------------------------

run_ts_gc_trace() {
  local port=$1 rate=$2 dest=$3
  log "  TS GC trace at ${rate} Mbps..."
  stop_ts; sleep 2

  sudo GODEBUG=gctrace=1 setsid derper \
    -certmode manual -certdir ${CERT_DIR} \
    -a :${TS_PORT} -hostname derp.tailscale.com \
    </dev/null >/dev/null 2>/tmp/ts_gctrace.log &
  sleep 4
  wait_port "$RELAY_IP" "$TS_PORT" 10 || return 1

  local f="${dest}/ts_gctrace_rate.json"
  run_rate "ts_gc" "$port" "$rate" "tls" "$f"
  sleep 2

  cp /tmp/ts_gctrace.log "${dest}/"
  stop_ts
  log "  GC trace done"
}

run_ts_gogcoff() {
  local port=$1 rate=$2 dest=$3
  log "  TS GOGC=off at ${rate} Mbps..."
  stop_ts; sleep 2

  sudo GOGC=off setsid derper \
    -certmode manual -certdir ${CERT_DIR} \
    -a :${TS_PORT} -hostname derp.tailscale.com \
    </dev/null >/dev/null 2>/tmp/ts_gogcoff.log &
  sleep 4
  wait_port "$RELAY_IP" "$TS_PORT" 10 || return 1

  local f="${dest}/ts_gogcoff.json"
  run_rate "ts_gogcoff" "$port" "$rate" "tls" "$f"
  sleep 2

  stop_ts
  log "  GOGC=off done"
}

# ---- Rate sweep ----------------------------------------------

run_rate_sweep() {
  local label=$1 port=$2 tls_flag=$3 rates=$4
  local ts_ceiling=$5 dest=$6

  log "  ${label} rate sweep..."
  mkdir -p "$dest"

  for rate in $rates; do
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

# ---- Latency suite -------------------------------------------

run_latency_suite() {
  local label=$1 port=$2 tls_flag=$3
  local ts_ceiling=$4 dest=$5

  if [ "$SKIP_LATENCY" -eq 1 ]; then
    log "  ${label} latency: skipped"
    return
  fi

  log "  ${label} latency suite..."
  mkdir -p "$dest"

  while IFS=' ' read -r level_name level_rate; do
    local runs=10
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
  local label=$1 workers=$2
  local out="${OUT_BASE}/${label}_ktls"
  mkdir -p "$out"/{probe,rate,latency,cpu,workers,gc_trace}
  mkdir -p "$out"/tls_stat

  log ""
  log "============================================"
  log "  Config: ${label} (${workers} workers)"
  log "  Relay: $RELAY_IP (local)"
  log "  Client: $CLIENT_SSH"
  log "============================================"

  collect_system_info "$label" "$workers" "$out"

  local ts_ceiling=0
  if [ -f "$out/system_info.txt" ]; then
    ts_ceiling=$(grep ts_tls_ceiling \
      "$out/system_info.txt" | awk '{print $2}')
    ts_ceiling=${ts_ceiling:-0}
  fi

  # ---- TS ----

  if [ "$SKIP_TS" -eq 0 ]; then
    start_ts
    if ! wait_port "$RELAY_IP" "$TS_PORT" 10; then
      log "ERROR: TS failed to start"
      stop_ts; return 1
    fi
    log "  TS derper started on :${TS_PORT}"

    if [ "$LATENCY_ONLY" -eq 0 ]; then
      ts_ceiling=$(run_ts_probe "$TS_PORT" \
        "$PROBE_RATES" "$out/probe")
      echo "ts_tls_ceiling: ${ts_ceiling}" \
        >> "$out/system_info.txt"

      start_monitor "derper" "ts_rate"
      run_rate_sweep "ts" "$TS_PORT" "tls" \
        "$SWEEP_RATES" "$ts_ceiling" "$out/rate"
      stop_monitor
      download_cpu "ts_rate" "$out/cpu"
    fi

    if [ "$ts_ceiling" -eq 0 ]; then
      ts_ceiling=7500
      log "  Using fallback TS ceiling: ${ts_ceiling} Mbps"
    fi
    start_monitor "derper" "ts_lat"
    run_latency_suite "ts" "$TS_PORT" "tls" \
      "$ts_ceiling" "$out/latency"
    stop_monitor
    download_cpu "ts_lat" "$out/cpu"

    if [ "$LATENCY_ONLY" -eq 0 ]; then
      if [ "$ts_ceiling" -gt 0 ]; then
        run_ts_gc_trace "$TS_PORT" "$ts_ceiling" \
          "$out/gc_trace"
        run_ts_gogcoff "$TS_PORT" "$ts_ceiling" \
          "$out/gc_trace"
      fi
    fi

    stop_ts
    drop_caches
    sleep 10
  else
    log "  TS: skipped"
    if [ "$ts_ceiling" -eq 0 ]; then
      ts_ceiling=7500
    fi
    log "  Using TS ceiling: ${ts_ceiling} Mbps"
  fi

  # ---- HD kTLS ----

  collect_tls_stat "hd_before" "$out/tls_stat"

  start_hd "$workers"
  if ! wait_port "$RELAY_IP" "$HD_PORT" 10; then
    log "ERROR: HD failed to start"
    stop_hd; return 1
  fi
  log "  HD kTLS started on :${HD_PORT}"

  hd_worker_stats "hd_before" "$out/workers"

  if [ "$LATENCY_ONLY" -eq 0 ]; then
    start_monitor "hyper-derp" "hd_rate"
    run_rate_sweep "hd" "$HD_PORT" "tls" \
      "$SWEEP_RATES" "$ts_ceiling" "$out/rate"
    stop_monitor
    download_cpu "hd_rate" "$out/cpu"

    hd_worker_stats "hd_rate_after" "$out/workers"
  fi

  start_monitor "hyper-derp" "hd_lat"
  run_latency_suite "hd" "$HD_PORT" "tls" \
    "$ts_ceiling" "$out/latency"
  stop_monitor
  download_cpu "hd_lat" "$out/cpu"

  hd_worker_stats "hd_lat_after" "$out/workers"

  stop_hd
  collect_tls_stat "hd_after" "$out/tls_stat"

  echo "complete $(date -u +%Y-%m-%dT%H:%M:%SZ)" \
    > "$out/DONE"
  log "  ${label} config complete"
}

# ---- Main ----------------------------------------------------

main() {
  log "Bare Metal -- Swapped Roles"
  log "Output: $OUT_BASE"
  log "Relay: $RELAY_IP (local)"
  log "Client: $CLIENT_SSH"
  log ""

  # Ensure client has cpu dir.
  client_cmd "mkdir -p /tmp/cpu"

  for cfg in "${CONFIGS[@]}"; do
    local label workers
    read -r label workers <<< "$cfg"

    if [ -n "$ONLY" ] && [ "$label" != "$ONLY" ]; then
      log "Skipping ${label} (--only ${ONLY})"
      continue
    fi

    run_config "$label" "$workers"

    # Skip TS for second config.
    SKIP_TS=1
    log ""
  done

  log ""
  log "=============================="
  log "  Bare metal sweep complete"
  log "=============================="
  log "Results: $OUT_BASE"
}

main
