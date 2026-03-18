#!/bin/bash
# Haswell Bare Metal — Profiling & Diagnostics.
# Implements /home/karl/dev/HD-bench/HASWELL_PERF_PLAN.md.
# Relay = hd-test01 (Haswell), Client = ksys (Raptor Lake).
set -uo pipefail

RELAY=10.50.0.1
RELAY_SSH=hd-test01
RELAY_USER=worker
HD_PORT=3341
TS_PORT=3340
SIZE=1400
PEERS=20
PAIRS=10
DURATION=20
PERF_DURATION=15

OUT=/home/karl/dev/Hyper-DERP/bench_results/bare-metal-haswell/perf
mkdir -p "$OUT" "$OUT/tcpdump" "$OUT/tcp_comparison"

log() { echo "[$(date +%H:%M:%S)] $*" >&2; }

rcmd() {
  timeout "${2:-30}" ssh -o StrictHostKeyChecking=no \
    -o ConnectTimeout=10 -o LogLevel=ERROR \
    "${RELAY_USER}@${RELAY_SSH}" "$1" 2>/dev/null || true
}

rout() {
  timeout "${2:-30}" ssh -o StrictHostKeyChecking=no \
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

# ---- Server management (on relay) ---------------------------

start_hd() {
  local workers=$1 tls=${2:-tls}
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
}

stop_hd() {
  rcmd "sudo pkill -9 hyper-derp 2>/dev/null"
  sleep 2
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
}

stop_ts() {
  rcmd "sudo pkill -9 derper 2>/dev/null"
  sleep 2
}

drop_caches() {
  rcmd "sync; echo 3 | sudo tee \
    /proc/sys/vm/drop_caches >/dev/null"
  sleep 2
}

# ---- Bench client (local) -----------------------------------

run_bench() {
  local port=$1 rate=$2 tls_flag=$3 dur=${4:-$DURATION}
  local extra=""
  [ "$tls_flag" = "tls" ] && extra="--tls --insecure"
  derp-scale-test \
    --host "$RELAY" --port "$port" \
    --peers "$PEERS" --active-pairs "$PAIRS" \
    --duration "$dur" --msg-size "$SIZE" \
    --rate-mbps "$rate" \
    $extra \
    --json 2>/dev/null
}

run_bench_bg() {
  local port=$1 rate=$2 tls_flag=$3 dur=${4:-$DURATION}
  run_bench "$port" "$rate" "$tls_flag" "$dur" >/dev/null &
  echo $!
}

# ---- perf helpers (on relay via SSH) -------------------------

perf_stat() {
  local bin=$1 counters=$2 outfile=$3
  local pid
  pid=$(rout "pgrep -x $bin")
  if [ -z "$pid" ]; then
    log "  ERROR: $bin not running"
    return 1
  fi
  rout "sudo perf stat -e $counters \
    -p $pid -- sleep $PERF_DURATION 2>&1" > "$outfile"
}

perf_record() {
  local bin=$1 method=$2 outfile=$3
  local pid
  pid=$(rout "pgrep -x $bin")
  if [ -z "$pid" ]; then
    log "  ERROR: $bin not running"
    return 1
  fi
  rcmd "sudo perf record --call-graph $method \
    -o /tmp/perf_out.data \
    -p $pid -- sleep $PERF_DURATION" 60
  # Generate report.
  rout "sudo perf report -i /tmp/perf_out.data --stdio \
    --no-children 2>/dev/null" 120 > "${outfile%.perf.data}_report.txt"
  # Download perf.data.
  rscp "/tmp/perf_out.data" "$outfile"
  rcmd "sudo rm -f /tmp/perf_out.data"
}

# ---- A2: Flame Graph ----------------------------------------

run_flame_graph() {
  local config=$1 bin=$2 port=$3 rate=$4 tls=$5 workers=$6

  log "=== A2: Flame graph $config @ ${rate} Mbps ==="

  if [ "$bin" = "hyper-derp" ]; then
    start_hd "$workers" "$tls"
  else
    start_ts
  fi

  # Start bench client for duration + 10s headroom.
  local bg_pid
  bg_pid=$(run_bench_bg "$port" "$rate" "$tls" \
    $((PERF_DURATION + 10)))
  sleep 5  # Warmup.

  # perf record with LBR (try first, fall back to fp).
  log "  perf record (LBR)..."
  perf_record "$bin" lbr "$OUT/${config}_${rate}.perf.data"

  kill "$bg_pid" 2>/dev/null; wait "$bg_pid" 2>/dev/null

  if [ "$bin" = "hyper-derp" ]; then
    stop_hd
  else
    stop_ts
  fi
  drop_caches
}

# ---- A1: perf stat -------------------------------------------

run_perf_stat() {
  local config=$1 bin=$2 port=$3 rate=$4 tls=$5 workers=$6

  log "=== A1: perf stat $config @ ${rate} Mbps ==="

  if [ "$bin" = "hyper-derp" ]; then
    start_hd "$workers" "$tls"
  else
    start_ts
  fi

  # Pipeline counters.
  local bg_pid
  bg_pid=$(run_bench_bg "$port" "$rate" "$tls" \
    $((PERF_DURATION + 10)))
  sleep 3

  log "  Pipeline counters..."
  perf_stat "$bin" \
    "cycles,instructions,branches,branch-misses" \
    "$OUT/${config}_stat_pipeline_${rate}.txt"

  kill "$bg_pid" 2>/dev/null; wait "$bg_pid" 2>/dev/null
  sleep 3

  # Memory counters.
  bg_pid=$(run_bench_bg "$port" "$rate" "$tls" \
    $((PERF_DURATION + 10)))
  sleep 3

  log "  Memory counters..."
  perf_stat "$bin" \
    "L1-dcache-loads,L1-dcache-load-misses,LLC-loads,LLC-load-misses,LLC-stores,LLC-store-misses,dTLB-load-misses" \
    "$OUT/${config}_stat_memory_${rate}.txt"

  kill "$bg_pid" 2>/dev/null; wait "$bg_pid" 2>/dev/null
  sleep 3

  # Context / scheduling.
  bg_pid=$(run_bench_bg "$port" "$rate" "$tls" \
    $((PERF_DURATION + 10)))
  sleep 3

  log "  Context counters..."
  perf_stat "$bin" \
    "context-switches,cpu-migrations,cache-references,cache-misses" \
    "$OUT/${config}_stat_context_${rate}.txt"

  kill "$bg_pid" 2>/dev/null; wait "$bg_pid" 2>/dev/null

  if [ "$bin" = "hyper-derp" ]; then
    stop_hd
  else
    stop_ts
  fi
  drop_caches
}

# ---- B: TCP Segment Analysis ---------------------------------

run_tcpdump() {
  local rate=$1

  log "=== B: tcpdump + ss @ ${rate} Mbps ==="

  start_hd 4 tls

  local bg_pid
  bg_pid=$(run_bench_bg "$HD_PORT" "$rate" "tls" 20)
  sleep 2

  # tcpdump.
  rcmd "sudo tcpdump -i ens4f0np0 -w /tmp/relay_${rate}.pcap \
    -c 50000 port ${HD_PORT} 2>/dev/null &" 5

  # ss snapshots.
  rcmd "bash -c 'for i in \$(seq 1 15); do \
    echo \"=== t=\$i ===\"; ss -tin dst 10.50.0.2; sleep 1; \
    done > /tmp/ss_${rate}.txt'" 25

  kill "$bg_pid" 2>/dev/null; wait "$bg_pid" 2>/dev/null
  rcmd "sudo pkill tcpdump 2>/dev/null"
  sleep 2

  rscp "/tmp/relay_${rate}.pcap" "$OUT/tcpdump/"
  rscp "/tmp/ss_${rate}.txt" "$OUT/tcpdump/"

  stop_hd
  drop_caches
}

# ---- C: Plain TCP Comparison ---------------------------------

run_plain_tcp() {
  local workers=$1

  log "=== C: Plain TCP ${workers}w ==="

  start_hd "$workers" "notls"

  for rate in 3000 5000 7500 10000; do
    log "  ${rate} Mbps (10 runs)..."
    for r in $(seq -w 1 10); do
      local f="$OUT/tcp_comparison/hd_${workers}w_tcp_${rate}_r${r}.json"
      local tmp="${f}.tmp"
      run_bench "$HD_PORT" "$rate" "notls" 15 > "$tmp"
      sed -n '/{/,/}/p' "$tmp" > "$f"
      rm -f "$tmp"
      sleep 1
    done

    python3 -c "
import json, statistics
from pathlib import Path
tps, losses = [], []
for f in sorted(Path('$OUT/tcp_comparison').glob(
    'hd_${workers}w_tcp_${rate}_r*.json')):
  try:
    d = json.loads(f.read_text())
    tps.append(d['throughput_mbps'])
    losses.append(d['message_loss_pct'])
  except: pass
if tps:
  avg = statistics.mean(tps)
  cv = statistics.stdev(tps)/avg*100 if len(tps)>1 else 0
  print(f'    {avg:.0f} Mbps (CV {cv:.1f}%), '
        f'loss {statistics.mean(losses):.1f}%')
" 2>/dev/null
    sleep 2
  done

  # Perf stat at ceiling (detect from data).
  log "  perf stat at ceiling..."
  local bg_pid
  bg_pid=$(run_bench_bg "$HD_PORT" 7500 "notls" \
    $((PERF_DURATION + 10)))
  sleep 3

  local pid
  pid=$(rout "pgrep -x hyper-derp")
  if [ -n "$pid" ]; then
    rout "sudo perf stat -e \
      cycles,instructions,LLC-load-misses,cache-misses \
      -p $pid -- sleep $PERF_DURATION 2>&1" \
      > "$OUT/tcp_comparison/hd_${workers}w_tcp_stat_ceiling.txt"
  fi

  kill "$bg_pid" 2>/dev/null; wait "$bg_pid" 2>/dev/null
  stop_hd
  drop_caches
}

# ---- Main (execution order from plan) ------------------------

main() {
  log "Haswell Perf Profiling"
  log "Relay: hd-test01 (Haswell E5-1650 v3)"
  log "Client: ksys (Raptor Lake i5-13600KF)"
  log ""

  # Step 1-2: Flame graphs (highest priority).
  run_flame_graph "hd_2w" "hyper-derp" "$HD_PORT" 5000 tls 2
  run_flame_graph "hd_4w" "hyper-derp" "$HD_PORT" 10000 tls 4

  # Step 3-4: perf stat HD (high priority).
  run_perf_stat "hd_2w" "hyper-derp" "$HD_PORT" 5000 tls 2
  run_perf_stat "hd_4w" "hyper-derp" "$HD_PORT" 10000 tls 4

  # Step 5-6: TS flame graph + perf stat (medium).
  run_flame_graph "ts" "derper" "$TS_PORT" 5000 tls 0
  run_perf_stat "ts" "derper" "$TS_PORT" 3000 tls 0

  # Step 7: tcpdump + ss (medium).
  run_tcpdump 3000
  run_tcpdump 7500
  run_tcpdump 15000

  # Step 8: Plain TCP comparison (lower).
  run_plain_tcp 2
  run_plain_tcp 4

  log ""
  log "=============================="
  log "  Profiling complete"
  log "=============================="
  log "Results: $OUT"

  # Summary of key files.
  log ""
  log "Key outputs:"
  ls -la "$OUT"/*.perf.data 2>/dev/null | \
    awk '{print "  " $NF ": " $5 " bytes"}'
  ls -la "$OUT"/*_report*.txt 2>/dev/null | \
    awk '{print "  " $NF}'
}

main
