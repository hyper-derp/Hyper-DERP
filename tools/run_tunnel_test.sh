#!/bin/bash
# Bare Metal Tunnel Test — all 6 phases.
# Runs locally on Raptor Lake (relay), SSHes to hd-test01 (client).
# Implements bare-metal/TUNNEL_TEST_PLAN.md.
set -uo pipefail

CLIENT_SSH=hd-test01
CLIENT_USER=worker
HD_PORT=3341
TS_PORT=3340
CERT_DIR=/tmp/bench_certs_local
RUNS=10
DURATION=15

OUT_BASE=/tmp/bench_tunnel
mkdir -p "$OUT_BASE"

log() { echo "[$(date +%H:%M:%S)] $*" >&2; }

ccmd() {
  timeout "${3:-30}" ssh -o StrictHostKeyChecking=no \
    -o ConnectTimeout=10 -o LogLevel=ERROR \
    "${CLIENT_USER}@${CLIENT_SSH}" "$1" 2>/dev/null
}

ns_cmd() {
  local ns=$1; shift
  ccmd "sudo ip netns exec $ns $*"
}

ts_sock() { echo "/var/run/tailscale-${1}/tailscaled.sock"; }

# Pair IPs: pair N sender=ns-Na (100.64.0.{2N-1}),
#           receiver=ns-Nb (100.64.0.{2N})
recv_ip() { echo "100.64.0.$(($1 * 2))"; }
send_ns() { echo "ns-${1}a"; }
recv_ns() { echo "ns-${1}b"; }

# ---- Relay management ----------------------------------------

start_ts() {
  log "  Starting TS relay..."
  sudo pkill -9 derper 2>/dev/null; sleep 1
  sudo setsid derper -certmode manual -certdir "$CERT_DIR" \
    -a :${TS_PORT} -hostname 10.50.0.2 \
    </dev/null >/tmp/ts_tunnel.log 2>&1 &
  sleep 4
  log "  TS relay on :${TS_PORT}"
}

stop_ts() {
  sudo pkill -9 derper 2>/dev/null; sleep 2
}

start_hd() {
  local workers=${1:-4}
  log "  Starting HD relay (${workers}w)..."
  sudo pkill -9 hyper-derp 2>/dev/null; sleep 1
  sudo modprobe tls
  sudo setsid hyper-derp --port ${HD_PORT} \
    --workers $workers \
    --tls-cert ${CERT_DIR}/cert.pem \
    --tls-key ${CERT_DIR}/key.pem \
    --metrics-port 9090 --debug-endpoints \
    </dev/null >/tmp/hd_tunnel.log 2>&1 &
  sleep 4
  log "  HD relay on :${HD_PORT}"
}

stop_hd() {
  sudo pkill -9 hyper-derp 2>/dev/null; sleep 2
}

drop_caches() {
  sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
  sleep 2
}

# ---- Update DERP map port ------------------------------------

set_derp_port() {
  local port=$1
  local certname=""
  if [ "$port" = "3340" ]; then
    # TS derper generates its own cert
    certname=$(grep CertName /tmp/ts_tunnel.log 2>/dev/null \
      | grep -oP 'sha256-raw:[a-f0-9]+' | head -1)
  fi
  sudo tee /etc/headscale/derp-map.yaml > /dev/null << EOFDERP
regions:
  900:
    regionid: 900
    regioncode: "test"
    regionname: "Bare Metal"
    nodes:
      - name: "bm-relay"
        regionid: 900
        hostname: "10.50.0.2"
$([ -n "$certname" ] && echo "        certname: \"$certname\"")
        derpport: $port
        insecurefortests: true
EOFDERP
  sudo systemctl restart headscale
  sleep 5
  log "  DERP map updated to port $port"
}

# ---- Wait for DERP reconnection -----------------------------

wait_derp() {
  local pair=$1 max_wait=${2:-90}
  local ns
  ns=$(send_ns "$pair")
  local rip
  rip=$(recv_ip "$pair")
  local deadline=$((SECONDS + max_wait))
  while ((SECONDS < deadline)); do
    # Trigger DERP path with a ping.
    ns_cmd "$ns" "ping -c 1 -W 2 $rip >/dev/null 2>&1" "" 10
    local status
    status=$(ns_cmd "$ns" "tailscale \
      --socket=$(ts_sock "$ns") status 2>/dev/null" \
      | grep "pair${pair}-b" || true)
    if echo "$status" | grep -qE 'relay|active'; then
      log "  pair $pair: DERP OK"
      return 0
    fi
    sleep 3
  done
  log "  WARNING: pair $pair not on DERP after ${max_wait}s"
  return 1
}

# ---- CPU monitoring ------------------------------------------

start_cpu() {
  local label=$1
  mkdir -p "$OUT_BASE/cpu"
  nohup mpstat -P ALL 1 \
    > "$OUT_BASE/cpu/${label}_relay_mpstat.txt" 2>&1 &
  ccmd "nohup mpstat -P ALL 1 \
    > /tmp/cpu_${label}_client.txt 2>&1 &"
}

stop_cpu() {
  local label=$1
  pkill -f "mpstat.*-P ALL" 2>/dev/null || true
  ccmd "pkill -f mpstat 2>/dev/null"
  sleep 1
  scp -o StrictHostKeyChecking=no -o LogLevel=ERROR \
    "${CLIENT_USER}@${CLIENT_SSH}:/tmp/cpu_${label}_client.txt" \
    "$OUT_BASE/cpu/${label}_client_mpstat.txt" 2>/dev/null || true
}

# ---- iperf3 run (single pair) --------------------------------

run_iperf() {
  local pair=$1 streams=$2 duration=$3 outfile=$4
  local rns
  rns=$(recv_ns "$pair")
  local sns
  sns=$(send_ns "$pair")
  local rip
  rip=$(recv_ip "$pair")
  # Unique port per pair to avoid conflicts.
  local port=$((5200 + pair))

  # Kill any stale iperf3 in this namespace.
  ns_cmd "$rns" "pkill -9 -f 'iperf3.*-p $port' 2>/dev/null"
  sleep 0.5

  # Start iperf3 server in receiver namespace.
  ns_cmd "$rns" "iperf3 -s -1 -D --pidfile /tmp/iperf3_${rns}.pid \
    -p $port 2>/dev/null"
  sleep 1

  # Run client in sender namespace.
  local timeout_s=$((duration + 30))
  local result
  result=$(ccmd "sudo ip netns exec $sns iperf3 -c $rip -p $port \
    -t $duration -P $streams -J 2>/dev/null" "" "$timeout_s")

  echo "$result" > "$outfile"

  # Kill server unconditionally.
  ns_cmd "$rns" "pkill -9 -f 'iperf3.*-p $port' 2>/dev/null; \
    rm -f /tmp/iperf3_${rns}.pid"
}

# ---- ping run (single pair) ---------------------------------

run_ping() {
  local pair=$1 count=$2 interval=$3 outfile=$4
  local sns
  sns=$(send_ns "$pair")
  local rip
  rip=$(recv_ip "$pair")

  ns_cmd "$sns" "ping -c $count -i $interval $rip 2>&1" "" 60 \
    > "$outfile"
}

# ---- Extract metrics -----------------------------------------

extract_iperf() {
  python3 -c "
import json, sys
try:
  d = json.load(open('$1'))
  end = d['end']
  if 'sum_sent' in end:
    s = end['sum_sent']
  else:
    s = end['streams'][0]['sender']
  mbps = s['bits_per_second'] / 1e6
  retrans = s.get('retransmits', 0)
  print(f'{mbps:.0f} Mbps, {retrans} retransmits')
except Exception as e:
  print(f'parse error: {e}')
" 2>/dev/null
}

# ---- Phase runner --------------------------------------------

run_phase() {
  local phase=$1 pairs=$2 relay=$3 dest=$4
  local label="${relay}_phase${phase}"

  log "  Phase $phase ($pairs pairs, $relay)..."
  mkdir -p "$dest"

  start_cpu "$label"

  for run in $(seq -w 1 "$RUNS"); do
    log "    run $run/$RUNS"

    # tcp1: single stream per pair, all pairs simultaneous.
    local pids=()
    for p in $(seq 1 "$pairs"); do
      run_iperf "$p" 1 "$DURATION" \
        "${dest}/tcp1_pair${p}_r${run}.json" &
      pids+=($!)
    done
    for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null; done
    sleep 2

    # tcp4: 4 streams per pair, all pairs simultaneous.
    pids=()
    for p in $(seq 1 "$pairs"); do
      run_iperf "$p" 4 "$DURATION" \
        "${dest}/tcp4_pair${p}_r${run}.json" &
      pids+=($!)
    done
    for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null; done
    sleep 2

    # ping: 1000 pings per pair.
    pids=()
    for p in $(seq 1 "$pairs"); do
      run_ping "$p" 1000 0.01 \
        "${dest}/ping_pair${p}_r${run}.txt" &
      pids+=($!)
    done
    for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null; done
    sleep 2
  done

  stop_cpu "$label"

  # Summary.
  log "  Phase $phase summary ($relay):"
  for p in $(seq 1 "$pairs"); do
    local tcp1_file="${dest}/tcp1_pair${p}_r01.json"
    log "    pair $p tcp1: $(extract_iperf "$tcp1_file")"
  done
}

# ---- Phase 5: long duration ---------------------------------

run_phase5() {
  local relay=$1 dest=$2
  local label="${relay}_phase5"

  log "  Phase 5 (300s, all pairs, $relay)..."
  mkdir -p "$dest"

  start_cpu "$label"

  local pids=()
  for p in 1 2 3 4; do
    run_iperf "$p" 1 300 "${dest}/long_pair${p}.json" &
    pids+=($!)
  done
  for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null; done

  stop_cpu "$label"

  for p in 1 2 3 4; do
    log "    pair $p: $(extract_iperf "${dest}/long_pair${p}.json")"
  done
}

# ---- Phase 6: churn ------------------------------------------

run_phase6() {
  local relay=$1 dest=$2
  local label="${relay}_phase6"

  log "  Phase 6 (churn, $relay)..."
  mkdir -p "$dest"

  start_cpu "$label"

  # Pair 1: bulk transfer (60s).
  run_iperf 1 1 60 "${dest}/bulk_pair1.json" &
  local bulk_pid=$!

  # Pair 2: continuous ping.
  run_ping 2 3000 0.02 "${dest}/ping_pair2.txt" &
  local ping_pid=$!

  # Pair 3: connect/disconnect churn.
  local rns3
  rns3=$(recv_ns 3)
  local sns3
  sns3=$(send_ns 3)
  for i in $(seq 1 4); do
    sleep 12
    log "    churn cycle $i: reconnecting pair 3..."
    # Disconnect.
    ns_cmd "$sns3" "tailscale --socket=$(ts_sock "$sns3") down \
      2>/dev/null" || true
    sleep 3
    # Reconnect.
    ns_cmd "$sns3" "tailscale --socket=$(ts_sock "$sns3") up \
      --login-server http://10.50.0.2:8080 --accept-dns=false \
      2>/dev/null" || true
  done

  wait $bulk_pid 2>/dev/null
  wait $ping_pid 2>/dev/null

  stop_cpu "$label"

  log "    bulk: $(extract_iperf "${dest}/bulk_pair1.json")"
}

# ---- System info ---------------------------------------------

collect_system_info() {
  {
    echo "test: bare_metal_tunnel"
    echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "relay: Raptor Lake i5-13600KF (10.50.0.2)"
    echo "client: Haswell E5-1650 v3 (10.50.0.1)"
    echo "relay_kernel: $(uname -r)"
    echo "client_kernel: $(ccmd 'uname -r')"
    echo "pairs: 4"
    echo "runs: $RUNS"
    echo "duration: ${DURATION}s"
    echo "link: 25GbE SFP28 DAC"
    echo "hd_version: $(hyper-derp --version 2>&1)"
    echo "ts_version: $(derper --help 2>&1 | head -1)"
  } > "$OUT_BASE/system_info.txt"
}

# ---- Main ----------------------------------------------------

main() {
  log "Bare Metal Tunnel Test"
  log "Relay: Raptor Lake (local)"
  log "Client: Haswell (hd-test01)"
  log ""

  collect_system_info

  # ---- TS runs ----
  start_ts
  set_derp_port 3340
  sleep 10

  # Wait for all pairs to reconnect.
  for p in 1 2 3 4; do
    wait_derp "$p" 60
  done
  log "  All pairs connected via DERP (TS)"

  run_phase 1 1 ts "$OUT_BASE/phase1/ts"
  run_phase 2 2 ts "$OUT_BASE/phase2/ts"
  run_phase 3 3 ts "$OUT_BASE/phase3/ts"
  run_phase 4 4 ts "$OUT_BASE/phase4/ts"
  run_phase5 ts "$OUT_BASE/phase5_300s/ts"
  run_phase6 ts "$OUT_BASE/phase6_churn/ts"

  stop_ts
  drop_caches
  sleep 10

  # ---- HD runs ----
  start_hd 4
  set_derp_port 3341
  sleep 10

  for p in 1 2 3 4; do
    wait_derp "$p" 60
  done
  log "  All pairs connected via DERP (HD)"

  run_phase 1 1 hd "$OUT_BASE/phase1/hd"
  run_phase 2 2 hd "$OUT_BASE/phase2/hd"
  run_phase 3 3 hd "$OUT_BASE/phase3/hd"
  run_phase 4 4 hd "$OUT_BASE/phase4/hd"
  run_phase5 hd "$OUT_BASE/phase5_300s/hd"
  run_phase6 hd "$OUT_BASE/phase6_churn/hd"

  stop_hd

  log ""
  log "=============================="
  log "  Tunnel test complete"
  log "=============================="
  log "Results: $OUT_BASE"
}

main
